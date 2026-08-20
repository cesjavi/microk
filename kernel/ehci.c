#include "ehci.h"
#include "pci.h"
#include "io.h"
#include "string.h"

/* EHCI (USB 2.0) host controller registers -- capability registers are
 * fixed at BAR0+0, operational registers start at BAR0+CAPLENGTH (spec
 * section 2.2/2.3). All MMIO, unlike UHCI's pure I/O-port BAR4, so
 * registers are accessed via volatile pointers straight off the BAR
 * physical address -- same convention kernel/net.c already uses for the
 * e1000's BAR0 (this kernel's identity mapping covers the low physical
 * range QEMU/real chipsets place PCI MMIO BARs in, so no explicit
 * vmm_map_page call is needed here either). */

#define EHCI_MAX_PORTS 15 /* HCSPARAMS N_PORTS is a 4-bit field */

#define EHCI_USBCMD_RS       0x00000001u /* Run/Stop */
#define EHCI_USBCMD_HCRESET  0x00000002u
#define EHCI_USBSTS_HCHALTED 0x00001000u
#define EHCI_CONFIGFLAG_CF   0x00000001u

#define EHCI_PORTSC_CCS  0x00000001u /* Current Connect Status */
#define EHCI_PORTSC_CSC  0x00000002u /* Connect Status Change */
#define EHCI_PORTSC_PED  0x00000004u /* Port Enabled */
#define EHCI_PORTSC_PR   0x00000100u /* Port Reset */
#define EHCI_PORTSC_PO   0x00002000u /* Port Owner (1 = released to companion controller) */

/* USBLEGSUP (EHCI Extended Capability ID 1), found at PCI config offset
 * HCCPARAMS.EECP when nonzero -- BIOS/legacy USB handoff protocol. */
#define EHCI_LEGSUP_CAPID_MASK  0x000000FFu
#define EHCI_LEGSUP_CAPID_LEGSUP 0x01u
#define EHCI_LEGSUP_OS_OWNED    0x00010000u
#define EHCI_LEGSUP_BIOS_OWNED  0x01000000u

typedef struct {
    int present;
    uint32_t mmio_base;
    uint32_t op_base;
    int n_ports;
    int port_connected[EHCI_MAX_PORTS];
    int port_enabled[EHCI_MAX_PORTS];
    int port_owned_by_companion[EHCI_MAX_PORTS];
} ehci_controller_t;

static ehci_controller_t ehci;
static char ehci_status_buf[512];

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

static void append_hex32(char *out, uint32_t *pos, uint32_t max, uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        append_hex_digit(out, pos, max, (uint8_t)((value >> shift) & 0x0F));
    }
}

/* storage_init() runs before the scheduler's first task switch enables
 * interrupts, so timer_get_ticks() never advances here -- same constraint
 * kernel/uhci.c and kernel/ata.c already document and solve with a pure
 * I/O-port busy-wait via the port-0x80 diagnostic-port delay trick. */
static void ehci_io_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) {
        inb(0x80);
    }
}

static volatile uint32_t *ehci_op_reg(uint32_t offset) {
    return (volatile uint32_t *)(uintptr_t)(ehci.op_base + offset);
}

static volatile uint32_t *ehci_portsc(int port) {
    return ehci_op_reg(0x44 + (uint32_t)port * 4);
}

static int ehci_find_controller(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func, uint32_t *out_bar0) {
    int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (dev->class_code == 0x0C && dev->subclass == 0x03 && dev->prog_if == 0x20) {
            *out_bus = dev->bus;
            *out_slot = dev->slot;
            *out_func = dev->function;
            *out_bar0 = dev->bar[0] & 0xFFFFFFF0u;
            return 1;
        }
    }
    return 0;
}

/* Legacy BIOS handoff (EHCI spec 5.1): if the controller advertises a
 * Legacy Support extended capability, claim ownership by setting the OS
 * Owned bit and waiting for the BIOS Owned bit to clear. Bounded by
 * iteration count, not a timer, for the same reason as ehci_io_delay --
 * if a real BIOS never releases ownership this gives up rather than
 * hanging the boot forever. Most QEMU EHCI models don't implement this
 * capability at all (EECP reads 0), in which case this is a no-op. */
static void ehci_bios_handoff(uint8_t bus, uint8_t slot, uint8_t func, uint32_t hccparams) {
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFF);
    if (eecp < 0x40) {
        return; /* no extended capabilities list */
    }

    uint32_t legsup = pci_config_read32(bus, slot, func, eecp);
    if ((legsup & EHCI_LEGSUP_CAPID_MASK) != EHCI_LEGSUP_CAPID_LEGSUP) {
        return; /* first capability isn't Legacy Support */
    }

    pci_config_write32(bus, slot, func, eecp, legsup | EHCI_LEGSUP_OS_OWNED);

    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        legsup = pci_config_read32(bus, slot, func, eecp);
        if (!(legsup & EHCI_LEGSUP_BIOS_OWNED)) {
            return;
        }
        ehci_io_delay(1000);
    }
    /* Gave up waiting for the BIOS to release ownership -- proceed anyway
     * (matches how real OSes behave: log-worthy, not fatal). */
}

