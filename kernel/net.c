#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "video.h"
#include "vmm.h"
#include <string.h>

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
static int e1000_rings_ready = 0;
static uint32_t e1000_rx_cur = 0;
static uint32_t e1000_tx_tail = 0;

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

/* Endian helpers */
static inline uint16_t ntohs(uint16_t netshort) {
    return (uint16_t)((netshort << 8) | (netshort >> 8));
}

static inline uint16_t htons(uint16_t hostshort) {
    return (uint16_t)((hostshort << 8) | (hostshort >> 8));
}

static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(E1000_MMIO_VMEM + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(E1000_MMIO_VMEM + reg) = value;
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

#include "llm.h"

/* Static formatting helper prototypes to prevent order warnings */
static void append_str(char *out, uint32_t *pos, uint32_t max, const char *text);
static void append_ip(char *out, uint32_t *pos, uint32_t max, const uint8_t ip[4]);
static void append_mac(char *out, uint32_t *pos, uint32_t max, const uint8_t mac[6]);
static int e1000_send_frame(const uint8_t *frame, uint16_t len);

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

int net_llm_service_set_enabled(int enabled) {
    net_llm_service_on = enabled ? 1 : 0;
    return 1;
}

int net_llm_service_enabled(void) {
    return net_llm_service_on;
}

int net_llm_service_set_port(uint16_t port) {
    if (port == 0) {
        return 0;
    }
    net_llm_service_port = port;
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
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_config.link_up ? " link=up" : " link=down");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), net_config.nic_present ? " nic=yes" : " nic=no");
    append_str(net_llm_status_buf, &pos, sizeof(net_llm_status_buf), "\n");
    return net_llm_status_buf;
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

static int net_send_udp_response(const eth_header_t *rx_eth, const ip_header_t *rx_ip, const udp_header_t *rx_udp, const char *payload) {
    uint8_t frame[512];
    eth_header_t *eth = (eth_header_t *)frame;
    ip_header_t *ip = (ip_header_t *)(frame + sizeof(eth_header_t));
    udp_header_t *udp = (udp_header_t *)(frame + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t *udp_payload = frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);
    uint32_t payload_len;
    uint16_t ip_len;
    uint16_t frame_len;

    if (!rx_eth || !rx_ip || !rx_udp || !payload) {
        return 0;
    }

    payload_len = strlen(payload);
    if (payload_len > sizeof(frame) - sizeof(eth_header_t) - sizeof(ip_header_t) - sizeof(udp_header_t)) {
        payload_len = sizeof(frame) - sizeof(eth_header_t) - sizeof(ip_header_t) - sizeof(udp_header_t);
    }

    memcpy(eth->dest, rx_eth->src, sizeof(eth->dest));
    memcpy(eth->src, net_config.mac, sizeof(eth->src));
    eth->type = htons(0x0800);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = 17;
    ip_len = sizeof(ip_header_t) + sizeof(udp_header_t) + payload_len;
    ip->len = htons(ip_len);
    ip->id = rx_ip->id;
    memcpy(ip->src, net_config.ip, sizeof(ip->src));
    memcpy(ip->dest, rx_ip->src, sizeof(ip->dest));
    ip->chksum = htons(net_checksum16(ip, sizeof(ip_header_t)));

    udp->src_port = rx_udp->dest_port;
    udp->dest_port = rx_udp->src_port;
    udp->len = htons(sizeof(udp_header_t) + payload_len);
    udp->chksum = 0;
    memcpy(udp_payload, payload, payload_len);

    frame_len = sizeof(eth_header_t) + ip_len;
    if (frame_len < 60) {
        memset(frame + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    return e1000_send_frame(frame, frame_len);
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
    if (ip->proto != 17) return; // UDP

    uint32_t ip_header_len = (ip->version_ihl & 0x0F) * 4;
    if (ip_header_len < sizeof(ip_header_t)) return;
    if (len < sizeof(eth_header_t) + ip_header_len + sizeof(udp_header_t)) return;

    udp_header_t *udp = (udp_header_t *)(data + sizeof(eth_header_t) + ip_header_len);
    
    // AI Remote Service
    uint16_t dport = (udp->dest_port << 8) | (udp->dest_port >> 8); // Swap for big endian
    if (dport == net_llm_service_port) {
        uint32_t udp_len = (udp->len << 8) | (udp->len >> 8);
        if (udp_len < sizeof(udp_header_t)) return;
        
        uint32_t remaining = len - sizeof(eth_header_t) - ip_header_len;
        if (udp_len > remaining) return; // Truncated/corrupted packet

        char *payload = (char *)((uint8_t *)udp + sizeof(udp_header_t));
        uint32_t payload_len = udp_len - sizeof(udp_header_t);

        char request_buf[128];
        uint32_t to_copy = payload_len > 127 ? 127 : payload_len;
        memcpy(request_buf, payload, to_copy);
        request_buf[to_copy] = '\0';
        to_copy = trim_packet_text(request_buf, to_copy);

        char response[256];
        if (!net_llm_service_handle_text(request_buf, to_copy, response, sizeof(response))) {
            return;
        }

        /*
         * e1000 TX is still pending, so this is the protocol boundary only.
         * Once net_send_udp() exists, response is ready to be sent to src:port.
         */
        klog("Network LLM request: ");
        klog(request_buf);
        klog("\nNetwork LLM response: ");
        klog(response);
        if (net_send_udp_response(eth, ip, udp, response)) {
            klog("Network LLM UDP response queued.");
        } else {
            klog("Network LLM UDP response queue failed.");
        }
    }
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
    volatile uint32_t *eerd = (volatile uint32_t *)(mmio + 0x0014);
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

void net_poll(void) {
    if (!e1000_rings_ready) {
        return;
    }

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
        e1000_rx_cur = (e1000_rx_cur + 1) % E1000_RX_DESC_COUNT;
        if (e1000_rx_cur == 0) {
            e1000_write(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1);
        }
    }
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
                uint32_t ral = *(volatile uint32_t *)(E1000_MMIO_VMEM + 0x5400);
                uint32_t rah = *(volatile uint32_t *)(E1000_MMIO_VMEM + 0x5404);
                
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

int net_config_dhcp(void) {
    net_config.mode = NET_MODE_DHCP;
    net_config.has_config = 1;
    memset(net_config.ip, 0, sizeof(net_config.ip));
    memset(net_config.gateway, 0, sizeof(net_config.gateway));
    memset(net_config.dns, 0, sizeof(net_config.dns));
    return 1;
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
