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

/* Queue Head / Transfer Descriptor link-pointer bits (Intel UHCI spec). */
#define UHCI_PTR_TERMINATE  0x00000001u
#define UHCI_PTR_QH         0x00000002u
#define UHCI_PTR_DEPTH      0x00000004u

/* TD Control/Status dword bits. */
#define TD_CTRL_ACTLEN_MASK 0x000007FFu
#define TD_CTRL_BITSTUFF    (1u << 17)
#define TD_CTRL_CRCTIMEO    (1u << 18)
#define TD_CTRL_NAK         (1u << 19)
#define TD_CTRL_BABBLE      (1u << 20)
#define TD_CTRL_DBUFERR     (1u << 21)
#define TD_CTRL_STALLED     (1u << 22)
#define TD_CTRL_ACTIVE      (1u << 23)
#define TD_CTRL_IOC         (1u << 24)
#define TD_CTRL_IOS         (1u << 25)
#define TD_CTRL_LS          (1u << 26)
#define TD_CTRL_CERR_SHIFT  27
#define TD_CTRL_SPD         (1u << 29)
#define TD_CTRL_ERROR_BITS  (TD_CTRL_BITSTUFF | TD_CTRL_CRCTIMEO | TD_CTRL_BABBLE | TD_CTRL_DBUFERR | TD_CTRL_STALLED)

/* TD Token dword bits. */
#define TD_TOKEN_PID_SETUP  0x2Du
#define TD_TOKEN_PID_IN     0x69u
#define TD_TOKEN_PID_OUT    0xE1u
#define TD_TOKEN_DEVADDR_SHIFT  8
#define TD_TOKEN_ENDPOINT_SHIFT 15
#define TD_TOKEN_TOGGLE     (1u << 19)
#define TD_TOKEN_MAXLEN_SHIFT   21

/* Standard USB requests used for enumeration. */
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIGURATION 0x02

/* Scratch area for control transfers: one Queue Head, an array of Transfer
 * Descriptors big enough for the largest data stage we issue (a
 * configuration descriptor, capped below), and a byte buffer for the setup
 * packet plus received/sent data. All in one PMM page so the natural 4KB
 * alignment trivially satisfies the 16-byte TD/QH alignment requirement.
 * The same QH and TD pool are reused for Bulk-Only Transport bulk
 * transfers (Etapa 3) -- this driver only ever has one transfer in flight
 * at a time (synchronous/polled, never control and bulk simultaneously),
 * so a second QH/frame-list entry would just be unused complexity. 64 TDs
 * at the 64-byte full-speed bulk max packet covers up to 4KB (8 sectors)
 * of CBW data in one chain. */
#define UHCI_CTRL_MAX_TD    64
#define UHCI_CTRL_DATA_OFF  16
#define UHCI_CTRL_DATA_MAX  256

/* link/status/element are polled after the UHCI controller writes them back
 * via DMA -- nothing in the visible C code ever stores to td->status after
 * submission, so without volatile GCC (-O3 here) is free to treat the poll
 * loop's read as invariant and hoist/cache it in a register, spinning
 * forever on a stale value while the hardware updates memory it never
 * re-reads. Cost this a lot of debugging: FRNUM (read via inb/inw, always
 * asm volatile) kept advancing normally while td->status appeared frozen
 * solid even seconds later, which briefly looked like a DMA/bus-mastering
 * problem before turning out to be a compiler-visible aliasing one. */
typedef struct {
    volatile uint32_t link;
    volatile uint32_t element;
} __attribute__((packed, aligned(16))) uhci_qh_t;

