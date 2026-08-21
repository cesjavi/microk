#include "ehci.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
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
#define EHCI_USBCMD_ASE      0x00000020u /* Async Schedule Enable */
#define EHCI_USBSTS_ASS      0x00008000u /* Async Schedule Status */
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

/* Queue Head link-pointer bits (EHCI spec 3.2.3): Typ selects what the
 * pointer refers to (QH here, always -- this driver never links to
 * iTD/siTD/FSTN entries), T marks the pointer invalid/terminate. */
#define EHCI_PTR_TERMINATE 0x00000001u
#define EHCI_PTR_TYP_QH    0x00000002u

/* qTD token (status/control) dword bits (EHCI spec 3.5.3). */
#define QTD_TOK_STATUS_ACTIVE   (1u << 7)
#define QTD_TOK_STATUS_HALTED   (1u << 6)
#define QTD_TOK_STATUS_DBUFERR  (1u << 5)
#define QTD_TOK_STATUS_BABBLE   (1u << 4)
#define QTD_TOK_STATUS_XACTERR  (1u << 3)
#define QTD_TOK_STATUS_ERROR_BITS (QTD_TOK_STATUS_HALTED | QTD_TOK_STATUS_DBUFERR | QTD_TOK_STATUS_BABBLE | QTD_TOK_STATUS_XACTERR)
#define QTD_TOK_PID_OUT   (0u << 8)
#define QTD_TOK_PID_IN    (1u << 8)
#define QTD_TOK_PID_SETUP (2u << 8)
#define QTD_TOK_CERR_3    (3u << 10)
#define QTD_TOK_IOC       (1u << 15)
#define QTD_TOK_BYTES_SHIFT 16
#define QTD_TOK_BYTES_MASK  (0x7FFFu << QTD_TOK_BYTES_SHIFT)
#define QTD_TOK_DT        (1u << 31)

/* Queue Head endpoint characteristics (DWord 1) / capabilities (DWord 2)
 * field shifts (EHCI spec 3.6.2). */
#define QH_CHAR_ENDPT_SHIFT   8
#define QH_CHAR_SPEED_SHIFT   12
#define QH_CHAR_SPEED_HIGH    (2u << QH_CHAR_SPEED_SHIFT)
#define QH_CHAR_DTC           (1u << 14) /* data toggle comes from qTD, not QH */
#define QH_CHAR_HEAD          (1u << 15) /* head of async reclamation list */
#define QH_CHAR_MPL_SHIFT     16
#define QH_CAP_MULT_1         (1u << 30)

/* Standard USB requests/descriptor types used for enumeration -- same
 * values as kernel/uhci.c's copies (USB spec constants, not driver
 * details), duplicated for the same reason as the config descriptor
 * parser below. */
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIGURATION 0x02

#define EHCI_CTRL_MAX_QTD  1   /* one stage in flight at a time -- see ehci_control_transfer */
#define EHCI_CTRL_DATA_OFF 512 /* clear of the QH/qTD pool, still one PMM page */
#define EHCI_CTRL_DATA_MAX 256 /* same cap as kernel/uhci.c's control scratch */

typedef struct {
    volatile uint32_t link;
    volatile uint32_t endpoint_chars;
    volatile uint32_t endpoint_caps;
    volatile uint32_t current_qtd;
    /* Transfer overlay -- same layout as ehci_qtd_t from here on, the HC
     * copies an active qTD's fields into this area while it runs. */
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
} __attribute__((aligned(32))) ehci_qh_t;

typedef struct {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
} __attribute__((aligned(32))) ehci_qtd_t;

typedef struct {
    ehci_qh_t head_qh;
    ehci_qtd_t td[EHCI_CTRL_MAX_QTD];
    uint8_t buf[EHCI_CTRL_DATA_OFF + EHCI_CTRL_DATA_MAX];
} ehci_ctrl_area_t;

/* One enumerated device per root port -- no hub support, so address
 * assignment can just be port+1 instead of a real free-address allocator.
 * Mirrors kernel/uhci.c's uhci_device_t (Etapa 2 fields only; bulk
 * endpoints are recorded for a following Bulk-Only-Transport stage but
 * not used yet). */
typedef struct {
    int valid;
    uint8_t fail_stage;
    int8_t fail_rc;
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
} ehci_device_t;

