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
    /* Persistent data toggle per bulk pipe, same reasoning as
     * kernel/uhci.c's uhci_device_t fields of the same name. */
    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;
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

/* A single qTD's 5 buffer-pointer dwords describe up to 5 consecutive
 * physical pages: buffer[0]'s low 12 bits are the starting offset within
 * its page, buffer[1..4] are the page-aligned base addresses of each
 * following page (EHCI spec 3.5.4) -- the HC advances through them on
 * its own as the transfer crosses page boundaries. kernel/uhci.c never
 * needs this (it chunks into one Transfer Descriptor per max-packet
 * already), but ehci_bulk_transfer hands the caller's buffer straight to
 * hardware with no scratch copy (same no-copy reasoning UHCI's own
 * uhci_bulk_transfer comment gives), and that buffer's alignment isn't
 * under this driver's control -- a 512-byte sector read from an
 * arbitrary stack address can genuinely straddle a page boundary. */
static int ehci_qtd_set_buffer(ehci_qtd_t *td, const void *buf, uint32_t len) {
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    uint32_t page0 = addr & ~0xFFFu;
    uint32_t last = addr + (len > 0 ? len - 1 : 0);
    uint32_t page_count = ((last & ~0xFFFu) - page0) / 0x1000u + 1;
    if (page_count > 5) {
        return -1; /* would need a 6th page pointer, which qTDs don't have */
    }
    td->buffer[0] = addr;
    for (uint32_t i = 1; i < page_count; i++) {
        td->buffer[i] = page0 + i * 0x1000u;
    }
    return 0;
}

/* Runs one bulk transfer of up to `len` bytes directly to/from `buf` -- no
 * scratch copy, same reasoning as kernel/uhci.c's uhci_bulk_transfer.
 * Single qTD (see ehci_qtd_set_buffer above for why that's still safe
 * across page boundaries), so up to ~20KB in one call depending on
 * buf's starting offset into its first page; callers here never need
 * more than one sector at a time. *toggle carries the pipe's DATA0/DATA1
 * state across calls the same way UHCI's does, computed from how many
 * whole packets this call actually moved (EHCI toggles DATA0/DATA1
 * between packets within a single qTD on its own -- this driver just
 * needs to predict where that leaves things for the *next* qTD, since
 * each call here is its own fresh qTD, not a continuation of the last
 * one's overlay state). Returns 0 on success (out_len may be less than
 * len on a short read), -1 on stall/error, -2 on timeout. */
static int ehci_bulk_transfer(uint8_t addr, uint8_t endpoint, int data_in, uint16_t mps,
                               void *buf, uint32_t len, uint8_t *toggle, uint32_t *out_len) {
    if (!ehci.present || !ehci.ctrl) {
        return -1;
    }
    if (mps == 0) {
        mps = 512; /* high-speed bulk default */
    }
    if (out_len) {
        *out_len = 0;
    }
    if (len == 0) {
        return 0;
    }

    ehci_qh_t *qh = &ehci.ctrl->head_qh;
    qh->endpoint_chars = QH_CHAR_HEAD | QH_CHAR_DTC | QH_CHAR_SPEED_HIGH
        | ((uint32_t)addr) | ((uint32_t)endpoint << QH_CHAR_ENDPT_SHIFT)
        | ((uint32_t)mps << QH_CHAR_MPL_SHIFT);

    ehci_qtd_t td;
    memset(&td, 0, sizeof(td));
    td.next_qtd = EHCI_PTR_TERMINATE;
    td.alt_next_qtd = EHCI_PTR_TERMINATE;
    if (ehci_qtd_set_buffer(&td, buf, len) != 0) {
        return -1;
    }
    td.token = QTD_TOK_STATUS_ACTIVE | QTD_TOK_CERR_3
        | (data_in ? QTD_TOK_PID_IN : QTD_TOK_PID_OUT)
        | (len << QTD_TOK_BYTES_SHIFT) | (uint32_t)((*toggle & 1) ? QTD_TOK_DT : 0);
    memcpy((void *)&ehci.ctrl->td[0], &td, sizeof(td));
    qh->next_qtd = (uint32_t)(uintptr_t)&ehci.ctrl->td[0];
    qh->alt_next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;

    uint32_t bytes_left = 0;
    int rc = ehci_wait_overlay(&bytes_left);
    qh->next_qtd = EHCI_PTR_TERMINATE;
    qh->token = 0;
    if (rc != 0) {
        return rc;
    }

    uint32_t actual = len - bytes_left;
    uint32_t packets = (actual + mps - 1) / mps;
    if (packets == 0) {
        packets = 1; /* a genuine zero-length packet still consumes the toggle */
    }
    *toggle = (uint8_t)((*toggle + packets) & 1);
    if (out_len) {
        *out_len = actual;
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
            dev->bulk_in_toggle = 0;
            dev->bulk_out_toggle = 0;
        }
    }
}

