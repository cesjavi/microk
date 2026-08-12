#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "task.h"
#include "video.h"
#include "vmm.h"
#include "string.h"

#define E1000_MMIO_VMEM 0xFE000000

#define PCI_CLASS_NETWORK 0x02
#define PCI_SUBCLASS_ETHERNET 0x00
#define E1000_VENDOR_INTEL 0x8086

#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_RCTL   0x0100
#define E1000_REG_TCTL   0x0400
#define E1000_REG_TIPG   0x0410
#define E1000_REG_RDBAL  0x2800
#define E1000_REG_RDBAH  0x2804
#define E1000_REG_RDLEN  0x2808
#define E1000_REG_RDH    0x2810
#define E1000_REG_RDT    0x2818
#define E1000_REG_TDBAL  0x3800
#define E1000_REG_TDBAH  0x3804
#define E1000_REG_TDLEN  0x3808
#define E1000_REG_TDH    0x3810
#define E1000_REG_TDT    0x3818
#define E1000_REG_RAL0   0x5400
#define E1000_REG_RAH0   0x5404

#define E1000_RCTL_EN    (1u << 1)
#define E1000_RCTL_UPE   (1u << 3)
#define E1000_RCTL_MPE   (1u << 4)
#define E1000_RCTL_BAM   (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN    (1u << 1)
#define E1000_TCTL_PSP   (1u << 3)
#define E1000_CTRL_SLU   (1u << 6)

#define E1000_TX_CMD_EOP  0x01
#define E1000_TX_CMD_IFCS 0x02
#define E1000_TX_CMD_RS   0x08
#define E1000_TX_STA_DD   0x01
#define E1000_RX_STA_DD   0x01

#define E1000_RX_DESC_COUNT 16
#define E1000_TX_DESC_COUNT 8
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 2048

static net_config_t net_config;
static char net_status_buf[256];
static char net_llm_status_buf[160];
static int net_llm_service_on = 0;
static uint16_t net_llm_service_port = 1234;
static char net_llm_service_token[33]; /* empty = no auth required */

/* Remote shell (RSH) UDP service */
#define NET_RSH_PORT_DEFAULT 2323
static char     net_rsh_status_buf[192];
static int      net_rsh_enabled = 0;
static uint16_t net_rsh_port    = NET_RSH_PORT_DEFAULT;
static char     net_rsh_token[33]; /* empty = no auth required */
static int e1000_rings_ready = 0;
static uint32_t e1000_rx_cur = 0;
static uint32_t e1000_tx_tail = 0;

// Ping reply tracking — set by net_handle_icmp() when the expected reply arrives
static int      ping_reply_received = 0;
static uint16_t ping_pending_id  = 0;
static uint16_t ping_pending_seq = 0;

// DNS client state (type-independent, so can stay here)
#define DNS_CLIENT_PORT 5300
static int      dns_reply_received = 0;
static uint16_t dns_txid           = 0;
static uint8_t  dns_resolved_ip[4];

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

static e1000_rx_desc_t *e1000_rx_descs;
static e1000_tx_desc_t *e1000_tx_descs;
static uint8_t *e1000_rx_buffers;
static uint8_t *e1000_tx_buffers;

typedef struct {
    uint8_t dest[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed)) eth_header_t;

typedef struct {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t len;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t ttl;
    uint8_t proto;
    uint16_t chksum;
    uint8_t src[4];
    uint8_t dest[4];
} __attribute__((packed)) ip_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t len;
    uint16_t chksum;
} __attribute__((packed)) udp_header_t;

/* TCP: minimal header, no options (data_offset always 5, 20-byte header).
 * Client-only, single connection at a time -- see the "TCP client" section
 * below for the state machine this drives. */
typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; /* high nibble = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t chksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* IPv4 pseudo-header, summed into the TCP checksum alongside the segment
 * itself per RFC 793 -- unlike UDP's checksum (optional in IPv4, left as 0
 * on TX by net_send_udp), TCP's is mandatory. */
typedef struct {
    uint8_t  src[4];
    uint8_t  dest[4];
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} __attribute__((packed)) tcp_pseudo_header_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t chksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_header_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* DHCP / BOOTP */
typedef struct {
    uint8_t  op;          /* 1=request 2=reply */
    uint8_t  htype;       /* 1=Ethernet */
    uint8_t  hlen;        /* 6 */
    uint8_t  hops;
    uint32_t xid;         /* transaction id (network byte order) */
    uint16_t secs;
    uint16_t flags;       /* 0x8000 = broadcast */
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];   /* offered IP */
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];  /* client MAC + padding */
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;       /* 0x63825363 in network byte order */
    /* options follow immediately */
} __attribute__((packed)) dhcp_packet_t;

#define DHCP_PORT_SERVER  67
#define DHCP_PORT_CLIENT  68
#define DHCP_MAGIC_BE     0x63825363u /* as seen in network byte stream */

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

typedef enum {
    DHCP_IDLE = 0,
    DHCP_DISCOVERING,
    DHCP_REQUESTING,
    DHCP_BOUND
} dhcp_state_t;

// DHCP client state
static dhcp_state_t dhcp_client_state = DHCP_IDLE;
static uint32_t     dhcp_xid          = 0;
static uint8_t      dhcp_offered_ip[4];
static uint8_t      dhcp_server_id[4];
static uint8_t      dhcp_offered_mask[4];
static uint8_t      dhcp_offered_gw[4];
static uint8_t      dhcp_offered_dns[4];

/* Endian helpers */
static inline uint16_t ntohs(uint16_t netshort) {
    return (uint16_t)((netshort << 8) | (netshort >> 8));
}

static inline uint16_t htons(uint16_t hostshort) {
    return (uint16_t)((hostshort << 8) | (hostshort >> 8));
}

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | (((h >> 8) & 0xFF) << 16) |
           (((h >> 16) & 0xFF) << 8) | ((h >> 24) & 0xFF);
}

/* Byte-swap is its own inverse, same as ntohs/htons above. */
static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(E1000_MMIO_VMEM + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(E1000_MMIO_VMEM + reg) = value;
}

/* ARP Packet format */
typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    uint8_t sender_mac[6];
    uint8_t sender_ip[4];
    uint8_t target_mac[6];
    uint8_t target_ip[4];
} __attribute__((packed)) arp_packet_t;

/* ARP Cache */
typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
} arp_entry_t;

#define ARP_CACHE_SIZE 16
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* UDP port → handler dispatch table */
#define UDP_BINDINGS_MAX 8
typedef struct {
    uint16_t port;
    net_udp_handler_t handler;
    int active;
} udp_binding_t;
static udp_binding_t udp_bindings[UDP_BINDINGS_MAX];

#include "llm.h"

/* Static formatting helper prototypes to prevent order warnings */
static void append_str(char *out, uint32_t *pos, uint32_t max, const char *text);
static void append_ip(char *out, uint32_t *pos, uint32_t max, const uint8_t ip[4]);
static void append_mac(char *out, uint32_t *pos, uint32_t max, const uint8_t mac[6]);
static void append_hex8(char *out, uint32_t *pos, uint32_t max, uint8_t value);
static void append_hex32(char *out, uint32_t *pos, uint32_t max, uint32_t value);
static int e1000_send_frame(const uint8_t *frame, uint16_t len);
static void net_llm_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                                 const uint8_t *payload, uint16_t len);
static void dhcp_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                              const uint8_t *payload, uint16_t len);
static void dns_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                             const uint8_t *payload, uint16_t len);
static void net_rsh_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                                 const uint8_t *payload, uint16_t len);

static int text_equals(const char *text, uint32_t len, const char *expected) {
    uint32_t expected_len = strlen(expected);
    return len == expected_len && memcmp(text, expected, expected_len) == 0;
}

static int text_starts_with(const char *text, uint32_t len, const char *prefix) {
    uint32_t prefix_len = strlen(prefix);
    return len >= prefix_len && memcmp(text, prefix, prefix_len) == 0;
}

static int parse_u16(const char *text, uint16_t *out) {
    uint32_t value = 0;
    const char *p = text;

    if (!text || !text[0] || !out) {
        return 0;
    }
    while (*p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > 65535) {
            return 0;
        }
        p++;
    }
    if (value == 0) {
        return 0;
    }
    *out = (uint16_t)value;
    return 1;
}

static uint16_t net_checksum16(const void *data, uint32_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len) {
        sum += ((uint16_t)bytes[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Split form of net_checksum16 for TCP, whose checksum covers two
 * non-contiguous regions (the IPv4 pseudo-header, then the TCP segment) --
 * accumulate over each with net_checksum16_partial, then fold/complement
 * once at the end with net_checksum16_finish. net_checksum16 itself is left
 * untouched since ICMP/IP callers only ever sum one contiguous buffer. */
static uint32_t net_checksum16_partial(const void *data, uint32_t len, uint32_t sum) {
    const uint8_t *bytes = (const uint8_t *)data;
    while (len > 1) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len) {
        sum += ((uint16_t)bytes[0] << 8);
    }
    return sum;
}

static uint16_t net_checksum16_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void copy_limited_text(char *out, uint32_t out_size, const char *text) {
    uint32_t pos = 0;

    if (!out || out_size == 0) {
        return;
    }
    while (text && text[pos] && pos + 1 < out_size) {
        out[pos] = text[pos];
        pos++;
    }
    out[pos] = '\0';
}

static uint32_t trim_packet_text(char *text, uint32_t len) {
    while (len > 0 && (text[len - 1] == '\0' || text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
    return len;
}

static void net_llm_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                                 const uint8_t *payload, uint16_t len) {
    char request_buf[128];
    char response[256];
    uint32_t to_copy = len > 127 ? 127 : len;
    memcpy(request_buf, payload, to_copy);
    request_buf[to_copy] = '\0';
    to_copy = trim_packet_text(request_buf, to_copy);

    klog("Network LLM request: ");
    klog(request_buf);

    if (!net_llm_service_handle_text(request_buf, to_copy, response, sizeof(response))) return;

    klog("\nNetwork LLM response: ");
    klog(response);

    uint16_t resp_len = (uint16_t)strlen(response);
    if (net_send_udp(src_ip, src_port, net_llm_service_port, (const uint8_t *)response, resp_len)) {
        klog("Network LLM UDP response sent.");
    } else {
        klog("Network LLM UDP response send failed.");
    }
}

int net_llm_service_set_enabled(int enabled) {
    if (enabled) {
        net_llm_service_on = 1;
        net_udp_bind(net_llm_service_port, net_llm_udp_handler);
    } else {
        net_llm_service_on = 0;
        net_udp_unbind(net_llm_service_port);
    }
    return 1;
}

int net_llm_service_enabled(void) {
    return net_llm_service_on;
}

int net_llm_service_set_port(uint16_t port) {
    if (port == 0) return 0;
    if (net_llm_service_on) net_udp_unbind(net_llm_service_port);
    net_llm_service_port = port;
    if (net_llm_service_on) net_udp_bind(net_llm_service_port, net_llm_udp_handler);
    return 1;
}

const char *net_llm_service_status_string(void) {
    uint32_t pos = 0;
    char port_buf[8];

    memset(net_llm_status_buf, 0, sizeof(net_llm_status_buf));
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), "LLM net service: ");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_llm_service_on ? "on" : "off");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), " udp_port=");
    itoa(net_llm_service_port, port_buf, 10);
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), port_buf);
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_llm_service_token[0] ? " auth=on" : " auth=off");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_config.link_up ? " link=up" : " link=down");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_config.nic_present ? " nic=yes" : " nic=no");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), "\n");
    return net_llm_status_buf;
}