typedef struct {
    int present;
    uint32_t mmio_base;
    uint32_t op_base;
    int n_ports;
    int port_connected[EHCI_MAX_PORTS];
    int port_enabled[EHCI_MAX_PORTS];
    int port_owned_by_companion[EHCI_MAX_PORTS];
    ehci_ctrl_area_t *ctrl;
    ehci_device_t devices[EHCI_MAX_PORTS];
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

/* Timing for ehci_wait_overlay below, in port-0x80 busy-wait iterations
 * (see ehci_io_delay) rather than the HC's own FRINDEX frame counter.
 * FRINDEX (operational offset 0x0C) would be the theoretically correct
 * clock to use -- it's supposed to be the HC's free-running frame
 * counter, real wall-clock time regardless of guest CPU speed, and is
 * exactly what kernel/uhci.c's equivalent FRNUM-based polling relies on
 * for the same reason. It was tried here first, but confirmed dead under
 * this QEMU EHCI model via gdbstub: FRINDEX read back as a constant 0
 * across multiple real-time-separated reads while a transfer sat
 * waiting, so a wait loop gated on "FRINDEX must advance" hung forever.
 * Falling back to a large fixed iteration count is less principled (no
 * guarantee it corresponds to enough real time on a much slower or
 * faster host) but observably works against this QEMU version; revisit
 * with real EHCI hardware if this ever needs to be more robust than
 * "generous enough for the machines this was tested on". */
#define EHCI_SETTLE_ITERATIONS  250000u  /* before trusting an early Active=0 read */
#define EHCI_TIMEOUT_ITERATIONS 2000000u /* overall give-up point for a stuck transfer */

/* Polls the head QH's transfer overlay until the active qTD leaves
 * Active (success, error, or the timeout above), same role as
 * kernel/uhci.c's uhci_run_phase but against the QH overlay's token
 * instead of a per-TD status field -- the HC only exposes progress
 * through the QH's copy while a qTD is running. */
static int ehci_wait_overlay(uint32_t *out_bytes_left) {
    /* The caller just wrote a fresh qTD reference into the (previously
     * idle, Active=0) overlay a few instructions ago. QEMU's EHCI model
     * only notices and starts it the next time its own emulated
     * microframe timer fires in real wall-clock time -- under TCG,
     * emulated CPU instructions retire far faster than that, so a naive
     * "poll until Active clears" loop can read Active=0 on its very
     * first iteration and return instantly, having transferred nothing:
     * that 0 is still the caller's own just-written idle value, not
     * genuine completion. Burn a minimum number of iterations before
     * trusting an Active=0 reading, so the HC gets a real chance to pick
     * the transfer up first -- if Active is ever observed set to 1
     * before that, that's unambiguous (the HC really did start), so the
     * settle wait ends immediately in that case instead of always
     * running the full amount. */
    uint32_t token = 0;
    for (uint32_t settle_i = 0; settle_i < EHCI_SETTLE_ITERATIONS; settle_i++) {
        token = ehci.ctrl->head_qh.token;
        if (token & QTD_TOK_STATUS_ACTIVE) {
            break;
        }
        inb(0x80);
    }

    for (uint32_t i = 0; i < EHCI_TIMEOUT_ITERATIONS; i++) {
        token = ehci.ctrl->head_qh.token;
        if (!(token & QTD_TOK_STATUS_ACTIVE)) {
            if (out_bytes_left) {
                *out_bytes_left = (token & QTD_TOK_BYTES_MASK) >> QTD_TOK_BYTES_SHIFT;
            }
            return (token & QTD_TOK_STATUS_ERROR_BITS) ? -1 : 0;
        }
        inb(0x80);
    }

    if (out_bytes_left) {
        *out_bytes_left = (token & QTD_TOK_BYTES_MASK) >> QTD_TOK_BYTES_SHIFT;
    }
    return -2;
}

/* Async schedule bring-up: one head Queue Head, permanently linked to
 * itself (H=1, circular list of one), with USBCMD.ASE enabled so the HC
 * walks it continuously. Control/bulk transfers below just rewrite this
 * same QH's endpoint fields and point its overlay at a fresh qTD chain --
 * matches kernel/uhci.c's single reused control Queue Head, just EHCI's
 * async list instead of UHCI's frame-list-of-TDs. */
static int ehci_async_init(void) {
    ehci.ctrl = (ehci_ctrl_area_t *)pmm_alloc_block();
    if (!ehci.ctrl) {
        return -1;
    }
    memset(ehci.ctrl, 0, sizeof(*ehci.ctrl));

    ehci_qh_t *qh = &ehci.ctrl->head_qh;
    qh->link = (uint32_t)(uintptr_t)qh | EHCI_PTR_TYP_QH;
    qh->endpoint_chars = QH_CHAR_HEAD | QH_CHAR_DTC | QH_CHAR_SPEED_HIGH | (64u << QH_CHAR_MPL_SHIFT);
    qh->endpoint_caps = QH_CAP_MULT_1;
    qh->next_qtd = EHCI_PTR_TERMINATE;
    qh->alt_next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;

    volatile uint32_t *asynclistaddr = ehci_op_reg(0x18);
    *asynclistaddr = (uint32_t)(uintptr_t)qh;

    volatile uint32_t *usbcmd = ehci_op_reg(0x00);
    volatile uint32_t *usbsts = ehci_op_reg(0x04);
    *usbcmd |= EHCI_USBCMD_ASE;
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        if (*usbsts & EHCI_USBSTS_ASS) {
            break;
        }
        ehci_io_delay(1000);
    }

    return 0;
}