/* ---- USB Mass Storage: Bulk-Only Transport + minimal SCSI ----
 *
 * Built on top of the bulk pipes enumeration already found above
 * (ehci_bulk_transfer, dev->bulk_in_ep/out_ep). CBW/CSW framing and the
 * SCSI command set are byte-for-byte identical to kernel/uhci.c's own
 * copy of this section -- USB Mass Storage Bulk-Only Transport doesn't
 * vary by host controller, only the bulk-transfer primitive underneath
 * it does. Duplicated rather than factored into a shared module for now
 * (see ROADMAP.md note on this); kept deliberately parallel to UHCI's
 * naming/shape so a future dedup pass is mechanical. */

static void ehci_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t ehci_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t ehci_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Prints a fixed-length SCSI text field (space-padded ASCII, e.g.
 * INQUIRY's vendor/product strings) trimmed of trailing spaces, with any
 * non-printable byte shown as '.'. Identical to kernel/uhci.c's copy. */
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

#define USB_BOT_CBW_LEN 31
#define USB_BOT_CSW_LEN 13
#define USB_BOT_CBW_SIG 0x43425355u /* "USBC" */
#define USB_BOT_CSW_SIG 0x53425355u /* "USBS" */

static uint32_t ehci_msd_next_tag = 1;

/* First configured device with both bulk endpoints found -- no hub
 * support, so at most EHCI_MAX_PORTS devices exist and there's no
 * enumeration/LUN concept beyond "the one mass-storage device". */
static ehci_device_t *ehci_msd_device(void) {
    for (int p = 0; p < ehci.n_ports; p++) {
        ehci_device_t *dev = &ehci.devices[p];
        if (dev->valid && dev->configured && dev->bulk_in_ep && dev->bulk_out_ep) {
            return dev;
        }
    }
    return 0;
}

/* Runs one Bulk-Only Transport command: CBW (wrapping `cdb`, cdb_len
 * bytes) out, an optional data stage (`data_len` bytes, direction
 * `data_in`) through the matching bulk pipe, then the CSW in. Validates
 * the CSW signature and that its tag echoes the CBW's. Returns 0 on CSW
 * status "passed" (0x00), -1 on CSW "failed"/"phase error" or a
 * signature/tag mismatch, -2 on a transfer error/timeout on any of the
 * three stages. Identical logic to kernel/uhci.c's uhci_msd_command. */
static int ehci_msd_command(ehci_device_t *dev, const uint8_t *cdb, uint8_t cdb_len,
                             void *data, uint32_t data_len, int data_in, uint32_t *out_len) {
    if (!dev || cdb_len == 0 || cdb_len > 16) {
        return -2;
    }

    uint32_t tag = ehci_msd_next_tag++;

    uint8_t cbw[USB_BOT_CBW_LEN];
    memset(cbw, 0, sizeof(cbw));
    ehci_put_le32(cbw + 0, USB_BOT_CBW_SIG);
    ehci_put_le32(cbw + 4, tag);
    ehci_put_le32(cbw + 8, data_len);
    cbw[12] = (uint8_t)((data_len > 0 && data_in) ? 0x80 : 0x00);
    cbw[13] = 0; /* LUN 0 -- no hub/multi-LUN support */
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);

    if (ehci_bulk_transfer(dev->address, dev->bulk_out_ep, 0, dev->bulk_out_mps,
            cbw, USB_BOT_CBW_LEN, &dev->bulk_out_toggle, 0) != 0) {
        return -2;
    }

    uint32_t transferred = 0;
    if (data_len > 0) {
        uint8_t *pipe_toggle = data_in ? &dev->bulk_in_toggle : &dev->bulk_out_toggle;
        uint8_t pipe_ep = data_in ? dev->bulk_in_ep : dev->bulk_out_ep;
        uint16_t pipe_mps = data_in ? dev->bulk_in_mps : dev->bulk_out_mps;
        if (ehci_bulk_transfer(dev->address, pipe_ep, data_in, pipe_mps,
                data, data_len, pipe_toggle, &transferred) != 0) {
            return -2;
        }
    }

    uint8_t csw[USB_BOT_CSW_LEN];
    uint32_t csw_len = 0;
    if (ehci_bulk_transfer(dev->address, dev->bulk_in_ep, 1, dev->bulk_in_mps,
            csw, USB_BOT_CSW_LEN, &dev->bulk_in_toggle, &csw_len) != 0 || csw_len < USB_BOT_CSW_LEN) {
        return -2;
    }

    if (ehci_get_le32(csw + 0) != USB_BOT_CSW_SIG || ehci_get_le32(csw + 4) != tag) {
        return -1;
    }

    if (out_len) {
        *out_len = transferred;
    }
    return (csw[12] == 0) ? 0 : -1;
}