typedef struct {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct {
    uhci_qh_t qh;
    uhci_td_t td[UHCI_CTRL_MAX_TD];
    uint8_t buf[UHCI_CTRL_DATA_OFF + UHCI_CTRL_DATA_MAX];
} uhci_ctrl_area_t;

/* One enumerated device per root port -- no hub support, so address
 * assignment can just be port+1 instead of a real free-address allocator. */
typedef struct {
    int valid;
    uint8_t fail_stage; /* 0=n/a, else which enumeration step gave up first */
    int8_t fail_rc;     /* return code from uhci_control_transfer() at that step */
    uint8_t address;
    uint8_t max_packet0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configured;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    /* Persistent data toggle per bulk pipe -- USB spec resets both to DATA0
     * on SET_CONFIGURATION, then each pipe keeps its own alternating
     * sequence across separate transfers (unlike control transfers, which
     * always restart the sequence from a fixed point). */
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
} uhci_device_t;

typedef struct {
    uint16_t io_base;
    int present;
    uint32_t *frame_list;
    uhci_ctrl_area_t *ctrl;
    int port_connected[UHCI_PORT_COUNT];
    int port_enabled[UHCI_PORT_COUNT];
    int port_low_speed[UHCI_PORT_COUNT];
    uhci_device_t devices[UHCI_PORT_COUNT];
} uhci_controller_t;

static uhci_controller_t uhci;
static char uhci_status_buf[512];

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

static void append_hex32(char *out, uint32_t *pos, uint32_t max, uint32_t value) {
    append_hex16(out, pos, max, (uint16_t)(value >> 16));
    append_hex16(out, pos, max, (uint16_t)(value & 0xFFFF));
}

/* Prints a fixed-length SCSI text field (space-padded ASCII, e.g. INQUIRY's
 * vendor/product strings) trimmed of trailing spaces, with any non-printable
 * byte shown as '.'. */
static void append_scsi_text(char *out, uint32_t *pos, uint32_t max, const uint8_t *bytes, uint32_t len) {
    uint32_t end = len;
    while (end > 0 && bytes[end - 1] == ' ') {
        end--;
    }
    for (uint32_t i = 0; i < end && *pos + 1 < max; i++) {
        uint8_t c = bytes[i];
        out[*pos] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        (*pos)++;
    }
    out[*pos] = '\0';
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

static uint32_t uhci_td_token(uint8_t pid, uint8_t addr, uint8_t endpoint, int toggle, uint16_t len) {
    uint32_t maxlen_field = (len == 0) ? 0x7FFu : (uint32_t)(len - 1);
    uint32_t token = pid;
    token |= ((uint32_t)addr & 0x7Fu) << TD_TOKEN_DEVADDR_SHIFT;
    token |= ((uint32_t)endpoint & 0x0Fu) << TD_TOKEN_ENDPOINT_SHIFT;
    if (toggle) {
        token |= TD_TOKEN_TOGGLE;
    }
    token |= (maxlen_field & 0x7FFu) << TD_TOKEN_MAXLEN_SHIFT;
    return token;
}

/* The HC's own frame counter (FRNUM, 11 bits, one tick per real ~1ms SOF
 * generated by the controller's internal timer). Unlike an inb(0x80)
 * busy-loop iteration count, this advances at real wall-clock speed no
 * matter how fast the guest CPU runs under TCG -- discovered the hard way
 * when a fixed-iteration timeout (500000 port-0x80 reads, the same order of
 * magnitude as the port-reset delays above) fired instantly with FRNUM
 * still at 0 and the TD completely untouched: at native TCG speed that
 * iteration count finishes in far less than one real millisecond, so it
 * never gave the HC a chance to even process one frame. Polling against
 * FRNUM instead ties the timeout to genuine elapsed HC time. */
static uint16_t uhci_frnum(void) {
    return inw((uint16_t)(uhci.io_base + UHCI_REG_FRNUM)) & 0x07FFu;
}

/* Waits for roughly `frames` real HC frames (~1ms each) to elapse, driven by
 * FRNUM for the same reason uhci_run_phase polls it instead of an
 * iteration count -- used for USB spec recovery delays (e.g. >=2ms after
 * SET_ADDRESS) that need to correspond to real elapsed time. */
static void uhci_delay_frames(uint32_t frames) {
    uint16_t last_frame = uhci_frnum();
    uint32_t waited = 0;
    while (waited < frames) {
        uint16_t now = uhci_frnum();
        if (now != last_frame) {
            waited += (uint16_t)(now - last_frame) & 0x07FFu;
            last_frame = now;
        }
        inb(0x80);
    }
}

/* Timeout for a single TD, expressed in HC frames (~1ms each) -- generous
 * for a QEMU device to answer, short enough not to hang boot forever
 * against one that never responds. */
#define UHCI_CTRL_TIMEOUT_FRAMES 2000u

/* Runs the TD chain already built at uhci.ctrl->td[0..count) by pointing the
 * control Queue Head's element pointer at it and polling each TD in program
 * order until it leaves Active. allow_short lets an IN data stage end early
 * on a short packet (SPD) without treating it as an error -- normal when
 * asking for more bytes than a descriptor actually has. Leaves the queue
 * head idle (element=TERMINATE) before returning either way. out_done, if
 * non-null, receives how many TDs actually completed (short-circuited by
 * error or a short packet) -- bulk transfers need this to keep their
 * persistent data-toggle state correct across calls, since a short/failed
 * chain doesn't advance the toggle as far as building all `count` TDs
 * up front assumed.
 * Returns 0 on success, -1 on a real transfer error, -2 on timeout. */
static int uhci_run_phase(int count, int allow_short, uint16_t *out_actual, int *out_done) {
    uhci.ctrl->qh.element = (uint32_t)(uintptr_t)&uhci.ctrl->td[0];

    uint16_t total = 0;
    int rc = 0;
    int i;

    for (i = 0; i < count; i++) {
        uhci_td_t *td = &uhci.ctrl->td[i];
        uint16_t last_frame = uhci_frnum();
        uint32_t frames_waited = 0;
        uint32_t status;
        for (;;) {
            status = td->status;
            if (!(status & TD_CTRL_ACTIVE)) {
                break;
            }
            uint16_t now = uhci_frnum();
            if (now != last_frame) {
                frames_waited += (uint16_t)(now - last_frame) & 0x07FFu;
                last_frame = now;
                if (frames_waited >= UHCI_CTRL_TIMEOUT_FRAMES) {
                    rc = -2;
                    goto done;
                }
            }
            inb(0x80);
        }

        if (status & TD_CTRL_ERROR_BITS) {
            rc = -1;
            goto done;
        }

        uint32_t actlen_field = status & TD_CTRL_ACTLEN_MASK;
        uint16_t actual = (actlen_field == 0x7FFu) ? 0 : (uint16_t)(actlen_field + 1);
        total = (uint16_t)(total + actual);

        uint32_t maxlen_field = (td->token >> TD_TOKEN_MAXLEN_SHIFT) & 0x7FFu;
        uint16_t requested = (maxlen_field == 0x7FFu) ? 0 : (uint16_t)(maxlen_field + 1);
        if (allow_short && actual < requested) {
            i++; /* this TD did complete, just short -- count it */
            goto done;
        }
    }

done:
    uhci.ctrl->qh.element = UHCI_PTR_TERMINATE;
    if (out_actual) {
        *out_actual = total;
    }
    if (out_done) {
        *out_done = i;
    }
    return rc;
}

/* Runs one full USB control transfer (Setup, optional Data, Status) against
 * device address `addr`, endpoint 0. `mps` is the control endpoint's max
 * packet size -- pass 8 when it isn't known yet (every USB device's default
 * control pipe supports at least an 8-byte max packet). `data_in` selects
 * the data stage direction when wLength>0; the status stage direction and
 * data toggle sequence follow USB 2.0 spec 8.5.3 automatically. On success,
 * *out_len holds the number of bytes actually received for an IN data
 * stage (may be less than wLength -- short reads are normal). */
static int uhci_control_transfer(uint8_t addr, int low_speed, uint8_t mps,
                                  uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                  void *data, int data_in, uint16_t *out_len) {
    if (!uhci.present || !uhci.ctrl) {
        return -1;
    }
    if (wLength > UHCI_CTRL_DATA_MAX) {
        return -1;
    }
    if (mps == 0) {
        mps = 8;
    }
    if (out_len) {
        *out_len = 0;
    }

    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(wLength & 0xFF);
    setup[7] = (uint8_t)(wLength >> 8);
    memcpy(uhci.ctrl->buf, setup, 8);

    uhci_td_t *td0 = &uhci.ctrl->td[0];
    memset((void *)td0, 0, sizeof(*td0));
    td0->link = UHCI_PTR_TERMINATE;
    td0->status = TD_CTRL_ACTIVE | (3u << TD_CTRL_CERR_SHIFT) | (low_speed ? TD_CTRL_LS : 0);
    td0->token = uhci_td_token(TD_TOKEN_PID_SETUP, addr, 0, 0, 8);
    td0->buffer = (uint32_t)(uintptr_t)uhci.ctrl->buf;

    if (uhci_run_phase(1, 0, 0, 0) != 0) {
        return -1;
    }

    uint16_t received = 0;
    uint8_t *dbuf = uhci.ctrl->buf + UHCI_CTRL_DATA_OFF;

    if (wLength > 0) {
        if (!data_in && data) {
            memcpy(dbuf, data, wLength);
        }

        int n = (wLength + mps - 1) / mps;
        int toggle = 1;
        uint16_t remaining = wLength;
        for (int i = 0; i < n; i++) {
            uhci_td_t *td = &uhci.ctrl->td[i];
            uint16_t chunk = (remaining < mps) ? remaining : mps;
            memset((void *)td, 0, sizeof(*td));
            td->link = (i + 1 < n)
                ? (uint32_t)(((uintptr_t)&uhci.ctrl->td[i + 1]) | UHCI_PTR_DEPTH)
                : UHCI_PTR_TERMINATE;
            td->status = TD_CTRL_ACTIVE | (3u << TD_CTRL_CERR_SHIFT) | (low_speed ? TD_CTRL_LS : 0)
                | (data_in ? TD_CTRL_SPD : 0);
            td->token = uhci_td_token(data_in ? TD_TOKEN_PID_IN : TD_TOKEN_PID_OUT, addr, 0, toggle, chunk);
            td->buffer = (uint32_t)(uintptr_t)(dbuf + (wLength - remaining));
            toggle ^= 1;
            remaining = (uint16_t)(remaining - chunk);
        }

        int rc = uhci_run_phase(n, data_in, &received, 0);
        if (rc != 0) {
            return rc;
        }
        if (data_in && data && received > 0) {
            memcpy(data, dbuf, received);
        }
    }

    /* Status stage: opposite direction of the data stage, or IN if there was
     * no data stage at all. Always zero-length, always DATA1. */
    int status_in = (wLength > 0) ? !data_in : 1;
    uhci_td_t *tds = &uhci.ctrl->td[0];
    memset((void *)tds, 0, sizeof(*tds));
    tds->link = UHCI_PTR_TERMINATE;
    tds->status = TD_CTRL_ACTIVE | (3u << TD_CTRL_CERR_SHIFT) | (low_speed ? TD_CTRL_LS : 0);
    tds->token = uhci_td_token(status_in ? TD_TOKEN_PID_IN : TD_TOKEN_PID_OUT, addr, 0, 1, 0);
    tds->buffer = 0;

    if (uhci_run_phase(1, 0, 0, 0) != 0) {
        return -1;
    }

    if (out_len) {
        *out_len = received;
    }
    return 0;
}

/* Runs one bulk transfer of up to `len` bytes directly to/from `buf` -- no
 * scratch copy, since kernel buffers are already physically addressable the
 * same way the control-transfer scratch area is. Chunks into `mps`-sized
 * TDs (capped by the shared TD pool, UHCI_CTRL_MAX_TD * mps bytes at most)
 * and threads *toggle through uhci_run_phase's out_done so a short/failed
 * chain still leaves the right DATA0/DATA1 state for the next call on this
 * pipe. Returns 0 on success (out_len may be less than len on a short
 * read), -1 on stall/error (the pipe should be considered wedged until a
 * CLEAR_FEATURE(ENDPOINT_HALT), not implemented here -- Etapa 4 territory
 * if it turns out to matter for real devices), -2 on timeout. */
static int uhci_bulk_transfer(uint8_t addr, uint8_t endpoint, int data_in, uint16_t mps,
                               int low_speed, void *buf, uint32_t len, uint8_t *toggle,
                               uint32_t *out_len) {
    if (!uhci.present || !uhci.ctrl) {
        return -1;
    }
    if (mps == 0) {
        mps = 64;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (len == 0) {
        return 0;
    }

    int n = (int)((len + mps - 1) / mps);
    if (n > UHCI_CTRL_MAX_TD) {
        return -1; /* caller asked for more than one chain can carry */
    }

    int start_toggle = *toggle & 1;
    int tog = start_toggle;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;
    for (int i = 0; i < n; i++) {
        uhci_td_t *td = &uhci.ctrl->td[i];
        uint16_t chunk = (remaining < mps) ? (uint16_t)remaining : mps;
        memset((void *)td, 0, sizeof(*td));
        td->link = (i + 1 < n)
            ? (uint32_t)(((uintptr_t)&uhci.ctrl->td[i + 1]) | UHCI_PTR_DEPTH)
            : UHCI_PTR_TERMINATE;
        td->status = TD_CTRL_ACTIVE | (3u << TD_CTRL_CERR_SHIFT) | (low_speed ? TD_CTRL_LS : 0)
            | (data_in ? TD_CTRL_SPD : 0);
        td->token = uhci_td_token(data_in ? TD_TOKEN_PID_IN : TD_TOKEN_PID_OUT, addr, endpoint, tog, chunk);
        td->buffer = (uint32_t)(uintptr_t)(p + (len - remaining));
        tog ^= 1;
        remaining -= chunk;
    }

    uint16_t actual = 0;
    int done = 0;
    int rc = uhci_run_phase(n, data_in, &actual, &done);
    *toggle = (uint8_t)((start_toggle + done) & 1);
    if (out_len) {
        *out_len = actual;
    }
    return rc;
}

/* Walks a configuration descriptor's variable-length tail (interface +
 * endpoint descriptors) looking for the first bulk IN/OUT endpoint pair --
 * enough to describe a Mass Storage interface for the Bulk-Only Transport
 * stage (Etapa 3), without needing a general descriptor-tree model. */
static void uhci_parse_config_descriptor(uhci_device_t *dev, const uint8_t *buf, uint16_t len) {
    uint16_t off = 0;
    while ((uint32_t)off + 2 <= len) {
        uint8_t blen = buf[off];
        uint8_t btype = buf[off + 1];
        if (blen < 2 || (uint32_t)off + blen > len) {
            break;
        }
        if (btype == 0x04 && blen >= 9) { /* INTERFACE */
            dev->interface_class = buf[off + 5];
            dev->interface_subclass = buf[off + 6];
            dev->interface_protocol = buf[off + 7];
        } else if (btype == 0x05 && blen >= 7) { /* ENDPOINT */
            uint8_t ep_addr = buf[off + 2];
            uint8_t attrs = buf[off + 3];
            uint16_t ep_mps = (uint16_t)(buf[off + 4] | (buf[off + 5] << 8));
            if ((attrs & 0x03) == 0x02) { /* bulk */
                if (ep_addr & 0x80) {
                    dev->bulk_in_ep = (uint8_t)(ep_addr & 0x0F);
                    dev->bulk_in_mps = ep_mps;
                } else {
                    dev->bulk_out_ep = (uint8_t)(ep_addr & 0x0F);
                    dev->bulk_out_mps = ep_mps;
                }
            }
        }
        off = (uint16_t)(off + blen);
    }
}

/* Enumerates the device on one already-reset, already-enabled port: learns
 * the real control max packet size from a short 8-byte device descriptor
 * read, assigns it a device address, reads the full device and
 * configuration descriptors, and issues SET_CONFIGURATION. No hub support,
 * so address assignment is simply port+1. Any failed step leaves
 * dev->valid=0 and stops -- enumeration is best-effort diagnostic/prep work
 * at this stage, not something the rest of boot depends on. */
static void uhci_enumerate_port(int port) {
    uhci_device_t *dev = &uhci.devices[port];
    memset(dev, 0, sizeof(*dev));

    if (!uhci.ctrl || !uhci.port_connected[port] || !uhci.port_enabled[port]) {
        return;
    }

    int low_speed = uhci.port_low_speed[port];
    uint8_t new_addr = (uint8_t)(port + 1);

    uint8_t desc8[8];
    uint16_t got = 0;
    int rc = uhci_control_transfer(0, low_speed, 8, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_DEVICE << 8), 0, 8, desc8, 1, &got);
    if (rc != 0 || got < 8) {
        dev->fail_stage = 1;
        dev->fail_rc = (int8_t)rc;
        return;
    }
    uint8_t mps0 = desc8[7];
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) {
        mps0 = 8;
    }

    rc = uhci_control_transfer(0, low_speed, mps0, 0x00, USB_REQ_SET_ADDRESS,
            new_addr, 0, 0, 0, 0, 0);
    if (rc != 0) {
        dev->fail_stage = 2;
        dev->fail_rc = (int8_t)rc;
        return;
    }
    /* Device recovery time after SET_ADDRESS before it answers on the new
     * address (USB 2.0 spec: >=2ms). */
    uhci_delay_frames(10);

    uint8_t devdesc[18];
    rc = uhci_control_transfer(new_addr, low_speed, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_DEVICE << 8), 0, 18, devdesc, 1, &got);
    if (rc != 0 || got < 18) {
        dev->fail_stage = 3;
        dev->fail_rc = (int8_t)rc;
        return;
    }

    dev->address = new_addr;
    dev->max_packet0 = mps0;
    dev->device_class = devdesc[4];
    dev->device_subclass = devdesc[5];
    dev->device_protocol = devdesc[6];
    dev->vendor_id = (uint16_t)(devdesc[8] | (devdesc[9] << 8));
    dev->product_id = (uint16_t)(devdesc[10] | (devdesc[11] << 8));
    dev->valid = 1;

    uint8_t cfghdr[9];
    if (uhci_control_transfer(new_addr, low_speed, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, 9, cfghdr, 1, &got) != 0 || got < 9) {
        return; /* device descriptor alone is still recorded above */
    }

    uint16_t total_len = (uint16_t)(cfghdr[2] | (cfghdr[3] << 8));
    if (total_len < 9) {
        total_len = 9;
    }
    if (total_len > UHCI_CTRL_DATA_MAX) {
        total_len = UHCI_CTRL_DATA_MAX; /* defensive cap on our fixed scratch buffer */
    }

    uint8_t cfgbuf[UHCI_CTRL_DATA_MAX];
    if (uhci_control_transfer(new_addr, low_speed, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, total_len, cfgbuf, 1, &got) == 0 && got >= 9) {
        uhci_parse_config_descriptor(dev, cfgbuf, got);

        if (uhci_control_transfer(new_addr, low_speed, mps0, 0x00, USB_REQ_SET_CONFIGURATION,
                cfgbuf[5], 0, 0, 0, 0, 0) == 0) {
            dev->configured = 1;
            dev->bulk_in_toggle = 0;
            dev->bulk_out_toggle = 0;
        }
    }
}