/* Runs one full USB control transfer (Setup, optional Data, Status)
 * against device address `addr`, endpoint 0 -- same signature/behavior
 * as kernel/uhci.c's uhci_control_transfer, just built out of a qTD chain
 * instead of UHCI Transfer Descriptors. All our devices are high-speed
 * (non-high-speed ports get released to the companion controller before
 * enumeration ever runs), so there's no low_speed parameter here. */
static int ehci_control_transfer(uint8_t addr, uint8_t mps,
                                  uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                  void *data, int data_in, uint16_t *out_len) {
    if (!ehci.present || !ehci.ctrl) {
        return -1;
    }
    if (wLength > EHCI_CTRL_DATA_MAX) {
        return -1;
    }
    if (mps == 0) {
        mps = 64;
    }
    if (out_len) {
        *out_len = 0;
    }

    ehci_qh_t *qh = &ehci.ctrl->head_qh;
    qh->endpoint_chars = QH_CHAR_HEAD | QH_CHAR_DTC | QH_CHAR_SPEED_HIGH
        | ((uint32_t)addr) | ((uint32_t)mps << QH_CHAR_MPL_SHIFT);

    uint8_t *setup = ehci.ctrl->buf;
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(wLength & 0xFF);
    setup[7] = (uint8_t)(wLength >> 8);

    uint8_t *dbuf = ehci.ctrl->buf + EHCI_CTRL_DATA_OFF;
    if (wLength > 0 && !data_in && data) {
        memcpy(dbuf, data, wLength);
    }

    /* Run each stage (Setup, optional Data, Status) as its own single-qTD
     * submission rather than a linked qTD chain. The QH's overlay is a
     * scratchpad the HC loads a qTD's fields INTO while running it -- the
     * HC never writes completion status back into the original qTD
     * structure in memory, so a chain built and then polled only once at
     * the end has no reliable way to recover each stage's actual byte
     * count/status afterward (and polling the overlay's Active bit can
     * also be fooled by the brief gap between the HC retiring one chained
     * qTD and auto-loading the next). Submitting one at a time and
     * reading the overlay while it's still that qTD's turn sidesteps
     * both problems, at the cost of a few extra microseconds of
     * scheduling latency between stages -- irrelevant next to the
     * multi-millisecond port-reset delays already elsewhere in this
     * driver. */
    ehci_qtd_t td;

    memset(&td, 0, sizeof(td));
    /* next_qtd/alt_next_qtd must carry their own Terminate bit -- a
     * memset-zeroed pointer field decodes as T=0 (i.e. a *valid* pointer
     * to physical address 0), not "no pointer". Whatever the HC actually
     * does with the overlay's alt_next_qtd (short-packet handling) or
     * with a supposedly-terminating chain, it copies these fields
     * verbatim off the qTD in memory when it fetches it -- setting the
     * QH overlay's own copy of these (further down) doesn't help,
     * because the fetch overwrites it right back with whatever the qTD
     * itself says. Leaving them as a raw zero pointer instead of a
     * proper Terminate was the actual cause of every qTD submitted this
     * way immediately Halting with CERR exhausted, discovered by
     * comparing against a minimal reference qTD layout after ruling out
     * timing (see ehci_wait_overlay) as the cause. */
    td.next_qtd = EHCI_PTR_TERMINATE;
    td.alt_next_qtd = EHCI_PTR_TERMINATE;
    td.buffer[0] = (uint32_t)(uintptr_t)setup;
    /* No QTD_TOK_DT: SETUP transactions always use DATA0. */
    td.token = QTD_TOK_STATUS_ACTIVE | QTD_TOK_CERR_3 | QTD_TOK_PID_SETUP
        | (8u << QTD_TOK_BYTES_SHIFT);
    memcpy((void *)&ehci.ctrl->td[0], &td, sizeof(td));
    qh->next_qtd = (uint32_t)(uintptr_t)&ehci.ctrl->td[0];
    qh->alt_next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;
    if (ehci_wait_overlay(0) != 0) {
        qh->next_qtd = EHCI_PTR_TERMINATE;
        qh->token = 0;
        return -1;
    }

    uint16_t received = 0;
    if (wLength > 0) {
        memset(&td, 0, sizeof(td));
        td.next_qtd = EHCI_PTR_TERMINATE;
        td.alt_next_qtd = EHCI_PTR_TERMINATE;
        td.buffer[0] = (uint32_t)(uintptr_t)dbuf;
        /* Starting toggle DATA1 (USB 2.0 spec 8.5.3); unlike UHCI, EHCI
         * doesn't need one qTD per max-packet chunk here -- a single qTD
         * can span multiple packet transactions, and the HC toggles
         * DATA0/DATA1 between them on its own using this as the start. */
        td.token = QTD_TOK_STATUS_ACTIVE | QTD_TOK_CERR_3
            | (data_in ? QTD_TOK_PID_IN : QTD_TOK_PID_OUT)
            | ((uint32_t)wLength << QTD_TOK_BYTES_SHIFT) | QTD_TOK_DT;
        memcpy((void *)&ehci.ctrl->td[0], &td, sizeof(td));
        qh->next_qtd = (uint32_t)(uintptr_t)&ehci.ctrl->td[0];
        qh->alt_next_qtd = EHCI_PTR_TERMINATE;
        qh->token = 0;

        uint32_t bytes_left = 0;
        int rc = ehci_wait_overlay(&bytes_left);
        if (rc != 0) {
            qh->next_qtd = EHCI_PTR_TERMINATE;
            qh->token = 0;
            return rc;
        }
        received = (uint16_t)(wLength - bytes_left);
        if (data_in && data && received > 0) {
            memcpy(data, dbuf, received);
        }
    }

    int status_in = (wLength > 0) ? !data_in : 1;
    memset(&td, 0, sizeof(td));
    td.next_qtd = EHCI_PTR_TERMINATE;
    td.alt_next_qtd = EHCI_PTR_TERMINATE;
    td.buffer[0] = 0;
    td.token = QTD_TOK_STATUS_ACTIVE | QTD_TOK_CERR_3
        | (status_in ? QTD_TOK_PID_IN : QTD_TOK_PID_OUT)
        | QTD_TOK_DT /* status stage is always DATA1 */;
    memcpy((void *)&ehci.ctrl->td[0], &td, sizeof(td));
    qh->next_qtd = (uint32_t)(uintptr_t)&ehci.ctrl->td[0];
    qh->alt_next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;
    if (ehci_wait_overlay(0) != 0) {
        qh->next_qtd = EHCI_PTR_TERMINATE;
        qh->token = 0;
        return -1;
    }

    qh->next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;
    if (out_len) {
        *out_len = received;
    }
    return 0;
}

