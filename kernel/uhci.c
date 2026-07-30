#include "uhci.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "string.h"

/* UHCI I/O port register offsets (Intel UHCI spec), all relative to the
 * controller's I/O-space BAR (BAR4). Pure PIO, no MMIO -- matches this
 * kernel's existing PCI drivers (kernel/ata.c). */
#define UHCI_REG_USBCMD     0x00 /* 16-bit */
#define UHCI_REG_USBSTS     0x02 /* 16-bit */
#define UHCI_REG_USBINTR    0x04 /* 16-bit */
#define UHCI_REG_FRNUM      0x06 /* 16-bit */
#define UHCI_REG_FRBASEADD  0x08 /* 32-bit */
#define UHCI_REG_SOFMOD     0x0C /* 8-bit */
#define UHCI_REG_PORTSC1    0x10 /* 16-bit */
#define UHCI_REG_PORTSC2    0x12 /* 16-bit */

#define UHCI_CMD_RUN        0x0001
#define UHCI_CMD_HCRESET    0x0002
#define UHCI_CMD_GRESET     0x0004
#define UHCI_CMD_CF         0x0040
#define UHCI_CMD_MAXP64     0x0080

#define UHCI_PORTSC_CCS     0x0001 /* current connect status */
#define UHCI_PORTSC_CSC     0x0002 /* connect status change */
#define UHCI_PORTSC_PE      0x0004 /* port enable */
#define UHCI_PORTSC_LS_MASK 0x0030 /* line status */
#define UHCI_PORTSC_PR      0x0200 /* port reset */
#define UHCI_PORTSC_LSDA    0x0100 /* low speed device attached */
#define UHCI_PORTSC_RSVD1   0x0400 /* always reads as 1 */

#define UHCI_FRAME_COUNT    1024
#define UHCI_TD_TERMINATE   0x00000001u

#define UHCI_PORT_COUNT 2

typedef struct {
    uint16_t io_base;
    int present;
    uint32_t *frame_list;
    int port_connected[UHCI_PORT_COUNT];
    int port_enabled[UHCI_PORT_COUNT];
    int port_low_speed[UHCI_PORT_COUNT];
} uhci_controller_t;

static uhci_controller_t uhci;
static char uhci_status_buf[256];

static void append_str(char *out, uint32_t *pos, uint32_t max, const char *text) {
    while (text && *text && *pos + 1 < max) {
        out[*pos] = *text;
        (*pos)++;
        text++;
    }
    out[*pos] = '\0';
}

static void append_hex_digit(char *out, uint32_t *pos, uint32_t max, uint8_t value) {
    char c = value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
    if (*pos + 1 < max) {
        out[*pos] = c;
        (*pos)++;
        out[*pos] = '\0';
    }
}

static void append_hex8(char *out, uint32_t *pos, uint32_t max, uint8_t value) {
    append_hex_digit(out, pos, max, (uint8_t)((value >> 4) & 0x0F));
    append_hex_digit(out, pos, max, (uint8_t)(value & 0x0F));
}

static void append_hex16(char *out, uint32_t *pos, uint32_t max, uint16_t value) {
    append_hex8(out, pos, max, (uint8_t)((value >> 8) & 0xFF));
    append_hex8(out, pos, max, (uint8_t)(value & 0xFF));
}

/* storage_init() runs before the scheduler's first task switch enables
 * interrupts (see kernel/task.c: EFLAGS pushed with IF=1 for the initial
 * IRET), so timer_get_ticks() never advances here -- a timer-based wait
 * would spin forever. kernel/ata.c hits the same constraint and solves it
 * with pure I/O-port busy-waits; do the same here using the classic
 * port-0x80 "diagnostic port" delay trick. */
static void uhci_io_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) {
        inb(0x80);
    }
}

/* Resets and enables one port, recording connect/enable/speed state.
 * Follows the standard UHCI port-reset sequence: assert PR for >=50ms,
 * clear it, allow >=10ms recovery, then set PE and confirm. */