int net_llm_service_set_token(const char *token) {
    if (!token || !token[0] || strcmp(token, "off") == 0) {
        net_llm_service_token[0] = '\0';
    } else {
        uint32_t len = strlen(token);
        if (len > 32) len = 32;
        memcpy(net_llm_service_token, token, len);
        net_llm_service_token[len] = '\0';
    }
    return 1;
}

int net_llm_service_handle_text(const char *request, uint32_t request_len, char *response, uint32_t response_size) {
    char prompt[128];

    if (!request || !response || response_size == 0) {
        return 0;
    }
    response[0] = '\0';

    if (!net_llm_service_on) {
        copy_limited_text(response, response_size, "ERR service disabled\n");
        return 1;
    }

    /* Token auth: if configured, request must start with "<token> " */
    if (net_llm_service_token[0]) {
        uint32_t tlen = strlen(net_llm_service_token);
        if (request_len <= tlen + 1 ||
            memcmp(request, net_llm_service_token, tlen) != 0 ||
            request[tlen] != ' ') {
            copy_limited_text(response, response_size, "ERR unauthorized\n");
            return 1;
        }
        request     += tlen + 1;
        request_len -= tlen + 1;
    }

    if (text_equals(request, request_len, "PING")) {
        copy_limited_text(response, response_size, "PONG\n");
        return 1;
    }
    if (text_equals(request, request_len, "STATUS")) {
        copy_limited_text(response, response_size, llm_status_string());
        return 1;
    }
    if (text_equals(request, request_len, "INFO")) {
        copy_limited_text(response, response_size, llm_info_string());
        return 1;
    }
    if (text_starts_with(request, request_len, "ASK ")) {
        uint32_t prompt_len = request_len - 4;
        if (prompt_len >= sizeof(prompt)) {
            prompt_len = sizeof(prompt) - 1;
        }
        memcpy(prompt, request + 4, prompt_len);
        prompt[prompt_len] = '\0';
        llm_inference(prompt, response);
        return 1;
    }

    copy_limited_text(response, response_size, "ERR expected PING, STATUS, INFO or ASK <prompt>\n");
    return 1;
}

static int e1000_send_frame(const uint8_t *frame, uint16_t len) {
    uint32_t current_tail = e1000_tx_tail;
    if (!e1000_rings_ready || !frame || len == 0 || len > E1000_TX_BUF_SIZE) {
        return 0;
    }

    volatile e1000_tx_desc_t *desc = &e1000_tx_descs[current_tail];
    if (!(desc->status & E1000_TX_STA_DD)) {
        return 0;
    }

    uint8_t *buffer = e1000_tx_buffers + (current_tail * E1000_TX_BUF_SIZE);
    memcpy(buffer, frame, len);
    desc->addr = (uint32_t)buffer;
    desc->length = len;
    desc->cso = 0;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    e1000_tx_tail = (e1000_tx_tail + 1) % E1000_TX_DESC_COUNT;
    e1000_write(E1000_REG_TDT, e1000_tx_tail);

    for (uint32_t timeout = 0; timeout < 100000; timeout++) {
        if (desc->status & E1000_TX_STA_DD) {
            return 1;
        }
    }
    return 0;
}

/* Forward declaration — defined below the ARP section */
static int net_send_arp_request(const uint8_t target_ip[4]);

/* Resolves dest_ip to a MAC address: broadcast shortcut, then ARP cache,
 * then an ARP request polled for up to 1s. Shared by net_send_udp and the
 * TCP sender below -- extracted rather than duplicated since both need the
 * exact same cache-then-request-then-poll sequence. Returns 1 and fills
 * out_mac on success, 0 on failure (e.g. ARP timeout). */
static int net_resolve_mac(const uint8_t dest_ip[4], uint8_t out_mac[6]) {
    static const uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t broadcast_ip[4]  = {0xFF,0xFF,0xFF,0xFF};

    if (memcmp(dest_ip, broadcast_ip, 4) == 0) {
        memcpy(out_mac, broadcast_mac, 6);
        return 1;
    }

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, dest_ip, 4) == 0) {
            memcpy(out_mac, arp_cache[i].mac, 6);
            return 1;
        }
    }

    extern uint32_t timer_get_ticks(void);
    net_send_arp_request(dest_ip);
    uint32_t deadline = timer_get_ticks() + 100;
    while (timer_get_ticks() < deadline) {
        net_poll();
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && memcmp(arp_cache[i].ip, dest_ip, 4) == 0) {
                memcpy(out_mac, arp_cache[i].mac, 6);
                return 1;
            }
        }
    }
    return 0;
}

/* net_send_udp: send a UDP datagram to dest_ip:dest_port from src_port.
 * Handles ARP resolution automatically; dest_ip=255.255.255.255 uses broadcast MAC. */
int net_send_udp(const uint8_t dest_ip[4], uint16_t dest_port, uint16_t src_port,
                 const uint8_t *payload, uint16_t payload_len) {
    uint8_t dest_mac[6];

    if (!e1000_rings_ready || !net_config.nic_present || !dest_ip) return 0;

    if (!net_resolve_mac(dest_ip, dest_mac)) {
        return 0;
    }

    uint16_t udp_data_len = payload_len;
    /* Do the length math in 32 bits and bounds-check *before* truncating to
     * the on-wire uint16_t ip_len field. udp_data_len can be up to 65535,
     * so sizeof(ip_header_t)+sizeof(udp_header_t)+udp_data_len can exceed
     * 65535 and wrap a uint16_t back down to a small value -- which would
     * pass the frame_len check below while the memcpy() further down still
     * copies the full, untruncated udp_data_len into the fixed-size stack
     * buffer `frame`. */
    uint32_t ip_len32 = (uint32_t)sizeof(ip_header_t) + sizeof(udp_header_t) + udp_data_len;
    uint32_t frame_len = sizeof(eth_header_t) + ip_len32;
    if (frame_len > E1000_TX_BUF_SIZE) return 0;
    uint16_t ip_len = (uint16_t)ip_len32; /* safe: frame_len <= E1000_TX_BUF_SIZE (2048) above */

    uint8_t frame[E1000_TX_BUF_SIZE];
    memset(frame, 0, frame_len);

    eth_header_t *eth  = (eth_header_t *)frame;
    ip_header_t  *ip   = (ip_header_t *)(frame + sizeof(eth_header_t));
    udp_header_t *udp  = (udp_header_t *)(frame + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t      *data = frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);

    memcpy(eth->dest, dest_mac, 6);
    memcpy(eth->src, net_config.mac, 6);
    eth->type = htons(0x0800);

    ip->version_ihl = 0x45;
    ip->tos         = 0;
    ip->len         = htons(ip_len);
    ip->id          = htons(0x4D4B); // 'MK'
    ip->flags_offset = 0;
    ip->ttl         = 64;
    ip->proto       = 17; // UDP
    ip->chksum      = 0;
    memcpy(ip->src,  net_config.ip, 4);
    memcpy(ip->dest, dest_ip,       4);
    ip->chksum = htons(net_checksum16(ip, sizeof(ip_header_t)));

    udp->src_port  = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->len       = htons((uint16_t)(sizeof(udp_header_t) + udp_data_len));
    udp->chksum    = 0; // optional for IPv4

    if (payload && udp_data_len) memcpy(data, payload, udp_data_len);

    if (frame_len < 60) { memset(frame + frame_len, 0, 60 - frame_len); frame_len = 60; }
    return e1000_send_frame(frame, (uint16_t)frame_len);
}

/* ---- TCP client (single connection, synchronous/polled) ----
 *
 * Enough TCP to drive one blocking request/response exchange (net_http_get,
 * below) -- not a general sockets API. One connection at a time, no
 * retransmission queue: outbound segments (SYN, request data, FIN) are sent
 * once and rely on the caller's own poll-with-deadline loop timing out if
 * they're lost, the same way net_ping/net_dns_resolve already handle ARP/DNS
 * timeouts. Inbound reliability is the remote peer's problem, as with any
 * TCP receiver -- we just need to ACK promptly, which we do.
 *
 * TCP_WINDOW is a fixed advertised window rather than one derived from
 * actual buffer headroom -- simpler, and fine for the small, short-lived
 * transfers this is built for; data arriving past the caller's receive
 * buffer is simply not copied (but still ACKed, so the connection doesn't
 * stall waiting for us). */
#define TCP_WINDOW 8192

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_DONE /* FIN or RST seen; net_http_get is finished with this connection */
} tcp_conn_state_t;

typedef struct {
    tcp_conn_state_t state;
    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t local_seq;  /* next sequence number *we* will send */
    uint32_t remote_seq; /* next sequence number expected from remote == our ack field */
    int      rst_received;
    int      fin_received;
} tcp_conn_t;

static tcp_conn_t tcp_conn;
static uint16_t   tcp_next_local_port = 49152; /* ephemeral range, RFC 6335 */

/* Receive buffer for the connection currently in ESTABLISHED state -- set
 * by net_http_get before it starts polling, written to by net_handle_tcp as
 * segments arrive. */
static uint8_t  *tcp_recv_buf      = 0;
static uint32_t  tcp_recv_buf_size = 0;
static uint32_t  tcp_recv_len      = 0;

/* Builds and sends one TCP segment for the current tcp_conn: Ethernet + IP +
 * TCP header (no options) + optional payload, checksummed per RFC 793
 * (pseudo-header + segment). Does not touch tcp_conn.local_seq -- callers
 * advance it themselves by exactly what SYN/FIN/data each consume, since
 * this function may be called to retransmit the *same* sequence number
 * (e.g. re-ACKing) as well as to advance it. */