/* Walks a configuration descriptor's variable-length tail looking for the
 * first bulk IN/OUT endpoint pair -- identical logic (and byte layout) to
 * kernel/uhci.c's uhci_parse_config_descriptor, duplicated rather than
 * shared since the two drivers otherwise share no code and USB descriptor
 * parsing is small/stable enough that the duplication cost is low. */
static void ehci_parse_config_descriptor(ehci_device_t *dev, const uint8_t *buf, uint16_t len) {
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

/* Enumerates the device on one already-reset, already-enabled (high-speed)
 * port -- same sequence as kernel/uhci.c's uhci_enumerate_port (learn the
 * real control max packet size from a short 8-byte device descriptor read,
 * SET_ADDRESS, read full device + configuration descriptors,
 * SET_CONFIGURATION). No hub support, address = port+1. */
static void ehci_enumerate_port(int port) {
    ehci_device_t *dev = &ehci.devices[port];
    memset(dev, 0, sizeof(*dev));

    if (!ehci.ctrl || !ehci.port_connected[port] || !ehci.port_enabled[port]) {
        return;
    }

    uint8_t new_addr = (uint8_t)(port + 1);

    uint8_t desc8[8];
    uint16_t got = 0;
    int rc = ehci_control_transfer(0, 64, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_DEVICE << 8), 0, 8, desc8, 1, &got);
    if (rc != 0 || got < 8) {
        dev->fail_stage = 1;
        dev->fail_rc = (int8_t)rc;
        return;
    }
    uint8_t mps0 = desc8[7];
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) {
        mps0 = 64; /* high-speed control endpoints always use 64 */
    }

    rc = ehci_control_transfer(0, mps0, 0x00, USB_REQ_SET_ADDRESS,
            new_addr, 0, 0, 0, 0, 0);
    if (rc != 0) {
        dev->fail_stage = 2;
        dev->fail_rc = (int8_t)rc;
        return;
    }
    /* Device recovery time after SET_ADDRESS before it answers on the new
     * address (USB 2.0 spec: >=2ms). */
    ehci_io_delay(20000);

    uint8_t devdesc[18];
    rc = ehci_control_transfer(new_addr, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
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
    if (ehci_control_transfer(new_addr, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, 9, cfghdr, 1, &got) != 0 || got < 9) {
        return; /* device descriptor alone is still recorded above */
    }

    uint16_t total_len = (uint16_t)(cfghdr[2] | (cfghdr[3] << 8));
    if (total_len < 9) {
        total_len = 9;
    }
    if (total_len > EHCI_CTRL_DATA_MAX) {
        total_len = EHCI_CTRL_DATA_MAX;
    }

    uint8_t cfgbuf[EHCI_CTRL_DATA_MAX];
    if (ehci_control_transfer(new_addr, mps0, 0x80, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, total_len, cfgbuf, 1, &got) == 0 && got >= 9) {
        ehci_parse_config_descriptor(dev, cfgbuf, got);

        if (ehci_control_transfer(new_addr, mps0, 0x00, USB_REQ_SET_CONFIGURATION,
                cfgbuf[5], 0, 0, 0, 0, 0) == 0) {
            dev->configured = 1;
        }
    }
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

    /* Async schedule must be running before any control transfer can be
     * issued during enumeration below -- if the scratch page allocation
     * fails, ehci.ctrl stays NULL and ehci_control_transfer() just
     * reports failure per port instead of enumerating (ports themselves
     * are still probed/reset/reported, same degraded-but-not-fatal shape
     * kernel/uhci.c uses when its own ctrl allocation fails). */
    ehci_async_init();

    for (int p = 0; p < ehci.n_ports; p++) {
        ehci_probe_port(p);
        ehci_enumerate_port(p);
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

        if (!ehci.port_enabled[p]) {
            continue;
        }

        ehci_device_t *dev = &ehci.devices[p];
        if (!dev->valid) {
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "    enumeration failed at stage ");
            append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->fail_stage & 0x0F));
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ", rc=");
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), dev->fail_rc < 0 ? "-" : "");
            append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf),
                (uint8_t)((dev->fail_rc < 0 ? -dev->fail_rc : dev->fail_rc) & 0x0F));
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "\n");
            continue;
        }

        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "    addr ");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->address & 0x0F));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ", VID:PID 0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->vendor_id >> 12) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->vendor_id >> 8) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->vendor_id >> 4) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->vendor_id & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ":0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->product_id >> 12) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->product_id >> 8) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)((dev->product_id >> 4) & 0xF));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->product_id & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ", class 0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_class >> 4));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_class & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "/0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_subclass >> 4));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_subclass & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "/0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_protocol >> 4));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->device_protocol & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "\n");

        if (!dev->configured) {
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "    not configured\n");
            continue;
        }

        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "    configured, interface class 0x");
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->interface_class >> 4));
        append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), (uint8_t)(dev->interface_class & 0xF));
        append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf),
            (dev->device_class == 0x08 || dev->interface_class == 0x08) ? " (mass storage)\n" : "\n");

        if (dev->bulk_in_ep || dev->bulk_out_ep) {
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "    bulk IN ep");
            append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), dev->bulk_in_ep);
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), ", bulk OUT ep");
            append_hex_digit(ehci_status_buf, &pos, sizeof(ehci_status_buf), dev->bulk_out_ep);
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), " (Bulk-Only Transport not implemented yet)\n");
        }
    }

    return ehci_status_buf;
}