/* ---- USB Mass Storage: Bulk-Only Transport + minimal SCSI (Etapa 3) ----
 *
 * Built on top of the bulk pipes Etapa 2 already found (uhci_bulk_transfer,
 * dev->bulk_in_ep/out_ep). No block_device_t/VFS integration here -- that's
 * Etapa 4; this stage only needs to prove CBW/CSW + a handful of SCSI
 * commands work end to end against a real (emulated) device. */

#define USB_BOT_CBW_LEN 31
#define USB_BOT_CSW_LEN 13
#define USB_BOT_CBW_SIG 0x43425355u /* "USBC" */
#define USB_BOT_CSW_SIG 0x53425355u /* "USBS" */

static uint32_t uhci_msd_next_tag = 1;

static void uhci_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t uhci_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t uhci_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* First configured device with both bulk endpoints found -- no hub support,
 * so at most UHCI_PORT_COUNT devices exist and there's no enumeration/LUN
 * concept beyond "the one mass-storage device", matching where Etapa 2 left
 * off. */
static uhci_device_t *uhci_msd_device(void) {
    for (int p = 0; p < UHCI_PORT_COUNT; p++) {
        uhci_device_t *dev = &uhci.devices[p];
        if (dev->valid && dev->configured && dev->bulk_in_ep && dev->bulk_out_ep) {
            return dev;
        }
    }
    return 0;
}