static int net_send_tcp_segment(uint8_t flags, const uint8_t *payload, uint16_t payload_len) {
    uint8_t dest_mac[6];
    if (!e1000_rings_ready || !net_config.nic_present) return 0;
    if (!net_resolve_mac(tcp_conn.remote_ip, dest_mac)) return 0;

    uint32_t ip_len32 = (uint32_t)sizeof(ip_header_t) + sizeof(tcp_header_t) + payload_len;
    uint32_t frame_len = sizeof(eth_header_t) + ip_len32;
    if (frame_len > E1000_TX_BUF_SIZE) return 0;
    uint16_t ip_len = (uint16_t)ip_len32;

    uint8_t frame[E1000_TX_BUF_SIZE];
    memset(frame, 0, frame_len);

    eth_header_t *eth = (eth_header_t *)frame;
    ip_header_t  *ip  = (ip_header_t *)(frame + sizeof(eth_header_t));
    tcp_header_t *tcp = (tcp_header_t *)(frame + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t      *data = frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(tcp_header_t);

    memcpy(eth->dest, dest_mac, 6);
    memcpy(eth->src, net_config.mac, 6);
    eth->type = htons(0x0800);

    ip->version_ihl  = 0x45;
    ip->tos          = 0;
    ip->len          = htons(ip_len);
    ip->id           = htons(0x4D4C); /* 'ML' -- distinct from UDP's 'MK' id */
    ip->flags_offset = 0;
    ip->ttl          = 64;
    ip->proto        = 6; /* TCP */
    ip->chksum       = 0;
    memcpy(ip->src,  net_config.ip,        4);
    memcpy(ip->dest, tcp_conn.remote_ip,   4);
    ip->chksum = htons(net_checksum16(ip, sizeof(ip_header_t)));

    tcp->src_port    = htons(tcp_conn.local_port);
    tcp->dest_port   = htons(tcp_conn.remote_port);
    tcp->seq         = htonl(tcp_conn.local_seq);
    tcp->ack         = htonl((flags & TCP_FLAG_ACK) ? tcp_conn.remote_seq : 0);
    tcp->data_offset = (uint8_t)((sizeof(tcp_header_t) / 4) << 4);
    tcp->flags       = flags;
    tcp->window      = htons(TCP_WINDOW);
    tcp->chksum      = 0;
    tcp->urgent      = 0;

    if (payload && payload_len) {
        memcpy(data, payload, payload_len);
    }

    tcp_pseudo_header_t pseudo;
    memcpy(pseudo.src,  net_config.ip,      4);
    memcpy(pseudo.dest, tcp_conn.remote_ip, 4);
    pseudo.zero    = 0;
    pseudo.proto   = 6;
    pseudo.tcp_len = htons((uint16_t)(sizeof(tcp_header_t) + payload_len));

    uint32_t sum = net_checksum16_partial(&pseudo, sizeof(pseudo), 0);
    sum = net_checksum16_partial(tcp, sizeof(tcp_header_t) + payload_len, sum);
    tcp->chksum = htons(net_checksum16_finish(sum));

    if (frame_len < 60) { memset(frame + frame_len, 0, 60 - frame_len); frame_len = 60; }
    return e1000_send_frame(frame, (uint16_t)frame_len);
}

void arp_cache_add(const uint8_t ip[4], const uint8_t mac[6]) {
    // Check if it already exists
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    
    // Find an empty slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    
    // Fallback: simple FIFO
    static int victim = 0;
    memcpy(arp_cache[victim].ip, ip, 4);
    memcpy(arp_cache[victim].mac, mac, 6);
    arp_cache[victim].valid = 1;
    victim = (victim + 1) % ARP_CACHE_SIZE;
}

int net_udp_bind(uint16_t port, net_udp_handler_t handler) {
    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == port) {
            udp_bindings[i].handler = handler;
            return 1;
        }
    }
    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (!udp_bindings[i].active) {
            udp_bindings[i].port    = port;
            udp_bindings[i].handler = handler;
            udp_bindings[i].active  = 1;
            return 1;
        }
    }
    return 0; // table full
}

void net_udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == port) {
            udp_bindings[i].active = 0;
            return;
        }
    }
}

const char *arp_status_string(void) {
    static char buf[512];
    uint32_t pos = 0;
    memset(buf, 0, sizeof(buf));
    append_str(buf, &pos, sizeof(buf), "ARP Cache:\n");
    int count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            append_str(buf, &pos, sizeof(buf), "  IP: ");
            append_ip(buf, &pos, sizeof(buf), arp_cache[i].ip);
            append_str(buf, &pos, sizeof(buf), "  -> MAC: ");
            append_mac(buf, &pos, sizeof(buf), arp_cache[i].mac);
            append_str(buf, &pos, sizeof(buf), "\n");
            count++;
        }
    }
    if (count == 0) {
        append_str(buf, &pos, sizeof(buf), "  (cache is empty)\n");
    }
    return buf;
}

static int parse_ip4(const char *str, uint8_t out[4]) {
    const char *p = str;
    for (int i = 0; i < 4; i++) {
        uint32_t val = 0;
        if (!p || *p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (uint32_t)(*p++ - '0'); }
        if (val > 255) return 0;
        out[i] = (uint8_t)val;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    return 1;
}

static int net_send_arp_request(const uint8_t target_ip[4]) {
    uint8_t frame[sizeof(eth_header_t) + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + sizeof(eth_header_t));
    static const uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    memcpy(eth->dest, broadcast_mac, 6);
    memcpy(eth->src, net_config.mac, 6);
    eth->type = htons(0x0806);

    arp->hw_type    = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = htons(1); // ARP Request
    memcpy(arp->sender_mac, net_config.mac, 6);
    memcpy(arp->sender_ip,  net_config.ip,  4);
    memset(arp->target_mac, 0, 6);
    memcpy(arp->target_ip,  target_ip, 4);

    return e1000_send_frame(frame, (uint16_t)sizeof(frame));
}

static void net_send_icmp_echo_reply(const eth_header_t *rx_eth, const ip_header_t *rx_ip,
                                      const icmp_header_t *rx_icmp,
                                      const uint8_t *data, uint32_t data_len) {
    uint8_t frame[512];
    eth_header_t  *eth  = (eth_header_t *)frame;
    ip_header_t   *ip   = (ip_header_t *)(frame + sizeof(eth_header_t));
    icmp_header_t *icmp = (icmp_header_t *)((uint8_t *)ip + sizeof(ip_header_t));
    uint8_t *payload    = (uint8_t *)icmp + sizeof(icmp_header_t);

    if (data_len > 56) data_len = 56;
    uint16_t ip_total  = (uint16_t)(sizeof(ip_header_t) + sizeof(icmp_header_t) + data_len);
    uint32_t frame_len = sizeof(eth_header_t) + ip_total;

    memcpy(eth->dest, rx_eth->src, 6);
    memcpy(eth->src,  net_config.mac, 6);
    eth->type = htons(0x0800);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl   = 64;
    ip->proto = 1;
    ip->len   = htons(ip_total);
    ip->id    = rx_ip->id;
    memcpy(ip->src,  net_config.ip, 4);
    memcpy(ip->dest, rx_ip->src,   4);
    ip->chksum = 0;
    ip->chksum = htons(net_checksum16(ip, sizeof(ip_header_t)));

    icmp->type   = ICMP_ECHO_REPLY;
    icmp->code   = 0;
    icmp->id     = rx_icmp->id;
    icmp->seq    = rx_icmp->seq;
    icmp->chksum = 0;
    if (data && data_len) memcpy(payload, data, data_len);
    icmp->chksum = htons(net_checksum16(icmp, sizeof(icmp_header_t) + data_len));

    if (frame_len < 60) { memset(frame + frame_len, 0, 60 - frame_len); frame_len = 60; }
    e1000_send_frame(frame, (uint16_t)frame_len);
}

static int net_send_icmp_echo_request(const uint8_t target_mac[6], const uint8_t target_ip[4],
                                       uint16_t id, uint16_t seq) {
    static const uint8_t ping_data[8] = {'M','K','P','I','N','G',0,0};
    uint8_t frame[512];
    eth_header_t  *eth  = (eth_header_t *)frame;
    ip_header_t   *ip   = (ip_header_t *)(frame + sizeof(eth_header_t));
    icmp_header_t *icmp = (icmp_header_t *)((uint8_t *)ip + sizeof(ip_header_t));
    uint8_t *payload    = (uint8_t *)icmp + sizeof(icmp_header_t);
    const uint32_t data_len = sizeof(ping_data);
    uint16_t ip_total  = (uint16_t)(sizeof(ip_header_t) + sizeof(icmp_header_t) + data_len);
    uint32_t frame_len = sizeof(eth_header_t) + ip_total;

    memcpy(eth->dest, target_mac, 6);
    memcpy(eth->src,  net_config.mac, 6);
    eth->type = htons(0x0800);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl   = 64;
    ip->proto = 1;
    ip->len   = htons(ip_total);
    ip->id    = htons(id);
    memcpy(ip->src,  net_config.ip, 4);
    memcpy(ip->dest, target_ip, 4);
    ip->chksum = 0;
    ip->chksum = htons(net_checksum16(ip, sizeof(ip_header_t)));

    icmp->type   = ICMP_ECHO_REQUEST;
    icmp->code   = 0;
    icmp->id     = htons(id);
    icmp->seq    = htons(seq);
    icmp->chksum = 0;
    memcpy(payload, ping_data, data_len);
    icmp->chksum = htons(net_checksum16(icmp, sizeof(icmp_header_t) + data_len));

    if (frame_len < 60) { memset(frame + frame_len, 0, 60 - frame_len); frame_len = 60; }
    return e1000_send_frame(frame, (uint16_t)frame_len);
}

static void net_handle_icmp(const eth_header_t *eth, const ip_header_t *ip,
                              const uint8_t *icmp_start, uint32_t icmp_len) {
    if (icmp_len < sizeof(icmp_header_t)) return;
    // Only handle packets destined to our IP
    if (memcmp(ip->dest, net_config.ip, 4) != 0) return;

    const icmp_header_t *icmp = (const icmp_header_t *)icmp_start;
    const uint8_t *data = icmp_start + sizeof(icmp_header_t);
    uint32_t data_len   = icmp_len > sizeof(icmp_header_t) ? icmp_len - sizeof(icmp_header_t) : 0;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        net_send_icmp_echo_reply(eth, ip, icmp, data, data_len);
    } else if (icmp->type == ICMP_ECHO_REPLY && icmp->code == 0) {
        if (ntohs(icmp->id) == ping_pending_id && ntohs(icmp->seq) == ping_pending_seq) {
            ping_reply_received = 1;
        }
    }
}

/* Handles one TCP segment against the single active tcp_conn -- everything
 * else (other ports, other remote hosts, a second connection) is silently
 * ignored, since this client only ever has one connection open. No
 * reordering/reassembly: a segment whose sequence number doesn't match
 * exactly what we expect is dropped (the sender's own TCP retransmit timer
 * will resend it -- that's how TCP receivers are supposed to behave, not a
 * shortcut specific to this implementation). */