static void uhci_probe_port(int port) {
    uint16_t reg = (uint16_t)(UHCI_REG_PORTSC1 + port * 2);
    uint16_t status = inw((uint16_t)(uhci.io_base + reg));

    if (!(status & UHCI_PORTSC_CCS)) {
        uhci.port_connected[port] = 0;
        uhci.port_enabled[port] = 0;
        uhci.port_low_speed[port] = 0;
        return;
    }

    uhci.port_connected[port] = 1;

    /* Assert reset for >=50ms (UHCI spec). */
    outw((uint16_t)(uhci.io_base + reg), (uint16_t)(status | UHCI_PORTSC_PR));
    uhci_io_delay(50000);
    status = inw((uint16_t)(uhci.io_base + reg));
    outw((uint16_t)(uhci.io_base + reg), (uint16_t)(status & ~UHCI_PORTSC_PR));

    /* Recovery interval before the port is usable. */
    uhci_io_delay(10000);

    status = inw((uint16_t)(uhci.io_base + reg));
    if (!(status & UHCI_PORTSC_CCS)) {
        /* Device went away during reset (or was never really there). */
        uhci.port_connected[port] = 0;
        uhci.port_enabled[port] = 0;
        uhci.port_low_speed[port] = 0;
        return;
    }

    uhci.port_low_speed[port] = (status & UHCI_PORTSC_LSDA) ? 1 : 0;

    /* Enable the port. */
    outw((uint16_t)(uhci.io_base + reg), (uint16_t)(status | UHCI_PORTSC_PE));
    uhci_io_delay(2000);
    status = inw((uint16_t)(uhci.io_base + reg));
    uhci.port_enabled[port] = (status & UHCI_PORTSC_PE) ? 1 : 0;
}

static int uhci_find_controller(uint16_t *out_io_base) {
    int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (dev->class_code == 0x0C && dev->subclass == 0x03 && dev->prog_if == 0x00) {
            /* BAR4 is I/O space for UHCI; bit 0 marks I/O space, low 2 bits are flags. */
            *out_io_base = (uint16_t)(dev->bar[4] & 0xFFFC);
            return 1;
        }
    }
    return 0;
}

int uhci_init(void) {
    memset(&uhci, 0, sizeof(uhci));

    uint16_t io_base;
    if (!uhci_find_controller(&io_base)) {
        return -1;
    }
    uhci.io_base = io_base;

    /* Global reset: clears controller state, hold briefly then release. */
    outw((uint16_t)(uhci.io_base + UHCI_REG_USBCMD), UHCI_CMD_GRESET);
    uhci_io_delay(10000);
    outw((uint16_t)(uhci.io_base + UHCI_REG_USBCMD), 0);
    uhci_io_delay(10000);

    /* Frame list: 1024 32-bit pointers, must be 4KB-aligned. pmm_alloc_block()
     * hands out single PAGE_SIZE (4096) blocks, already aligned by
     * construction (bitmap allocator works in page units), same assumption
     * every other PMM consumer in this kernel already relies on. */
    uhci.frame_list = (uint32_t *)pmm_alloc_block();
    if (!uhci.frame_list) {
        uhci.present = 0;
        return -1;
    }
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        uhci.frame_list[i] = UHCI_TD_TERMINATE; /* no queue heads scheduled yet (Etapa 2) */
    }

    outl((uint16_t)(uhci.io_base + UHCI_REG_FRBASEADD), (uint32_t)(uintptr_t)uhci.frame_list);
    outw((uint16_t)(uhci.io_base + UHCI_REG_FRNUM), 0);
    outw((uint16_t)(uhci.io_base + UHCI_REG_USBINTR), 0); /* polling only, no IRQs */

    outw((uint16_t)(uhci.io_base + UHCI_REG_USBCMD), UHCI_CMD_RUN | UHCI_CMD_CF | UHCI_CMD_MAXP64);
    uhci.present = 1;

    for (int p = 0; p < UHCI_PORT_COUNT; p++) {
        uhci_probe_port(p);
    }

    return 0;
}

const char *uhci_status_string(void) {
    uint32_t pos = 0;
    memset(uhci_status_buf, 0, sizeof(uhci_status_buf));

    if (!uhci.present) {
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "USB: no UHCI controller found.\n");
        return uhci_status_buf;
    }

    append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "USB: UHCI controller at I/O base 0x");
    append_hex16(uhci_status_buf, &pos, sizeof(uhci_status_buf), uhci.io_base);
    append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "\n");

    for (int p = 0; p < UHCI_PORT_COUNT; p++) {
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "  port ");
        append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf), (uint8_t)p);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ": ");
        if (!uhci.port_connected[p]) {
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "no device\n");
            continue;
        }
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "device connected, ");
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), uhci.port_enabled[p] ? "enabled" : "NOT enabled");
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ", ");
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), uhci.port_low_speed[p] ? "low-speed" : "full-speed");
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "\n");
    }

    return uhci_status_buf;
}