/* Runs one Bulk-Only Transport command: CBW (wrapping `cdb`, cdb_len bytes)
 * out, an optional data stage (`data_len` bytes, direction `data_in`)
 * through the matching bulk pipe, then the CSW in. Validates the CSW
 * signature and that its tag echoes the CBW's. Returns 0 on CSW status
 * "passed" (0x00), -1 on CSW "failed"/"phase error" or a signature/tag
 * mismatch (protocol-level but not transport-level failure), -2 on a UHCI
 * transfer error/timeout on any of the three stages. */
static int uhci_msd_command(uhci_device_t *dev, const uint8_t *cdb, uint8_t cdb_len,
                             void *data, uint32_t data_len, int data_in, uint32_t *out_len) {
    if (!dev || cdb_len == 0 || cdb_len > 16) {
        return -2;
    }

    uint32_t tag = uhci_msd_next_tag++;

    uint8_t cbw[USB_BOT_CBW_LEN];
    memset(cbw, 0, sizeof(cbw));
    uhci_put_le32(cbw + 0, USB_BOT_CBW_SIG);
    uhci_put_le32(cbw + 4, tag);
    uhci_put_le32(cbw + 8, data_len);
    cbw[12] = (uint8_t)((data_len > 0 && data_in) ? 0x80 : 0x00);
    cbw[13] = 0; /* LUN 0 -- no hub/multi-LUN support */
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);

    if (uhci_bulk_transfer(dev->address, dev->bulk_out_ep, 0, dev->bulk_out_mps,
            0 /* bulk endpoints don't exist on low-speed devices, USB 2.0 spec 5.8 */,
            cbw, USB_BOT_CBW_LEN, &dev->bulk_out_toggle, 0) != 0) {
        return -2;
    }

    uint32_t transferred = 0;
    if (data_len > 0) {
        uint8_t *pipe_toggle = data_in ? &dev->bulk_in_toggle : &dev->bulk_out_toggle;
        uint8_t pipe_ep = data_in ? dev->bulk_in_ep : dev->bulk_out_ep;
        uint16_t pipe_mps = data_in ? dev->bulk_in_mps : dev->bulk_out_mps;
        if (uhci_bulk_transfer(dev->address, pipe_ep, data_in, pipe_mps, 0,
                data, data_len, pipe_toggle, &transferred) != 0) {
            return -2;
        }
    }

    uint8_t csw[USB_BOT_CSW_LEN];
    uint32_t csw_len = 0;
    if (uhci_bulk_transfer(dev->address, dev->bulk_in_ep, 1, dev->bulk_in_mps, 0,
            csw, USB_BOT_CSW_LEN, &dev->bulk_in_toggle, &csw_len) != 0 || csw_len < USB_BOT_CSW_LEN) {
        return -2;
    }

    if (uhci_get_le32(csw + 0) != USB_BOT_CSW_SIG || uhci_get_le32(csw + 4) != tag) {
        return -1;
    }

    if (out_len) {
        *out_len = transferred;
    }
    return (csw[12] == 0) ? 0 : -1;
}