static void net_handle_tcp(const ip_header_t *ip, const uint8_t *tcp_start, uint32_t tcp_seg_len) {
    if (tcp_seg_len < sizeof(tcp_header_t)) return;
    if (tcp_conn.state == TCP_CLOSED || tcp_conn.state == TCP_DONE) return;

    const tcp_header_t *tcp = (const tcp_header_t *)tcp_start;
    if (ntohs(tcp->dest_port) != tcp_conn.local_port) return;
    if (ntohs(tcp->src_port) != tcp_conn.remote_port) return;
    if (memcmp(ip->src, tcp_conn.remote_ip, 4) != 0) return;

    uint32_t data_offset_bytes = (uint32_t)((tcp->data_offset >> 4) & 0x0F) * 4;
    if (data_offset_bytes < sizeof(tcp_header_t) || data_offset_bytes > tcp_seg_len) return;
    const uint8_t *payload = tcp_start + data_offset_bytes;
    uint32_t payload_len = tcp_seg_len - data_offset_bytes;

    if (tcp->flags & TCP_FLAG_RST) {
        tcp_conn.rst_received = 1;
        tcp_conn.state = TCP_DONE;
        return;
    }

    if (tcp_conn.state == TCP_SYN_SENT) {
        if ((tcp->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)
            && ntohl(tcp->ack) == tcp_conn.local_seq + 1) {
            tcp_conn.local_seq += 1;
            tcp_conn.remote_seq = ntohl(tcp->seq) + 1;
            tcp_conn.state = TCP_ESTABLISHED;
            net_send_tcp_segment(TCP_FLAG_ACK, 0, 0);
        }
        return;
    }

    if (tcp_conn.state != TCP_ESTABLISHED) return;
    if (ntohl(tcp->seq) != tcp_conn.remote_seq) return; /* out of order / retransmit we already have -- drop */

    int has_fin = (tcp->flags & TCP_FLAG_FIN) != 0;
    if (payload_len == 0 && !has_fin) return; /* pure ACK: nothing new to acknowledge, don't ACK an ACK */

    if (payload_len > 0 && tcp_recv_buf && tcp_recv_len < tcp_recv_buf_size) {
        uint32_t space = tcp_recv_buf_size - tcp_recv_len;
        uint32_t copy_len = payload_len < space ? payload_len : space;
        memcpy(tcp_recv_buf + tcp_recv_len, payload, copy_len);
        tcp_recv_len += copy_len;
    }
    tcp_conn.remote_seq += payload_len;

    if (has_fin) {
        tcp_conn.remote_seq += 1;
        tcp_conn.fin_received = 1;
        /* Active close: FIN+ACK back, no TIME_WAIT -- acceptable for a
         * one-shot client that picks a fresh ephemeral port next time. */
        net_send_tcp_segment((uint8_t)(TCP_FLAG_FIN | TCP_FLAG_ACK), 0, 0);
        tcp_conn.local_seq += 1;
        tcp_conn.state = TCP_DONE;
    } else {
        net_send_tcp_segment(TCP_FLAG_ACK, 0, 0);
    }
}

static int net_send_arp_reply(const arp_packet_t *request) {
    uint8_t frame[sizeof(eth_header_t) + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + sizeof(eth_header_t));

    if (!request) {
        return 0;
    }

    memcpy(eth->dest, request->sender_mac, sizeof(eth->dest));
    memcpy(eth->src, net_config.mac, sizeof(eth->src));
    eth->type = htons(0x0806);

    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(2);
    memcpy(arp->sender_mac, net_config.mac, sizeof(arp->sender_mac));
    memcpy(arp->sender_ip, net_config.ip, sizeof(arp->sender_ip));
    memcpy(arp->target_mac, request->sender_mac, sizeof(arp->target_mac));
    memcpy(arp->target_ip, request->sender_ip, sizeof(arp->target_ip));

    return e1000_send_frame(frame, sizeof(frame));
}

static void net_handle_arp(arp_packet_t *arp) {
    if (!arp) return;
    
    uint16_t hw_type = ntohs(arp->hw_type);
    uint16_t proto_type = ntohs(arp->proto_type);
    uint16_t opcode = ntohs(arp->opcode);
    
    if (hw_type != 1 || proto_type != 0x0800) return;
    
    arp_cache_add(arp->sender_ip, arp->sender_mac);
    
    if (opcode == 1 && memcmp(arp->target_ip, net_config.ip, 4) == 0) {
        klog("Network: ARP Request parsed for our local IP address.");
        if (net_send_arp_reply(arp)) {
            klog("Network: ARP Reply queued.");
        } else {
            klog("Network: ARP Reply queue failed.");
        }
    } else if (opcode == 2) {
        klog("Network: ARP Reply received and registered in cache.");
    }
}

void net_handle_packet(uint8_t *data, uint32_t len) {
    if (len < sizeof(eth_header_t)) return;
    eth_header_t *eth = (eth_header_t *)data;

    // Check for ARP (0x0806 swapped is 0x0608)
    if (eth->type == 0x0608) {
        if (len < sizeof(eth_header_t) + sizeof(arp_packet_t)) return;
        net_handle_arp((arp_packet_t *)(data + sizeof(eth_header_t)));
        return;
    }
    
    // Check for IPv4 (0x0800)
    if (eth->type != 0x0008) return; // Network byte order for 0x0800 is 0x0008 on little endian? No, usually 0x0800.
    
    if (len < sizeof(eth_header_t) + sizeof(ip_header_t)) return;
    ip_header_t *ip = (ip_header_t *)(data + sizeof(eth_header_t));

    uint32_t ip_header_len = (ip->version_ihl & 0x0F) * 4;
    if (ip_header_len < sizeof(ip_header_t)) return;
    /* ip_header_len (IHL) is attacker-controlled and can be up to 60 bytes,
     * larger than the minimum eth+ip_header_t frame checked above. Without
     * this, a short frame with an inflated IHL underflows the subtraction
     * below to a huge uint32_t, which then gets treated as a valid ICMP
     * payload length -- reading (and, for echo requests, replying with)
     * stale bytes from beyond the actual received frame. */
    if (len < sizeof(eth_header_t) + ip_header_len) return;

    uint32_t ip_payload_len = len - sizeof(eth_header_t) - ip_header_len;

    /* Ethernet enforces a 60-byte minimum frame size, so any real payload
     * shorter than that arrives zero-padded -- ip_payload_len above (raw
     * bytes remaining in the frame) includes that padding. UDP already
     * guards against this by trusting its own self-describing udp->len
     * field instead of the raw remaining count (see below); ICMP and TCP
     * have no such field of their own and rely entirely on the IP header's
     * declared total length, so clamp to that here. Without this, e.g. a
     * TCP ACK-only segment (0 real payload bytes) padded up to the 60-byte
     * minimum was being treated as if it carried a handful of trailing
     * zero bytes as real data -- silently corrupting the start of a TCP
     * stream and desyncing sequence-number tracking for the rest of the
     * connection. */
    uint16_t ip_total_len = ntohs(ip->len);
    if (ip_total_len >= ip_header_len && (uint32_t)(ip_total_len - ip_header_len) < ip_payload_len) {
        ip_payload_len = ip_total_len - ip_header_len;
    }

    if (ip->proto == 1) { // ICMP
        net_handle_icmp(eth, ip, data + sizeof(eth_header_t) + ip_header_len, ip_payload_len);
        return;
    }

    if (ip->proto == 6) { // TCP
        net_handle_tcp(ip, data + sizeof(eth_header_t) + ip_header_len, ip_payload_len);
        return;
    }

    if (ip->proto != 17) return; // Only handle UDP beyond this point

    if (len < sizeof(eth_header_t) + ip_header_len + sizeof(udp_header_t)) return;

    udp_header_t *udp = (udp_header_t *)(data + sizeof(eth_header_t) + ip_header_len);

    uint16_t dport    = ntohs(udp->dest_port);
    uint16_t sport    = ntohs(udp->src_port);
    uint16_t udp_total = ntohs(udp->len);
    if (udp_total < sizeof(udp_header_t)) return;

    uint32_t remaining = len - sizeof(eth_header_t) - ip_header_len;
    if (udp_total > remaining) return;

    const uint8_t *udp_payload = (const uint8_t *)udp + sizeof(udp_header_t);
    uint16_t payload_len = udp_total - (uint16_t)sizeof(udp_header_t);

    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (udp_bindings[i].active && udp_bindings[i].port == dport) {
            udp_bindings[i].handler(ip->src, sport, udp_payload, payload_len);
            break;
        }
    }
}

// net_ping: resolve target MAC via ARP, then send ICMP echo request and wait
// for the reply. Returns RTT in milliseconds on success, 0 on timeout, <0 on error.
int net_ping(const char *ip_str) {
    extern uint32_t timer_get_ticks();
    uint8_t target_ip[4];
    uint8_t target_mac[6];
    int found_mac = 0;

    if (!e1000_rings_ready || !net_config.nic_present) return -1;
    if (!ip_str || !parse_ip4(ip_str, target_ip))       return -2;

    // Look up target MAC in the ARP cache
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, target_ip, 4) == 0) {
            memcpy(target_mac, arp_cache[i].mac, 6);
            found_mac = 1;
            break;
        }
    }

    if (!found_mac) {
        net_send_arp_request(target_ip);
        uint32_t deadline = timer_get_ticks() + 100; // 1 s at 100 Hz
        while (timer_get_ticks() < deadline) {
            net_poll();
            for (int i = 0; i < ARP_CACHE_SIZE; i++) {
                if (arp_cache[i].valid && memcmp(arp_cache[i].ip, target_ip, 4) == 0) {
                    memcpy(target_mac, arp_cache[i].mac, 6);
                    found_mac = 1;
                    break;
                }
            }
            if (found_mac) break;
        }
    }

    if (!found_mac) return 0; // ARP timeout — host unreachable at L2

    // Send ICMP echo request
    const uint16_t id  = 0x4D4B; // 'MK'
    const uint16_t seq = 1;
    ping_reply_received = 0;
    ping_pending_id  = id;
    ping_pending_seq = seq;
    if (!net_send_icmp_echo_request(target_mac, target_ip, id, seq)) return -3;

    // Wait up to 2 s for the echo reply
    uint32_t t0       = timer_get_ticks();
    uint32_t deadline = t0 + 200;
    while (timer_get_ticks() < deadline && !ping_reply_received) {
        net_poll();
    }

    if (!ping_reply_received) return 0; // Timeout

    uint32_t elapsed = timer_get_ticks() - t0;
    return (int)(elapsed * 10); // convert 100 Hz ticks → milliseconds
}

static int is_e1000_device(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != E1000_VENDOR_INTEL) {
        return 0;
    }

    switch (device_id) {
        case 0x100E: // 82540EM, QEMU e1000
        case 0x100F:
        case 0x1010:
        case 0x1011:
        case 0x1012:
        case 0x1013:
        case 0x1015:
        case 0x1016:
        case 0x1017:
        case 0x1018:
        case 0x1019:
        case 0x101A:
        case 0x101D:
        case 0x1026:
        case 0x1027:
        case 0x1028:
        case 0x1075:
        case 0x1076:
        case 0x1077:
        case 0x1078:
        case 0x1079:
        case 0x107A:
        case 0x107B:
        case 0x107C:
        case 0x108A:
        case 0x1099:
        case 0x10B5:
            return 1;
        default:
            return 0;
    }
}

static void append_str(char *out, uint32_t *pos, uint32_t max, const char *text) {
    while (text && *text && *pos + 1 < max) {
        out[*pos] = *text;
        (*pos)++;
        text++;
    }
    out[*pos] = '\0';
}

static void append_u8(char *out, uint32_t *pos, uint32_t max, uint8_t value) {
    char tmp[4];
    itoa(value, tmp, 10);
    append_str(out, pos, max, tmp);
}