static void ehci_probe_port(int port) {
    volatile uint32_t *portsc = ehci_portsc(port);
    uint32_t status = *portsc;

    if (!(status & EHCI_PORTSC_CCS)) {
        ehci.port_connected[port] = 0;
        ehci.port_enabled[port] = 0;
        ehci.port_owned_by_companion[port] = 0;
        return;
    }

    ehci.port_connected[port] = 1;

    /* Assert reset for >=50ms (EHCI spec 2.3.9), then clear it. */
    *portsc = (status & ~EHCI_PORTSC_PED) | EHCI_PORTSC_PR;
    ehci_io_delay(50000);
    status = *portsc;
    *portsc = status & ~EHCI_PORTSC_PR;
    ehci_io_delay(2000);

    status = *portsc;
    if (!(status & EHCI_PORTSC_CCS)) {
        /* Device went away during reset. */
        ehci.port_connected[port] = 0;
        ehci.port_enabled[port] = 0;
        ehci.port_owned_by_companion[port] = 0;
        return;
    }

    if (status & EHCI_PORTSC_PED) {
        /* Negotiated high-speed -- EHCI keeps ownership. */
        ehci.port_enabled[port] = 1;
        ehci.port_owned_by_companion[port] = 0;
        return;
    }

    /* Not high-speed: release the port to the companion UHCI/OHCI
     * controller (EHCI spec 4.2.2), which owns full/low-speed devices.
     * The companion driver (kernel/uhci.c) probes its own ports
     * independently and will pick this device up there. */
    ehci.port_enabled[port] = 0;
    ehci.port_owned_by_companion[port] = 1;
    status = *portsc;
    *portsc = status | EHCI_PORTSC_PO;
}

int ehci_init(void) {
    memset(&ehci, 0, sizeof(ehci));

    uint8_t bus, slot, func;
    uint32_t bar0;
    if (!ehci_find_controller(&bus, &slot, &func, &bar0)) {
        return -1;
    }
    ehci.mmio_base = bar0;

    /* Memory Space Enable (bit1) + Bus Master Enable (bit2), same
     * reasoning as kernel/uhci.c's I/O Space Enable for its BAR: don't
     * assume firmware/QEMU defaults already have these set. */
    uint32_t command = pci_config_read32(bus, slot, func, 0x04);
    command |= 0x0006;
    pci_config_write32(bus, slot, func, 0x04, command);

    volatile uint8_t *caplength_reg = (volatile uint8_t *)(uintptr_t)ehci.mmio_base;
    uint8_t caplength = *caplength_reg;
    ehci.op_base = ehci.mmio_base + caplength;

    volatile uint32_t *hcsparams_reg = (volatile uint32_t *)(uintptr_t)(ehci.mmio_base + 0x04);
    volatile uint32_t *hccparams_reg = (volatile uint32_t *)(uintptr_t)(ehci.mmio_base + 0x08);
    uint32_t hcsparams = *hcsparams_reg;
    uint32_t hccparams = *hccparams_reg;

    ehci.n_ports = (int)(hcsparams & 0x0F);
    if (ehci.n_ports > EHCI_MAX_PORTS) {
        ehci.n_ports = EHCI_MAX_PORTS;
    }

    ehci_bios_handoff(bus, slot, func, hccparams);

    /* Halt the controller before resetting it (spec requires RS=0 before
     * HCRESET is guaranteed well-defined). */
    volatile uint32_t *usbcmd = ehci_op_reg(0x00);
    volatile uint32_t *usbsts = ehci_op_reg(0x04);
    *usbcmd &= ~EHCI_USBCMD_RS;
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        if (*usbsts & EHCI_USBSTS_HCHALTED) {
            break;
        }
        ehci_io_delay(1000);
    }

    *usbcmd |= EHCI_USBCMD_HCRESET;
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        if (!(*usbcmd & EHCI_USBCMD_HCRESET)) {
            break;
        }
        ehci_io_delay(1000);
    }

    volatile uint32_t *usbintr = ehci_op_reg(0x08);
    *usbintr = 0; /* polling only, no IRQs -- same as kernel/uhci.c */

    /* Route all ports to this controller before starting it, then run. */
    volatile uint32_t *configflag = ehci_op_reg(0x40);
    *configflag = EHCI_CONFIGFLAG_CF;
    ehci_io_delay(1000);

    *usbcmd |= EHCI_USBCMD_RS;
    ehci_io_delay(1000);

    ehci.present = 1;

    for (int p = 0; p < ehci.n_ports; p++) {
        ehci_probe_port(p);
    }

    return 0;
}

const char *ehci_status_string(void) {
    uint32_t pos = 0;
    memset(ehci_status_buf, 0, sizeof(ehci_status_buf));

    if (!ehci.present) {
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "USB: no EHCI controller found.\n");
        return ehci_status_buf;
    }

    append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "USB: EHCI controller at MMIO base 0x");
    append_hex32(ehci_status_buf, &pos, sizeof(ehci_status_buf), ehci.mmio_base);
    append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ", ");
    append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(ehci.n_ports & 0x0F));
    append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), " port(s)\n");

    for (int p = 0; p < ehci.n_ports; p++) {
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "  port ");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)p);
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ": ");
        if (!ehci.port_connected[p]) {
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "no device\n");
            continue;
        }
        if (ehci.port_owned_by_companion[p]) {
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf),
                "device connected, not high-speed, released to companion controller\n");
            continue;
        }
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "device connected, ");
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ehci.port_enabled[p] ? "enabled, high-speed" : "NOT enabled");
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "\n");
    }

    return ehci_status_buf;
}