/* SCSI TEST UNIT READY (6-byte CDB, no data stage). */
static int uhci_msd_test_unit_ready(uhci_device_t *dev) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x00;
    return uhci_msd_command(dev, cdb, 6, 0, 0, 0, 0);
}

/* SCSI INQUIRY (6-byte CDB, data IN). `out_size` is capped to 36 bytes, the
 * standard INQUIRY data length every SCSI device supports. */
static int uhci_msd_inquiry(uhci_device_t *dev, uint8_t *out, uint32_t out_size) {
    uint8_t len = (uint8_t)(out_size < 36 ? out_size : 36);
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = len;
    return uhci_msd_command(dev, cdb, 6, out, len, 1, 0);
}

/* SCSI READ CAPACITY(10) (10-byte CDB, 8 bytes data IN: last valid LBA and
 * block size, both big-endian per SCSI convention). */
static int uhci_msd_read_capacity10(uhci_device_t *dev, uint32_t *out_last_lba, uint32_t *out_block_size) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    uint8_t data[8];
    uint32_t got = 0;
    int rc = uhci_msd_command(dev, cdb, 10, data, 8, 1, &got);
    if (rc != 0) {
        return rc;
    }
    if (got < 8) {
        return -2;
    }
    if (out_last_lba) {
        *out_last_lba = uhci_get_be32(data + 0);
    }
    if (out_block_size) {
        *out_block_size = uhci_get_be32(data + 4);
    }
    return 0;
}