/* SCSI TEST UNIT READY (6-byte CDB, no data stage). */
static int ehci_msd_test_unit_ready(ehci_device_t *dev) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x00;
    return ehci_msd_command(dev, cdb, 6, 0, 0, 0, 0);
}

/* SCSI INQUIRY (6-byte CDB, data IN). `out_size` is capped to 36 bytes,
 * the standard INQUIRY data length every SCSI device supports. */
static int ehci_msd_inquiry(ehci_device_t *dev, uint8_t *out, uint32_t out_size) {
    uint8_t len = (uint8_t)(out_size < 36 ? out_size : 36);
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = len;
    return ehci_msd_command(dev, cdb, 6, out, len, 1, 0);
}

/* SCSI READ CAPACITY(10) (10-byte CDB, 8 bytes data IN: last valid LBA
 * and block size, both big-endian per SCSI convention). */
static int ehci_msd_read_capacity10(ehci_device_t *dev, uint32_t *out_last_lba, uint32_t *out_block_size) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    uint8_t data[8];
    uint32_t got = 0;
    int rc = ehci_msd_command(dev, cdb, 10, data, 8, 1, &got);
    if (rc != 0) {
        return rc;
    }
    if (got < 8) {
        return -2;
    }
    if (out_last_lba) {
        *out_last_lba = ehci_get_be32(data + 0);
    }
    if (out_block_size) {
        *out_block_size = ehci_get_be32(data + 4);
    }
    return 0;
}

/* SCSI READ(10): `count` blocks of `block_size` bytes starting at `lba`
 * into `buf`. */
static int ehci_msd_read10(ehci_device_t *dev, uint32_t lba, uint16_t count, uint32_t block_size, void *buf) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    return ehci_msd_command(dev, cdb, 10, buf, (uint32_t)count * block_size, 1, 0);
}

/* SCSI WRITE(10): same layout/caps as ehci_msd_read10, opposite direction. */
static int ehci_msd_write10(ehci_device_t *dev, uint32_t lba, uint16_t count, uint32_t block_size, const void *buf) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    return ehci_msd_command(dev, cdb, 10, (void *)buf, (uint32_t)count * block_size, 0, 0);
}

/* ---- block_device_t integration ----
 *
 * Mirrors kernel/ata.c's ata_block_read/write + ata_primary_master shape
 * exactly (same as kernel/uhci.c's own equivalent), so
 * partition_scan_mbr/vfs_mount need zero changes to work on a USB stick
 * behind EHCI. One sector per SCSI command, same read-modify-write
 * handling for partial-sector I/O as kernel/uhci.c/kernel/ata.c. */

#define EHCI_MSD_MAX_BLOCK_SIZE 4096

static block_device_t ehci_msd_blockdev;
static int ehci_msd_blockdev_valid = 0;

static uint32_t ehci_msd_block_read(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ehci_device_t *dev = (ehci_device_t *)device->driver_data;
    if (!dev || !buffer) {
        return 0;
    }

    uint32_t block_size = device->block_size;
    uint8_t sector[EHCI_MSD_MAX_BLOCK_SIZE];
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
        if (ehci_msd_read10(dev, lba, 1, block_size, sector) != 0) {
            break;
        }
        memcpy(buffer + done, sector + sector_offset, chunk);
        done += chunk;
    }
    return done;
}

static uint32_t ehci_msd_block_write(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ehci_device_t *dev = (ehci_device_t *)device->driver_data;
    if (!dev || !buffer) {
        return 0;
    }

    uint32_t block_size = device->block_size;
    uint8_t sector[EHCI_MSD_MAX_BLOCK_SIZE];
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
            if (ehci_msd_read10(dev, lba, 1, block_size, sector) != 0) {
                break;
            }
            memcpy(sector + sector_offset, buffer + done, chunk);
            if (ehci_msd_write10(dev, lba, 1, block_size, sector) != 0) {
                break;
            }
        } else {
            if (ehci_msd_write10(dev, lba, 1, block_size, buffer + done) != 0) {
                break;
            }
        }
        done += chunk;
    }
    return done;
}