static void append_ip(char *out, uint32_t *pos, uint32_t max, const uint8_t ip[4]) {
    append_u8(out, pos, max, ip[0]);
    append_str(out, pos, max, ".");
    append_u8(out, pos, max, ip[1]);
    append_str(out, pos, max, ".");
    append_u8(out, pos, max, ip[2]);
    append_str(out, pos, max, ".");
    append_u8(out, pos, max, ip[3]);
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
    append_hex16(out, pos, max, (uint16_t)((value >> 16) & 0xFFFF));
    append_hex16(out, pos, max, (uint16_t)(value & 0xFFFF));
}

static void append_mac(char *out, uint32_t *pos, uint32_t max, const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (i > 0) append_str(out, pos, max, ":");
        append_hex8(out, pos, max, mac[i]);
    }
}

static int parse_octet(const char **cursor, uint8_t *out) {
    uint32_t value = 0;
    int digits = 0;
    const char *p = *cursor;

    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > 255) {
            return 0;
        }
        digits++;
        p++;
    }

    if (digits == 0) {
        return 0;
    }

    *out = (uint8_t)value;
    *cursor = p;
    return 1;
}

static int parse_ipv4(const char *text, uint8_t out[4]) {
    const char *p = text;

    if (!text || !text[0]) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        if (!parse_octet(&p, &out[i])) {
            return 0;
        }
        if (i < 3) {
            if (*p != '.') {
                return 0;
            }
            p++;
        }
    }

    return *p == '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(char *text) {
    char *start = text;
    char *end;

    while (is_space(*start)) {
        start++;
    }
    if (start != text) {
        char *dst = text;
        while (*start) {
            *dst++ = *start++;
        }
        *dst = '\0';
    }

    end = text + strlen(text);
    while (end > text && is_space(end[-1])) {
        end--;
    }
    *end = '\0';
}

static int copy_token(char *out, uint32_t out_size, const char *start, const char *end) {
    uint32_t pos = 0;

    if (!out || out_size == 0 || !start || !end || end < start) {
        return 0;
    }

    while (start < end && pos + 1 < out_size) {
        out[pos++] = *start++;
    }
    out[pos] = '\0';
    trim(out);
    return pos > 0;
}

static int key_equals(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

void net_init(void) {
    memset(&net_config, 0, sizeof(net_config));
    net_config.mode = NET_MODE_DHCP;
    net_config.netmask[0] = 255;
    net_config.netmask[1] = 255;
    net_config.netmask[2] = 255;
    net_config.hostname[0] = 'm';
    net_config.hostname[1] = 'i';
    net_config.hostname[2] = 'c';
    net_config.hostname[3] = 'r';
    net_config.hostname[4] = 'o';
    net_config.hostname[5] = 'k';
    net_config.hostname[6] = '\0';
}

static uint32_t e1000_eeprom_read(uint32_t mmio, uint32_t offset) {
    volatile uint32_t *eerd = (volatile uint32_t *)(uintptr_t)(mmio + 0x0014);
    *eerd = (offset << 8) | 1;
    
    int timeout = 100000;
    while (!(*eerd & 0x10) && --timeout > 0) {
        // Spin
    }
    return *eerd >> 16;
}

static int e1000_init_rings(void) {
    uint32_t rx_desc_bytes = E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t);
    uint32_t tx_desc_bytes = E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t);
    uint32_t rx_buf_bytes = E1000_RX_DESC_COUNT * E1000_RX_BUF_SIZE;
    uint32_t tx_buf_bytes = E1000_TX_DESC_COUNT * E1000_TX_BUF_SIZE;
    uint32_t total_bytes = rx_desc_bytes + tx_desc_bytes + rx_buf_bytes + tx_buf_bytes;
    uint8_t *region = (uint8_t *)pmm_alloc_region(total_bytes);

    if (!region) {
        klog("Network: e1000 ring allocation failed.");
        return 0;
    }

    memset(region, 0, total_bytes);
    e1000_rx_descs = (e1000_rx_desc_t *)region;
    region += rx_desc_bytes;
    e1000_tx_descs = (e1000_tx_desc_t *)region;
    region += tx_desc_bytes;
    e1000_rx_buffers = region;
    region += rx_buf_bytes;
    e1000_tx_buffers = region;

    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; i++) {
        e1000_rx_descs[i].addr = (uint32_t)(e1000_rx_buffers + (i * E1000_RX_BUF_SIZE));
        e1000_rx_descs[i].status = 0;
    }
    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; i++) {
        e1000_tx_descs[i].addr = (uint32_t)(e1000_tx_buffers + (i * E1000_TX_BUF_SIZE));
        e1000_tx_descs[i].status = E1000_TX_STA_DD;
    }

    e1000_rx_cur = 0;
    e1000_tx_tail = 0;

    e1000_write(E1000_REG_RCTL, 0);
    e1000_write(E1000_REG_TCTL, 0);
    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_SLU);
    e1000_write(E1000_REG_RAL0,
                ((uint32_t)net_config.mac[0]) |
                ((uint32_t)net_config.mac[1] << 8) |
                ((uint32_t)net_config.mac[2] << 16) |
                ((uint32_t)net_config.mac[3] << 24));
    e1000_write(E1000_REG_RAH0,
                ((uint32_t)net_config.mac[4]) |
                ((uint32_t)net_config.mac[5] << 8) |
                (1u << 31));

    e1000_write(E1000_REG_RDBAL, (uint32_t)e1000_rx_descs);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN, rx_desc_bytes);
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1);

    e1000_write(E1000_REG_TDBAL, (uint32_t)e1000_tx_descs);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN, tx_desc_bytes);
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);

    e1000_write(E1000_REG_TIPG, 10 | (8 << 10) | (6 << 20));
    e1000_write(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_SECRC);

    e1000_rings_ready = 1;
    klog("Network: e1000 RX/TX rings initialized.");
    return 1;
}

/* net_poll() mutates shared ring state (e1000_rx_cur, the descriptor ring,
 * RDT) and is called both from the dedicated net_poll_task and, re-entrantly,
 * from synchronous helpers running on task 0 (net_send_udp's ARP wait,
 * net_ping, net_config_dhcp, net_dns_resolve). Since the timer IRQ can call
 * task_switch() unconditionally on every 10th tick, one of those synchronous
 * callers can be preempted mid-poll and net_poll_task resumed, which would
 * start an independent, interleaved pass over the same ring state and
 * desynchronize e1000_rx_cur from the hardware head. This flag makes a
 * reentrant call a no-op instead of corrupting the ring. */
static volatile int net_poll_active = 0;

void net_poll(void) {
    if (!e1000_rings_ready) {
        return;
    }
    if (net_poll_active) {
        return;
    }
    net_poll_active = 1;

    for (uint32_t budget = 0; budget < E1000_RX_DESC_COUNT; budget++) {
        volatile e1000_rx_desc_t *desc = &e1000_rx_descs[e1000_rx_cur];

        if (!(desc->status & E1000_RX_STA_DD)) {
            break;
        }

        if (desc->length > 0 && desc->length <= E1000_RX_BUF_SIZE) {
            net_handle_packet(e1000_rx_buffers + (e1000_rx_cur * E1000_RX_BUF_SIZE), desc->length);
        }

        desc->status = 0;
        desc->errors = 0;
        desc->length = 0;
        // RDT must be the index of the descriptor we just freed (the
        // pre-increment cursor), not the post-increment one. QEMU's e1000
        // model (hw/net/e1000.c, e1000_has_rxbufs) treats the ring as
        // non-empty exactly while RDH != RDT for a short packet -- and
        // hardware's own RDH advances to match wherever it just finished
        // writing, i.e. to the *post-increment* index, the instant it
        // delivers a packet. Writing that same post-increment value here
        // makes RDH == RDT the moment hardware catches up, which QEMU reads
        // as "zero descriptors available" and silently drops every
        // following packet (confirmed via `-trace e1000_receiver_overrun`:
        // "RDH=1, RDT=1" right after the first packet was drained) -- a
        // permanent stall, not just a capped ring, since nothing ever moves
        // RDT again once stuck. Writing the freed (pre-increment) index
        // instead keeps RDT one step behind RDH, which is exactly the
        // invariant the initial RDT = E1000_RX_DESC_COUNT - 1 (with
        // RDH = 0) already established at ring init -- this preserves it
        // on every subsequent packet instead of collapsing it after the
        // first.
        e1000_write(E1000_REG_RDT, e1000_rx_cur);
        e1000_rx_cur = (e1000_rx_cur + 1) % E1000_RX_DESC_COUNT;
    }

    net_poll_active = 0;
}

void net_detect_pci(void) {
    int count = pci_device_count();

    for (int i = 0; i < count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) {
            continue;
        }

        if ((dev->class_code == PCI_CLASS_NETWORK && dev->subclass == PCI_SUBCLASS_ETHERNET) ||
            is_e1000_device(dev->vendor_id, dev->device_id)) {
            net_config.nic_present = 1;
            net_config.nic_vendor_id = dev->vendor_id;
            net_config.nic_device_id = dev->device_id;
            net_config.nic_mmio_base = dev->bar[0] & ~0x0F;
            net_config.nic_io_base = dev->bar[1] & ~0x03;
            net_config.nic_name[0] = 'e';
            net_config.nic_name[1] = '1';
            net_config.nic_name[2] = '0';
            net_config.nic_name[3] = '0';
            net_config.nic_name[4] = '0';
            net_config.nic_name[5] = '\0';
            
            // Map 128 KB of e1000 MMIO space (32 pages)
            uint32_t phys = net_config.nic_mmio_base;
            if (phys != 0 && phys != 0xFFFFFFFF) {
                uint32_t command = pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
                command |= 0x00000006; // memory space + bus mastering
                pci_config_write32(dev->bus, dev->slot, dev->function, 0x04, command);

                for (uint32_t offset = 0; offset < 128 * 1024; offset += 4096) {
                    vmm_map_page_ext(E1000_MMIO_VMEM + offset, phys + offset, 0x1B);
                }
                
                // Read the pre-configured MAC address from RAL[0]/RAH[0] registers
                uint32_t ral = *(volatile uint32_t *)(uintptr_t)(E1000_MMIO_VMEM + 0x5400);
                uint32_t rah = *(volatile uint32_t *)(uintptr_t)(E1000_MMIO_VMEM + 0x5404);
                
                // Validate if RAL/RAH are set (not all 0s or all 1s)
                if (ral != 0 && ral != 0xFFFFFFFF) {
                    net_config.mac[0] = ral & 0xFF;
                    net_config.mac[1] = (ral >> 8) & 0xFF;
                    net_config.mac[2] = (ral >> 16) & 0xFF;
                    net_config.mac[3] = (ral >> 24) & 0xFF;
                    net_config.mac[4] = rah & 0xFF;
                    net_config.mac[5] = (rah >> 8) & 0xFF;
                } else {
                    // Fallback to EEPROM reads
                    uint16_t w0 = e1000_eeprom_read(E1000_MMIO_VMEM, 0);
                    uint16_t w1 = e1000_eeprom_read(E1000_MMIO_VMEM, 1);
                    uint16_t w2 = e1000_eeprom_read(E1000_MMIO_VMEM, 2);
                    net_config.mac[0] = w0 & 0xFF;
                    net_config.mac[1] = w0 >> 8;
                    net_config.mac[2] = w1 & 0xFF;
                    net_config.mac[3] = w1 >> 8;
                    net_config.mac[4] = w2 & 0xFF;
                    net_config.mac[5] = w2 >> 8;
                }
                
                net_config.link_up = 1; // Mark link up
                klog("Network: Intel e1000 NIC initialized. MAC Address retrieved.");
                e1000_init_rings();
            }
            return;
        }
    }
}