/* SCSI READ(10): `count` blocks of `block_size` bytes starting at `lba`
 * into `buf`. `count * block_size` is capped by the shared TD pool the same
 * way any bulk transfer is (UHCI_CTRL_MAX_TD * bulk_in_mps bytes at most --
 * 8 512-byte sectors at the usual full-speed 64-byte max packet). */
static int uhci_msd_read10(uhci_device_t *dev, uint32_t lba, uint16_t count, uint32_t block_size, void *buf) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    return uhci_msd_command(dev, cdb, 10, buf, (uint32_t)count * block_size, 1, 0);
}

/* SCSI WRITE(10): same layout/caps as uhci_msd_read10, opposite direction. */
static int uhci_msd_write10(uhci_device_t *dev, uint32_t lba, uint16_t count, uint32_t block_size, const void *buf) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    return uhci_msd_command(dev, cdb, 10, (void *)buf, (uint32_t)count * block_size, 0, 0);
}

/* ---- block_device_t integration (Etapa 4) ----
 *
 * Mirrors kernel/ata.c's ata_block_read/write + ata_primary_master shape
 * exactly, so partition_scan_mbr/vfs_mount need zero changes to work on a
 * USB stick. One sector per SCSI command (like ata.c, not batched through
 * uhci_bulk_transfer's up-to-8-sector chain capacity) -- this stage only
 * needs correctness, and matching ata.c's already-proven read-modify-write
 * handling for partial-sector I/O is worth more than the extra throughput
 * from batching. */

#define UHCI_MSD_MAX_BLOCK_SIZE 4096

static block_device_t uhci_msd_blockdev;
static int uhci_msd_blockdev_valid = 0;

static uint32_t uhci_msd_block_read(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    uhci_device_t *dev = (uhci_device_t *)device->driver_data;
    if (!dev || !buffer) {
        return 0;
    }

    uint32_t block_size = device->block_size;
    uint8_t sector[UHCI_MSD_MAX_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t absolute = offset + done;
        uint32_t lba = absolute / block_size;
        uint32_t sector_offset = absolute % block_size;
        uint32_t chunk = block_size - sector_offset;
        if (chunk > size - done) {
            chunk = size - done;
        }

        if (lba >= device->block_count) {
            break;
        }
        if (uhci_msd_read10(dev, lba, 1, block_size, sector) != 0) {
            break;
        }
        memcpy(buffer + done, sector + sector_offset, chunk);
        done += chunk;
    }
    return done;
}