int ehci_msd_init(void) {
    ehci_msd_blockdev_valid = 0;

    ehci_device_t *dev = ehci_msd_device();
    if (!dev) {
        return -1;
    }

    uint32_t last_lba = 0, block_size = 0;
    if (ehci_msd_read_capacity10(dev, &last_lba, &block_size) != 0) {
        return -1;
    }
    if (block_size == 0 || block_size > EHCI_MSD_MAX_BLOCK_SIZE) {
        return -1;
    }

    memset(&ehci_msd_blockdev, 0, sizeof(ehci_msd_blockdev));
    ehci_msd_blockdev.name = "usbmsd_ehci0";
    ehci_msd_blockdev.block_size = block_size;
    ehci_msd_blockdev.block_count = last_lba + 1;
    ehci_msd_blockdev.driver_data = dev;
    ehci_msd_blockdev.read = ehci_msd_block_read;
    ehci_msd_blockdev.write = ehci_msd_block_write;
    ehci_msd_blockdev_valid = 1;
    return 0;
}

block_device_t *ehci_msd_primary(void) {
    return ehci_msd_blockdev_valid ? &ehci_msd_blockdev : 0;
}

static char ehci_msd_buf[512];

/* Runs a diagnostic pass over the attached mass-storage device and
 * formats the result: INQUIRY, TEST UNIT READY, READ CAPACITY(10), then
 * a READ(10) of LBA 0. All read-only/safe against real hardware.
 * with_write_test additionally does a WRITE(10)+READ(10) roundtrip at a
 * scratch LBA -- same caveats as kernel/uhci.c's uhci_msd_test_string. */
const char *ehci_msd_test_string(int with_write_test) {
    uint32_t pos = 0;
    memset(ehci_msd_buf, 0, sizeof(ehci_msd_buf));

    ehci_device_t *dev = ehci_msd_device();
    if (!dev) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): no configured mass-storage device found.\n");
        return ehci_msd_buf;
    }

    uint8_t inq[36];
    memset(inq, 0, sizeof(inq));
    if (ehci_msd_inquiry(dev, inq, sizeof(inq)) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): INQUIRY failed.\n");
        return ehci_msd_buf;
    }
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): INQUIRY ok, vendor='");
    append_scsi_text(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), inq + 8, 8);
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "' product='");
    append_scsi_text(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), inq + 16, 16);
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "'\n");

    int tur_rc = ehci_msd_test_unit_ready(dev);
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): TEST UNIT READY ");
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), tur_rc == 0 ? "ready\n" : "not ready\n");

    uint32_t last_lba = 0, block_size = 0;
    if (ehci_msd_read_capacity10(dev, &last_lba, &block_size) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): READ CAPACITY(10) failed.\n");
        return ehci_msd_buf;
    }
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): capacity 0x");
    append_hex32(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), last_lba + 1);
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), " blocks x 0x");
    append_hex32(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), block_size);
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), " bytes\n");

    if (block_size != 512) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): block size != 512, skipping READ(10)/WRITE(10) checks.\n");
        return ehci_msd_buf;
    }

    uint8_t sector[512];
    if (ehci_msd_read10(dev, 0, 1, 512, sector) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): READ(10) LBA 0 failed.\n");
        return ehci_msd_buf;
    }
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): READ(10) LBA 0 ok.\n");

    if (!with_write_test) {
        return ehci_msd_buf;
    }

    uint8_t original[512];
    if (ehci_msd_read10(dev, 100, 1, 512, original) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): WRITE(10) test: READ(10) LBA 100 (baseline) failed.\n");
        return ehci_msd_buf;
    }
    uint8_t pattern[512];
    memset(pattern, 0xA5, sizeof(pattern));
    if (ehci_msd_write10(dev, 100, 1, 512, pattern) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): WRITE(10) LBA 100 failed.\n");
        return ehci_msd_buf;
    }
    uint8_t verify[512];
    if (ehci_msd_read10(dev, 100, 1, 512, verify) != 0) {
        append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): WRITE(10) test: readback failed.\n");
        return ehci_msd_buf;
    }
    int match = memcmp(verify, pattern, sizeof(pattern)) == 0;
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), "USB MSD (EHCI): WRITE(10) roundtrip ");
    append_str(ehci_msd_buf, &pos, sizeof(ehci_msd_buf), match ? "ok\n" : "MISMATCH\n");
    ehci_msd_write10(dev, 100, 1, 512, original); /* restore, best-effort */

    return ehci_msd_buf;
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
            append_str(ehci_status_buf, &pos, sizeof(ehci_status_buf), "\n");
        }
    }

    return ehci_status_buf;
}