const net_config_t *net_get_config(void) {
    return &net_config;
}

const char *net_status_string(void) {
    uint32_t pos = 0;

    memset(net_status_buf, 0, sizeof(net_status_buf));
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "Network:\n");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "  nic:      ");
    if (net_config.nic_present) {
        append_str(net_status_buf, &pos, sizeof(net_status_buf), net_config.nic_name);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), " vendor=0x");
        append_hex16(net_status_buf, &pos, sizeof(net_status_buf), net_config.nic_vendor_id);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), " device=0x");
        append_hex16(net_status_buf, &pos, sizeof(net_status_buf), net_config.nic_device_id);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  mmio:     phys=0x");
        append_hex32(net_status_buf, &pos, sizeof(net_status_buf), net_config.nic_mmio_base);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), ", virt=0x");
        append_hex32(net_status_buf, &pos, sizeof(net_status_buf), E1000_MMIO_VMEM);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  io:       0x");
        append_hex32(net_status_buf, &pos, sizeof(net_status_buf), net_config.nic_io_base);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  mac:      ");
        append_mac(net_status_buf, &pos, sizeof(net_status_buf), net_config.mac);
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n");
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "  e1000 io: ");
        append_str(net_status_buf, &pos, sizeof(net_status_buf), e1000_rings_ready ? "rx/tx rings ready\n" : "rings not initialized\n");
    } else {
        append_str(net_status_buf, &pos, sizeof(net_status_buf), "none\n");
    }
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "  link:     ");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), net_config.link_up ? "up\n" : "down\n");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "  mode:     ");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), net_config.mode == NET_MODE_DHCP ? "dhcp\n" : "static\n");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "  ip:       ");
    append_ip(net_status_buf, &pos, sizeof(net_status_buf), net_config.ip);
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  netmask:  ");
    append_ip(net_status_buf, &pos, sizeof(net_status_buf), net_config.netmask);
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  gateway:  ");
    append_ip(net_status_buf, &pos, sizeof(net_status_buf), net_config.gateway);
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  dns:      ");
    append_ip(net_status_buf, &pos, sizeof(net_status_buf), net_config.dns);
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n  hostname: ");
    append_str(net_status_buf, &pos, sizeof(net_status_buf), net_config.hostname);
    append_str(net_status_buf, &pos, sizeof(net_status_buf), "\n");

    return net_status_buf;
}

/* ---- DHCP client implementation ---- */

static int dhcp_get_option(const uint8_t *opts, uint16_t opts_len,
                            uint8_t code, uint8_t *out, uint8_t out_max) {
    uint16_t i = 0;
    while (i < opts_len) {
        uint8_t opt = opts[i++];
        if (opt == 255) break;
        if (opt == 0) continue; // pad
        if (i >= opts_len) break;
        uint8_t len = opts[i++];
        if (i + (uint16_t)len > opts_len) break; /* truncated option, malformed packet */
        if (opt == code && out && out_max > 0) {
            uint8_t copy = len < out_max ? len : out_max;
            memcpy(out, opts + i, copy);
            i += len;
            return copy;
        }
        i += len;
    }
    return 0;
}

static void dhcp_send_discover(void) {
    static const uint8_t bcast4[4] = {255, 255, 255, 255};
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    dhcp_packet_t *pkt = (dhcp_packet_t *)buf;

    pkt->op    = 1;
    pkt->htype = 1;
    pkt->hlen  = 6;
    pkt->xid   = dhcp_xid;           /* raw 4-byte blob, server mirrors it back */
    pkt->flags = htons(0x8000);       /* request broadcast reply */
    memcpy(pkt->chaddr, net_config.mac, 6);
    pkt->magic = htonl(DHCP_MAGIC_BE);

    uint8_t *opt = buf + sizeof(dhcp_packet_t);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_MSG_DISCOVER;
    *opt++ = 55; *opt++ = 4; *opt++ = 1; *opt++ = 3; *opt++ = 6; *opt++ = 51;
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(sizeof(dhcp_packet_t) + 10);
    net_send_udp(bcast4, DHCP_PORT_SERVER, DHCP_PORT_CLIENT, buf, pkt_len);
}

static void dhcp_send_request(void) {
    static const uint8_t bcast4[4] = {255, 255, 255, 255};
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    dhcp_packet_t *pkt = (dhcp_packet_t *)buf;

    pkt->op    = 1;
    pkt->htype = 1;
    pkt->hlen  = 6;
    pkt->xid   = dhcp_xid;
    pkt->flags = htons(0x8000);
    memcpy(pkt->chaddr, net_config.mac, 6);
    pkt->magic = htonl(DHCP_MAGIC_BE);

    uint8_t *opt = buf + sizeof(dhcp_packet_t);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_MSG_REQUEST;
    *opt++ = 50; *opt++ = 4;
    *opt++ = dhcp_offered_ip[0]; *opt++ = dhcp_offered_ip[1];
    *opt++ = dhcp_offered_ip[2]; *opt++ = dhcp_offered_ip[3];
    *opt++ = 54; *opt++ = 4;
    *opt++ = dhcp_server_id[0]; *opt++ = dhcp_server_id[1];
    *opt++ = dhcp_server_id[2]; *opt++ = dhcp_server_id[3];
    *opt++ = 55; *opt++ = 4; *opt++ = 1; *opt++ = 3; *opt++ = 6; *opt++ = 51;
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(sizeof(dhcp_packet_t) + 22);
    net_send_udp(bcast4, DHCP_PORT_SERVER, DHCP_PORT_CLIENT, buf, pkt_len);
}

static void dhcp_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                              const uint8_t *payload, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < (uint16_t)sizeof(dhcp_packet_t)) return;
    const dhcp_packet_t *pkt = (const dhcp_packet_t *)payload;

    if (pkt->op != 2) return;                       /* must be a BOOTP reply */
    if (pkt->xid != dhcp_xid) return;               /* must match our transaction */
    if (pkt->magic != htonl(DHCP_MAGIC_BE)) return; /* bad magic cookie */

    const uint8_t *opts     = payload + sizeof(dhcp_packet_t);
    uint16_t       opts_len = len - (uint16_t)sizeof(dhcp_packet_t);

    uint8_t msg_type = 0;
    dhcp_get_option(opts, opts_len, 53, &msg_type, 1);

    if (msg_type == DHCP_MSG_OFFER && dhcp_client_state == DHCP_DISCOVERING) {
        memcpy(dhcp_offered_ip, pkt->yiaddr, 4);
        dhcp_get_option(opts, opts_len, 1,  dhcp_offered_mask, 4); /* subnet mask */
        dhcp_get_option(opts, opts_len, 3,  dhcp_offered_gw,   4); /* router */
        dhcp_get_option(opts, opts_len, 6,  dhcp_offered_dns,  4); /* DNS */
        dhcp_get_option(opts, opts_len, 54, dhcp_server_id,    4); /* server id */
        if (dhcp_server_id[0] == 0) memcpy(dhcp_server_id, pkt->siaddr, 4);

        dhcp_client_state = DHCP_REQUESTING;
        dhcp_send_request();

    } else if (msg_type == DHCP_MSG_ACK && dhcp_client_state == DHCP_REQUESTING) {
        memcpy(dhcp_offered_ip, pkt->yiaddr, 4);
        dhcp_get_option(opts, opts_len, 1, dhcp_offered_mask, 4);
        dhcp_get_option(opts, opts_len, 3, dhcp_offered_gw,   4);
        dhcp_get_option(opts, opts_len, 6, dhcp_offered_dns,  4);
        dhcp_client_state = DHCP_BOUND;
    }
}

int net_config_dhcp(void) {
    extern uint32_t timer_get_ticks(void);

    if (!e1000_rings_ready || !net_config.nic_present) {
        klog("DHCP: no NIC available.");
        return 0;
    }

    /* Build XID from MAC to make it pseudo-unique across reboots */
    dhcp_xid = ((uint32_t)net_config.mac[2] << 24) |
               ((uint32_t)net_config.mac[3] << 16) |
               ((uint32_t)net_config.mac[4] << 8)  |
               (uint32_t)net_config.mac[5];

    memset(dhcp_offered_ip,   0, 4);
    memset(dhcp_server_id,    0, 4);
    memset(dhcp_offered_mask, 0, 4);
    memset(dhcp_offered_gw,   0, 4);
    memset(dhcp_offered_dns,  0, 4);

    net_udp_bind(DHCP_PORT_CLIENT, dhcp_udp_handler);

    int got_lease = 0;
    for (int attempt = 0; attempt < 3 && !got_lease; attempt++) {
        dhcp_client_state = DHCP_DISCOVERING;
        dhcp_send_discover();
        klog("DHCP: Discover sent.");

        uint32_t deadline = timer_get_ticks() + 200; /* 2 s at 100 Hz */
        while (timer_get_ticks() < deadline && dhcp_client_state != DHCP_BOUND) {
            net_poll();
        }
        if (dhcp_client_state == DHCP_BOUND) got_lease = 1;
    }

    net_udp_unbind(DHCP_PORT_CLIENT);

    if (got_lease) {
        memcpy(net_config.ip,      dhcp_offered_ip,   4);
        memcpy(net_config.netmask, dhcp_offered_mask, 4);
        memcpy(net_config.gateway, dhcp_offered_gw,   4);
        memcpy(net_config.dns,     dhcp_offered_dns,  4);
        net_config.mode       = NET_MODE_DHCP;
        net_config.has_config = 1;
        klog("DHCP: lease acquired.");
        return 1;
    }

    klog("DHCP: timed out, no offer received.");
    return 0;
}

/* ---- DNS client implementation ---- */

static uint16_t dns_encode_name(uint8_t *out, const char *name) {
    uint16_t pos = 0;
    while (*name) {
        const char *seg = name;
        while (*seg && *seg != '.') seg++;
        uint8_t seglen = (uint8_t)(seg - name);
        if (seglen == 0 || seglen > 63) break;
        out[pos++] = seglen;
        memcpy(out + pos, name, seglen);
        pos += seglen;
        name = *seg == '.' ? seg + 1 : seg;
    }
    out[pos++] = 0; /* root label */
    return pos;
}

static uint16_t dns_skip_name(const uint8_t *pkt, uint16_t pos, uint16_t len) {
    while (pos < len) {
        uint8_t b = pkt[pos];
        if (b == 0)              { pos++; break; }
        if ((b & 0xC0) == 0xC0) { pos += 2; break; } /* compressed pointer */
        pos += (uint16_t)(b + 1);
    }
    return pos;
}