static uint32_t uhci_msd_block_write(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    uhci_device_t *dev = (uhci_device_t *)device->driver_data;
    if (!dev || !buffer) {
        return 0;
    }

    uint32_t block_size = device->block_size;
    uint8_t sector[UHCI_MSD_MAX_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t absolute = offset + done;
        uint32_t lba = absolute / block_size;
        uint32_t sector_offset = absolute % block_size;
        uint32_t chunk = block_size - sector_offset;
        if (chunk > size - done) {
            chunk = size - done;
        }

        if (lba >= device->block_count) {
            break;
        }
        if (chunk < block_size) {
            /* Partial write: read-modify-write, same as ata_block_write. */
            if (uhci_msd_read10(dev, lba, 1, block_size, sector) != 0) {
                break;
            }
            memcpy(sector + sector_offset, buffer + done, chunk);
            if (uhci_msd_write10(dev, lba, 1, block_size, sector) != 0) {
                break;
            }
        } else {
            if (uhci_msd_write10(dev, lba, 1, block_size, buffer + done) != 0) {
                break;
            }
        }
        done += chunk;
    }
    return done;
}

int uhci_msd_init(void) {
    uhci_msd_blockdev_valid = 0;

    uhci_device_t *dev = uhci_msd_device();
    if (!dev) {
        return -1;
    }

    uint32_t last_lba = 0, block_size = 0;
    if (uhci_msd_read_capacity10(dev, &last_lba, &block_size) != 0) {
        return -1;
    }
    if (block_size == 0 || block_size > UHCI_MSD_MAX_BLOCK_SIZE) {
        return -1;
    }

    memset(&uhci_msd_blockdev, 0, sizeof(uhci_msd_blockdev));
    uhci_msd_blockdev.name = "usbmsd0";
    uhci_msd_blockdev.block_size = block_size;
    uhci_msd_blockdev.block_count = last_lba + 1;
    uhci_msd_blockdev.driver_data = dev;
    uhci_msd_blockdev.read = uhci_msd_block_read;
    uhci_msd_blockdev.write = uhci_msd_block_write;
    uhci_msd_blockdev_valid = 1;
    return 0;
}

block_device_t *uhci_msd_primary(void) {
    return uhci_msd_blockdev_valid ? &uhci_msd_blockdev : 0;
}

static char uhci_msd_buf[512];

/* Runs a diagnostic pass over the attached mass-storage device and formats
 * the result: INQUIRY, TEST UNIT READY, READ CAPACITY(10), then a READ(10)
 * of LBA 0. All read-only/safe against real hardware.
 *
 * with_write_test additionally does a WRITE(10)+READ(10) roundtrip at a
 * scratch LBA (100) to prove WRITE(10) works -- gated behind this flag
 * rather than always running, since unlike the read-only checks this
 * genuinely writes to whatever is plugged in; callers should only pass 1
 * for a disposable test image (e.g. this project's `usbstick.img`), not
 * against real hardware someone forgot was attached. */