static void dns_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                             const uint8_t *payload, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < 12) return;
    uint16_t id = ((uint16_t)payload[0] << 8) | payload[1];
    if (id != dns_txid) return;
    uint16_t flags = ((uint16_t)payload[2] << 8) | payload[3];
    if (!(flags & 0x8000)) return;      /* not a response */
    if ((flags & 0x000F) != 0) return;  /* RCODE != NOERROR */
    uint16_t qdcount = ((uint16_t)payload[4] << 8) | payload[5];
    uint16_t ancount = ((uint16_t)payload[6] << 8) | payload[7];
    if (ancount == 0) return;

    uint16_t pos = 12;
    for (uint16_t q = 0; q < qdcount && pos < len; q++) {
        pos = dns_skip_name(payload, pos, len);
        pos += 4; /* QTYPE + QCLASS */
    }
    for (uint16_t i = 0; i < ancount && pos < len; i++) {
        pos = dns_skip_name(payload, pos, len);
        if (pos + 10 > len) return;
        uint16_t rtype = ((uint16_t)payload[pos] << 8) | payload[pos + 1]; pos += 2;
        pos += 2; /* class */
        pos += 4; /* TTL */
        uint16_t rdlen = ((uint16_t)payload[pos] << 8) | payload[pos + 1]; pos += 2;
        if (rtype == 1 && rdlen == 4 && pos + 4 <= len) {
            memcpy(dns_resolved_ip, payload + pos, 4);
            dns_reply_received = 1;
            return;
        }
        pos += rdlen;
    }
}

int net_dns_resolve(const char *hostname, uint8_t out_ip[4]) {
    extern uint32_t timer_get_ticks(void);
    if (!hostname || !out_ip) return 0;

    /* Fast path: hostname is already a dotted-quad IP */
    if (parse_ip4(hostname, out_ip)) return 1;

    if (!net_config.has_config || !e1000_rings_ready) return 0;
    if (net_config.dns[0] == 0 && net_config.dns[1] == 0 &&
        net_config.dns[2] == 0 && net_config.dns[3] == 0) return 0;

    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    dns_txid = (uint16_t)(0xD000u |
               ((uint16_t)net_config.mac[4] << 4) |
               (uint16_t)net_config.mac[5]);

    buf[0] = (uint8_t)(dns_txid >> 8);
    buf[1] = (uint8_t)(dns_txid & 0xFF);
    buf[2] = 0x01; /* flags: RD=1 */
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; /* QDCOUNT=1 */

    uint16_t pos = 12;
    pos += dns_encode_name(buf + pos, hostname);
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* QTYPE  = A */
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* QCLASS = IN */

    dns_reply_received = 0;
    net_udp_bind(DNS_CLIENT_PORT, dns_udp_handler);
    net_send_udp(net_config.dns, 53, DNS_CLIENT_PORT, buf, pos);

    uint32_t deadline = timer_get_ticks() + 300; /* 3 s at 100 Hz */
    while (timer_get_ticks() < deadline && !dns_reply_received) {
        net_poll();
    }
    net_udp_unbind(DNS_CLIENT_PORT);

    if (dns_reply_received) {
        memcpy(out_ip, dns_resolved_ip, 4);
        return 1;
    }
    return 0;
}

int net_config_dns(const char *dns) {
    uint8_t parsed_dns[4];

    if (!parse_ipv4(dns, parsed_dns)) {
        return 0;
    }

    memcpy(net_config.dns, parsed_dns, sizeof(net_config.dns));
    net_config.has_config = 1;
    return 1;
}

/* ---- TCP connect/close + minimal HTTP client ---- */

/* Opens the single TCP connection: sends SYN, waits ~1s for SYN-ACK,
 * retrying the SYN (same ISN) up to 2 more times on timeout -- connection
 * setup is the one step worth retrying here, since net_http_get's request
 * and close aren't retried at all (see the "TCP client" section above for
 * why). Returns 0 on success, -1 if the NIC/link isn't up, -2 on handshake
 * timeout or an RST from the remote. */
static int net_tcp_connect(const uint8_t dest_ip[4], uint16_t dest_port) {
    extern uint32_t timer_get_ticks(void);
    if (!e1000_rings_ready || !net_config.nic_present) return -1;

    memset(&tcp_conn, 0, sizeof(tcp_conn));
    memcpy(tcp_conn.remote_ip, dest_ip, 4);
    tcp_conn.remote_port = dest_port;
    tcp_conn.local_port  = tcp_next_local_port++;
    if (tcp_next_local_port == 0) {
        tcp_next_local_port = 49152; /* wrapped past 65535 */
    }

    /* ISN doesn't need to be cryptographically random for a lab kernel --
     * just distinct enough per connection that a stray segment from an
     * earlier connection on the same local port doesn't look current. */
    tcp_conn.local_seq = timer_get_ticks() ^ ((uint32_t)tcp_conn.local_port << 16);
    tcp_conn.state = TCP_SYN_SENT;

    for (int attempt = 0; attempt < 3; attempt++) {
        net_send_tcp_segment(TCP_FLAG_SYN, 0, 0);

        uint32_t deadline = timer_get_ticks() + 100; /* 1s per attempt */
        while (timer_get_ticks() < deadline && tcp_conn.state == TCP_SYN_SENT) {
            net_poll();
        }
        if (tcp_conn.state == TCP_ESTABLISHED) {
            return 0;
        }
        if (tcp_conn.state == TCP_DONE) {
            return -2; /* RST */
        }
    }
    tcp_conn.state = TCP_CLOSED;
    return -2;
}

/* Best-effort abandon for a connection net_http_get is giving up on before
 * it reached TCP_DONE via a normal FIN exchange (timeout, full receive
 * buffer). Sends one RST and stops -- not a graceful close, but this
 * connection's ephemeral port won't be reused for a long time regardless. */
static void net_tcp_abort(void) {
    if (tcp_conn.state == TCP_ESTABLISHED || tcp_conn.state == TCP_SYN_SENT) {
        net_send_tcp_segment((uint8_t)(TCP_FLAG_RST | TCP_FLAG_ACK), 0, 0);
    }
    tcp_conn.state = TCP_CLOSED;
}

/* Blocking HTTP/1.0 GET: resolves host (net_dns_resolve, so dotted-quad IPs
 * work directly too, no DNS needed), opens a TCP connection, sends the
 * request, and copies whatever response bytes arrive (headers + body,
 * verbatim -- no parsing) into out[] until the connection closes or out
 * fills up. This is deliberately just "enough TCP to fetch something",
 * matching the roadmap's "preparar base para HTTP simple" -- no chunked
 * transfer-encoding, no redirects, no keep-alive.
 * Returns the number of bytes written to out on success (>=0, 0 is a valid
 * empty response), or a negative error: -1 bad arguments or DNS failure,
 * -2 TCP connect failed, -3 could not send the request, -4 no response
 * bytes ever arrived before giving up. */
int net_http_get(const char *host, uint16_t port, const char *path, char *out, uint32_t out_size) {
    extern uint32_t timer_get_ticks(void);
    if (!host || !path || !out || out_size == 0) {
        return -1;
    }

    uint8_t dest_ip[4];
    if (!net_dns_resolve(host, dest_ip)) {
        return -1;
    }

    if (net_tcp_connect(dest_ip, port) != 0) {
        return -2;
    }

    char request[256];
    uint32_t pos = 0;
    append_str(request, &pos, sizeof(request), "GET ");
    append_str(request, &pos, sizeof(request), path);
    append_str(request, &pos, sizeof(request), " HTTP/1.0\r\nHost: ");
    append_str(request, &pos, sizeof(request), host);
    append_str(request, &pos, sizeof(request), "\r\nConnection: close\r\n\r\n");

    uint16_t req_len = (uint16_t)pos;
    if (!net_send_tcp_segment((uint8_t)(TCP_FLAG_PSH | TCP_FLAG_ACK), (const uint8_t *)request, req_len)) {
        net_tcp_abort();
        return -3;
    }
    tcp_conn.local_seq += req_len;

    tcp_recv_buf      = (uint8_t *)out;
    tcp_recv_buf_size = out_size;
    tcp_recv_len      = 0;

    uint32_t deadline = timer_get_ticks() + 500; /* 5s to receive the whole response */
    while (timer_get_ticks() < deadline && tcp_conn.state == TCP_ESTABLISHED) {
        net_poll();
        if (tcp_recv_len >= tcp_recv_buf_size) {
            break;
        }
    }

    int had_data       = (tcp_recv_len > 0);
    int closed_cleanly = (tcp_conn.state == TCP_DONE && tcp_conn.fin_received);

    tcp_recv_buf      = 0;
    tcp_recv_buf_size = 0;

    if (tcp_conn.state != TCP_DONE) {
        net_tcp_abort(); /* timed out or buffer filled mid-stream */
    }

    if (!had_data && !closed_cleanly) {
        return -4;
    }
    return (int)tcp_recv_len;
}

/* ---- Remote shell (RSH) UDP service ---- */

static void net_rsh_execute(const char *cmd, char *out, uint32_t out_size) {
    uint32_t pos = 0;
    uint32_t clen = strlen(cmd);

    if (text_equals(cmd, clen, "PING") || text_equals(cmd, clen, "ping")) {
        append_str(out, &pos, out_size, "PONG\n");

    } else if (text_equals(cmd, clen, "net status")) {
        copy_limited_text(out, out_size, net_status_string());

    } else if (text_equals(cmd, clen, "llm status")) {
        copy_limited_text(out, out_size, llm_status_string());

    } else if (text_equals(cmd, clen, "llm info")) {
        copy_limited_text(out, out_size, llm_info_string());

    } else if (text_equals(cmd, clen, "arp")) {
        copy_limited_text(out, out_size, arp_status_string());

    } else if (text_starts_with(cmd, clen, "ping ")) {
        int rtt = net_ping(cmd + 5);
        if (rtt > 0) {
            char tmp[12];
            append_str(out, &pos, out_size, "PONG ");
            itoa(rtt, tmp, 10);
            append_str(out, &pos, out_size, tmp);
            append_str(out, &pos, out_size, "ms\n");
        } else if (rtt == 0) {
            append_str(out, &pos, out_size, "timeout\n");
        } else {
            append_str(out, &pos, out_size, "error (no route or ARP failed)\n");
        }

    } else if (text_starts_with(cmd, clen, "nslookup ")) {
        uint8_t ip[4] = {0, 0, 0, 0};
        const char *host = cmd + 9;
        if (net_dns_resolve(host, ip)) {
            append_str(out, &pos, out_size, host);
            append_str(out, &pos, out_size, " -> ");
            append_ip(out, &pos, out_size, ip);
            append_str(out, &pos, out_size, "\n");
        } else {
            append_str(out, &pos, out_size, "NXDOMAIN\n");
        }

    } else if (text_starts_with(cmd, clen, "ask ")) {
        char prompt[128];
        uint32_t plen = clen - 4;
        if (plen >= sizeof(prompt)) plen = sizeof(prompt) - 1;
        memcpy(prompt, cmd + 4, plen);
        prompt[plen] = '\0';
        llm_inference(prompt, out);

    } else if (text_starts_with(cmd, clen, "llm ask ")) {
        char prompt[128];
        uint32_t plen = clen - 8;
        if (plen >= sizeof(prompt)) plen = sizeof(prompt) - 1;
        memcpy(prompt, cmd + 8, plen);
        prompt[plen] = '\0';
        llm_inference(prompt, out);

    } else if (text_starts_with(cmd, clen, "cat ")) {
        extern int fat32_load_file(const char *, uint8_t *, uint32_t, uint32_t *);
        const char *fname = cmd + 4;
        uint32_t bytes_read = 0;
        if (fat32_load_file(fname, (uint8_t *)out, out_size - 1, &bytes_read)) {
            out[bytes_read] = '\0';
        } else {
            copy_limited_text(out, out_size, "ERR file not found\n");
        }

    } else if (text_equals(cmd, clen, "help")) {
        copy_limited_text(out, out_size,
            "Commands: PING, net status, llm status, llm info, arp, "
            "ping <ip>, nslookup <host>, ask <prompt>, llm ask <prompt>, "
            "cat <file>, help\n");

    } else {
        append_str(out, &pos, out_size, "ERR unknown: ");
        append_str(out, &pos, out_size, cmd);
        append_str(out, &pos, out_size, "\n");
    }
}

static void net_rsh_udp_handler(const uint8_t src_ip[4], uint16_t src_port,
                                 const uint8_t *payload, uint16_t len) {
    char cmd[128];
    static char response[960];

    uint32_t to_copy = len > 127 ? 127 : len;
    memcpy(cmd, payload, to_copy);
    cmd[to_copy] = '\0';
    to_copy = trim_packet_text(cmd, to_copy);

    const char *actual_cmd = cmd;
    if (net_rsh_token[0]) {
        uint32_t tlen = strlen(net_rsh_token);
        if (to_copy <= tlen + 1 ||
            memcmp(cmd, net_rsh_token, tlen) != 0 ||
            cmd[tlen] != ' ') {
            net_send_udp(src_ip, src_port, net_rsh_port,
                         (const uint8_t *)"ERR unauthorized\n", 17);
            return;
        }
        actual_cmd = cmd + tlen + 1;
    }

    response[0] = '\0';
    net_rsh_execute(actual_cmd, response, sizeof(response));

    uint16_t resp_len = (uint16_t)strlen(response);
    if (resp_len == 0) { response[0] = '\n'; resp_len = 1; }
    net_send_udp(src_ip, src_port, net_rsh_port, (const uint8_t *)response, resp_len);
}

int net_rsh_set_enabled(int enabled) {
    if (enabled) {
        net_rsh_enabled = 1;
        net_udp_bind(net_rsh_port, net_rsh_udp_handler);
        klog("RSH: remote shell enabled.");
    } else {
        net_rsh_enabled = 0;
        net_udp_unbind(net_rsh_port);
        klog("RSH: remote shell disabled.");
    }
    return 1;
}

int net_rsh_set_port(uint16_t port) {
    if (port == 0) return 0;
    if (net_rsh_enabled) net_udp_unbind(net_rsh_port);
    net_rsh_port = port;
    if (net_rsh_enabled) net_udp_bind(net_rsh_port, net_rsh_udp_handler);
    return 1;
}

int net_rsh_set_token(const char *token) {
    if (!token || !token[0] || strcmp(token, "off") == 0) {
        net_rsh_token[0] = '\0';
    } else {
        uint32_t len = strlen(token);
        if (len > 32) len = 32;
        memcpy(net_rsh_token, token, len);
        net_rsh_token[len] = '\0';
    }
    return 1;
}

const char *net_rsh_status_string(void) {
    uint32_t pos = 0;
    char port_buf[8];
    memset(net_rsh_status_buf, 0, sizeof(net_rsh_status_buf));
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), "RSH: ");
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), net_rsh_enabled ? "on" : "off");
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), " port=");
    itoa(net_rsh_port, port_buf, 10);
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), port_buf);
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), net_rsh_token[0] ? " auth=on" : " auth=off");
    append_str(net_rsh_status_buf, &pos, sizeof(net_rsh_status_buf), "\n");
    return net_rsh_status_buf;
}

int net_config_hostname(const char *hostname) {
    uint32_t len;

    if (!hostname || !hostname[0]) {
        return 0;
    }

    len = strlen(hostname);
    if (len >= sizeof(net_config.hostname)) {
        len = sizeof(net_config.hostname) - 1;
    }

    memcpy(net_config.hostname, hostname, len);
    net_config.hostname[len] = '\0';
    net_config.has_config = 1;
    return 1;
}

int net_load_config_text(const char *text, uint32_t size) {
    uint32_t pos = 0;
    int applied = 0;
    int static_seen = 0;
    char ip[24];
    char netmask[24];
    char gateway[24];
    char dns[24];

    memset(ip, 0, sizeof(ip));
    memset(netmask, 0, sizeof(netmask));
    memset(gateway, 0, sizeof(gateway));
    memset(dns, 0, sizeof(dns));

    if (!text || size == 0) {
        return 0;
    }

    while (pos < size && text[pos]) {
        const char *line_start = text + pos;
        const char *line_end;
        const char *equals;
        char key[16];
        char value[40];

        while (pos < size && text[pos] && text[pos] != '\n') {
            pos++;
        }
        line_end = text + pos;
        if (pos < size && text[pos] == '\n') {
            pos++;
        }

        while (line_start < line_end && is_space(*line_start)) {
            line_start++;
        }
        if (line_start >= line_end || *line_start == '#') {
            continue;
        }

        equals = line_start;
        while (equals < line_end && *equals != '=') {
            equals++;
        }
        if (equals >= line_end) {
            continue;
        }

        if (!copy_token(key, sizeof(key), line_start, equals) ||
            !copy_token(value, sizeof(value), equals + 1, line_end)) {
            continue;
        }

        if (key_equals(key, "mode")) {
            if (strcmp(value, "dhcp") == 0) {
                applied |= net_config_dhcp();
            } else if (strcmp(value, "static") == 0) {
                static_seen = 1;
            }
        } else if (key_equals(key, "ip")) {
            strncpy(ip, value, sizeof(ip) - 1);
        } else if (key_equals(key, "netmask")) {
            strncpy(netmask, value, sizeof(netmask) - 1);
        } else if (key_equals(key, "gateway")) {
            strncpy(gateway, value, sizeof(gateway) - 1);
        } else if (key_equals(key, "dns")) {
            strncpy(dns, value, sizeof(dns) - 1);
        } else if (key_equals(key, "hostname")) {
            applied |= net_config_hostname(value);
        } else if (key_equals(key, "llm_net")) {
            if (strcmp(value, "on") == 0 || strcmp(value, "enabled") == 0 || strcmp(value, "1") == 0) {
                applied |= net_llm_service_set_enabled(1);
            } else if (strcmp(value, "off") == 0 || strcmp(value, "disabled") == 0 || strcmp(value, "0") == 0) {
                applied |= net_llm_service_set_enabled(0);
            }
        } else if (key_equals(key, "llm_port")) {
            uint16_t port;
            if (parse_u16(value, &port)) {
                applied |= net_llm_service_set_port(port);
            }
        }
    }

    if (static_seen || ip[0] || netmask[0] || gateway[0]) {
        if (!ip[0] || !netmask[0] || !gateway[0]) {
            return applied;
        }
        applied |= net_config_static(ip, netmask, gateway);
    }

    if (dns[0]) {
        applied |= net_config_dns(dns);
    }

    return applied;
}
int net_config_static(const char *ip, const char *netmask, const char *gateway) {
    uint8_t parsed_ip[4];
    uint8_t parsed_netmask[4];
    uint8_t parsed_gateway[4];

    if (!parse_ipv4(ip, parsed_ip) ||
        !parse_ipv4(netmask, parsed_netmask) ||
        !parse_ipv4(gateway, parsed_gateway)) {
        return 0;
    }

    memcpy(net_config.ip, parsed_ip, sizeof(net_config.ip));
    memcpy(net_config.netmask, parsed_netmask, sizeof(net_config.netmask));
    memcpy(net_config.gateway, parsed_gateway, sizeof(net_config.gateway));
    net_config.mode = NET_MODE_STATIC;
    net_config.has_config = 1;
    return 1;
}

/* Persists the current in-memory net_config (and LLM net service settings)
 * to /net.cfg in the same key=value format net_load_config_text() parses,
 * so a boot-time `net config static ...`/`net config dhcp` survives reboot. */
int net_save_config(void) {
    extern int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t size);
    extern int fat32_create_file(const char *filename);

    char buf[256];
    uint32_t pos = 0;
    const int dns_set = net_config.dns[0] || net_config.dns[1] || net_config.dns[2] || net_config.dns[3];

    append_str(buf, &pos, sizeof(buf), "mode=");
    append_str(buf, &pos, sizeof(buf), net_config.mode == NET_MODE_STATIC ? "static" : "dhcp");
    append_str(buf, &pos, sizeof(buf), "\n");

    if (net_config.mode == NET_MODE_STATIC) {
        append_str(buf, &pos, sizeof(buf), "ip=");
        append_ip(buf, &pos, sizeof(buf), net_config.ip);
        append_str(buf, &pos, sizeof(buf), "\nnetmask=");
        append_ip(buf, &pos, sizeof(buf), net_config.netmask);
        append_str(buf, &pos, sizeof(buf), "\ngateway=");
        append_ip(buf, &pos, sizeof(buf), net_config.gateway);
        append_str(buf, &pos, sizeof(buf), "\n");
    }

    if (dns_set) {
        append_str(buf, &pos, sizeof(buf), "dns=");
        append_ip(buf, &pos, sizeof(buf), net_config.dns);
        append_str(buf, &pos, sizeof(buf), "\n");
    }

    if (net_config.hostname[0]) {
        append_str(buf, &pos, sizeof(buf), "hostname=");
        append_str(buf, &pos, sizeof(buf), net_config.hostname);
        append_str(buf, &pos, sizeof(buf), "\n");
    }

    append_str(buf, &pos, sizeof(buf), "llm_net=");
    append_str(buf, &pos, sizeof(buf), net_llm_service_enabled() ? "on" : "off");
    append_str(buf, &pos, sizeof(buf), "\nllm_port=");
    {
        char port_buf[8];
        itoa(net_llm_service_port, port_buf, 10);
        append_str(buf, &pos, sizeof(buf), port_buf);
    }
    append_str(buf, &pos, sizeof(buf), "\n");

    if (!fat32_write_file("net.cfg", (uint8_t *)buf, pos)) {
        if (!fat32_create_file("net.cfg")) return 0;
        if (!fat32_write_file("net.cfg", (uint8_t *)buf, pos)) return 0;
    }
    return 1;
}

void net_poll_task(void) {
    while (1) {
        net_poll();
        task_switch();
    }
}