const char *uhci_msd_test_string(int with_write_test) {
    uint32_t pos = 0;
    memset(uhci_msd_buf, 0, sizeof(uhci_msd_buf));

    uhci_device_t *dev = uhci_msd_device();
    if (!dev) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: no configured mass-storage device found.\n");
        return uhci_msd_buf;
    }

    uint8_t inq[36];
    memset(inq, 0, sizeof(inq));
    if (uhci_msd_inquiry(dev, inq, sizeof(inq)) != 0) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: INQUIRY failed.\n");
        return uhci_msd_buf;
    }
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: INQUIRY ok, vendor='");
    append_scsi_text(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), inq + 8, 8);
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "' product='");
    append_scsi_text(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), inq + 16, 16);
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "'\n");

    int tur_rc = uhci_msd_test_unit_ready(dev);
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: TEST UNIT READY ");
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), tur_rc == 0 ? "ready\n" : "not ready\n");

    uint32_t last_lba = 0, block_size = 0;
    if (uhci_msd_read_capacity10(dev, &last_lba, &block_size) != 0) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: READ CAPACITY(10) failed.\n");
        return uhci_msd_buf;
    }
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: capacity 0x");
    append_hex32(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), last_lba + 1);
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), " blocks x 0x");
    append_hex32(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), block_size);
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), " bytes\n");

    if (block_size != 512) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: block size != 512, skipping READ(10)/WRITE(10) checks.\n");
        return uhci_msd_buf;
    }

    uint8_t sector[512];
    if (uhci_msd_read10(dev, 0, 1, 512, sector) != 0) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: READ(10) LBA 0 failed.\n");
        return uhci_msd_buf;
    }
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: READ(10) LBA 0 ok, first 16 bytes:");
    for (int i = 0; i < 16; i++) {
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), " ");
        append_hex8(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), sector[i]);
    }
    append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "\n");

    if (with_write_test) {
        uint32_t scratch_lba = 100;
        if (scratch_lba > last_lba) {
            append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: device too small for write test, skipped.\n");
            return uhci_msd_buf;
        }
        uint8_t pattern[512];
        for (uint32_t i = 0; i < sizeof(pattern); i++) {
            pattern[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
        }
        if (uhci_msd_write10(dev, scratch_lba, 1, 512, pattern) != 0) {
            append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: WRITE(10) at LBA 0x64 failed.\n");
            return uhci_msd_buf;
        }
        uint8_t verify[512];
        if (uhci_msd_read10(dev, scratch_lba, 1, 512, verify) != 0) {
            append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), "USB MSD: post-write READ(10) failed.\n");
            return uhci_msd_buf;
        }
        int match = (memcmp(pattern, verify, sizeof(pattern)) == 0);
        append_str(uhci_msd_buf, &pos, sizeof(uhci_msd_buf), match
            ? "USB MSD: WRITE(10)+READ(10) roundtrip verified at LBA 0x64.\n"
            : "USB MSD: WRITE(10)+READ(10) roundtrip MISMATCH.\n");
    }

    return uhci_msd_buf;
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

            /* Bus Master Enable (PCI Command bit 2) so the controller's own
             * DMA reads/writes of the frame list, Queue Heads and Transfer
             * Descriptors actually reach guest RAM -- without it the HC
             * still runs (FRNUM keeps ticking, port registers still work
             * since those are plain I/O-port reads/writes, not DMA) but
             * silently never touches anything linked into the schedule.
             * I/O Space Enable (bit 0) is included too for the same reason
             * kernel/net.c enables Memory Space Enable for the e1000 BAR:
             * firmware/QEMU defaults shouldn't be assumed. */
            uint32_t command = pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
            command |= 0x0005;
            pci_config_write32(dev->bus, dev->slot, dev->function, 0x04, command);

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

    /* Control transfer scratch: one Queue Head + TD pool + data buffer,
     * same PMM-page alignment trick as the frame list (Etapa 2). Without it
     * the frame list falls back to all-Terminate like before -- the
     * controller and ports still come up, just no transfers are possible. */
    uhci.ctrl = (uhci_ctrl_area_t *)pmm_alloc_block();
    if (uhci.ctrl) {
        memset(uhci.ctrl, 0, sizeof(*uhci.ctrl));
        uhci.ctrl->qh.link = UHCI_PTR_TERMINATE;
        uhci.ctrl->qh.element = UHCI_PTR_TERMINATE;
    }

    uint32_t frame_entry = uhci.ctrl
        ? (uint32_t)(((uintptr_t)&uhci.ctrl->qh) | UHCI_PTR_QH)
        : UHCI_TD_TERMINATE;
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        /* Every frame points at the same control Queue Head -- the only
         * queue this driver schedules so far. Idle (qh.element=TERMINATE)
         * costs the HC one no-op fetch per frame; uhci_control_transfer()
         * points qh.element at real work only while a transfer is in
         * flight. */
        uhci.frame_list[i] = frame_entry;
    }

    outl((uint16_t)(uhci.io_base + UHCI_REG_FRBASEADD), (uint32_t)(uintptr_t)uhci.frame_list);
    outw((uint16_t)(uhci.io_base + UHCI_REG_FRNUM), 0);
    outw((uint16_t)(uhci.io_base + UHCI_REG_USBINTR), 0); /* polling only, no IRQs */

    outw((uint16_t)(uhci.io_base + UHCI_REG_USBCMD), UHCI_CMD_RUN | UHCI_CMD_CF | UHCI_CMD_MAXP64);
    uhci.present = 1;

    for (int p = 0; p < UHCI_PORT_COUNT; p++) {
        uhci_probe_port(p);
        uhci_enumerate_port(p);
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

        uhci_device_t *dev = &uhci.devices[p];
        if (!dev->valid) {
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "    enumeration failed at stage ");
            append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf), (uint8_t)(dev->fail_stage & 0x0F));
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ", rc=");
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->fail_rc < 0 ? "-" : "");
            append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf),
                (uint8_t)((dev->fail_rc < 0 ? -dev->fail_rc : dev->fail_rc) & 0x0F));
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "\n");
            continue;
        }

        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "    addr ");
        append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf), (uint8_t)(dev->address & 0x0F));
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ", VID:PID 0x");
        append_hex16(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->vendor_id);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ":0x");
        append_hex16(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->product_id);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ", class 0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->device_class);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "/0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->device_subclass);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "/0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->device_protocol);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "\n");

        if (!dev->configured) {
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "    not configured\n");
            continue;
        }

        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "    configured, interface class 0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->interface_class);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "/0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->interface_subclass);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "/0x");
        append_hex8(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->interface_protocol);
        append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf),
            dev->device_class == 0x08 || dev->interface_class == 0x08 ? " (mass storage)\n" : "\n");

        if (dev->bulk_in_ep || dev->bulk_out_ep) {
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "    bulk IN ep");
            append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->bulk_in_ep);
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), " (mps ");
            append_hex16(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->bulk_in_mps);
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), "), bulk OUT ep");
            append_hex_digit(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->bulk_out_ep);
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), " (mps ");
            append_hex16(uhci_status_buf, &pos, sizeof(uhci_status_buf), dev->bulk_out_mps);
            append_str(uhci_status_buf, &pos, sizeof(uhci_status_buf), ")\n");
        }
    }

    return uhci_status_buf;
}
