#ifndef NET_H
#define NET_H

#include <stdint.h>

typedef enum {
    NET_MODE_DHCP = 0,
    NET_MODE_STATIC = 1
} net_mode_t;

typedef struct {
    net_mode_t mode;
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint8_t mac[6];
    char hostname[32];
    int link_up;
    int nic_present;
    uint16_t nic_vendor_id;
    uint16_t nic_device_id;
    uint32_t nic_mmio_base;
    uint32_t nic_io_base;
    char nic_name[16];
    int has_config;
} net_config_t;

void net_init(void);
void net_detect_pci(void);
void net_handle_packet(uint8_t *data, uint32_t len);
void net_poll(void);
int net_wifi_scan(void);
int net_wifi_associate(uint8_t network_index);
int net_wifi_connect_wpa2(uint8_t network_index, const char *passphrase);
const net_config_t *net_get_config(void);
const char *net_status_string(void);
int net_config_dhcp(void);
int net_config_static(const char *ip, const char *netmask, const char *gateway);
int net_config_dns(const char *dns);
int net_config_hostname(const char *hostname);
int net_load_config_text(const char *text, uint32_t size);
int net_save_config(void);
int net_llm_service_set_enabled(int enabled);
int net_llm_service_enabled(void);
int net_llm_service_set_port(uint16_t port);
const char *net_llm_service_status_string(void);
int net_llm_service_handle_text(const char *request, uint32_t request_len, char *response, uint32_t response_size);

/* ARP Cache and diagnostics */
const char *arp_status_string(void);
void arp_cache_add(const uint8_t ip[4], const uint8_t mac[6]);

/* ICMP ping: returns RTT in ms on success, 0 on timeout, <0 on error */
int net_ping(const char *ip_str);

/* UDP: send a datagram; dest_ip=255.255.255.255 uses broadcast MAC. Resolves
 * dest_mac via the ARP cache (blocking on a fresh ARP request/reply if not
 * yet cached) -- callers replying to a peer whose MAC is already known (e.g.
 * from the frame that triggered the reply) should prefer
 * net_send_udp_to_mac() instead, both to skip the round trip and because
 * that resolution can't make progress at all when called from inside the
 * RX dispatch path (see net_send_udp_to_mac's doc comment). */
int net_send_udp(const uint8_t dest_ip[4], uint16_t dest_port, uint16_t src_port,
                 const uint8_t *payload, uint16_t payload_len);

/* UDP: send a datagram straight to a known dest_mac, no ARP cache lookup.
 * Mirrors how ICMP echo replies already reply using the request frame's
 * source MAC directly (see net_send_icmp_echo_reply in net.c) instead of
 * going through the ARP cache -- UDP port handlers invoked from
 * net_handle_packet() (itself called from inside net_poll()'s RX loop)
 * should use this with the src_mac their handler was given, rather than
 * net_send_udp(): net_send_udp()'s ARP-miss path waits by calling
 * net_poll() in a loop, which is a guaranteed no-op while already nested
 * inside net_poll() (net_poll_active is 1 the whole time), so any reply
 * needing a fresh ARP resolution from that context can never actually
 * complete. */
int net_send_udp_to_mac(const uint8_t dest_ip[4], const uint8_t dest_mac[6],
                        uint16_t dest_port, uint16_t src_port,
                        const uint8_t *payload, uint16_t payload_len);

/* UDP port binding: register/unregister a receive handler. src_mac is the
 * Ethernet source address of the frame that carried this datagram -- pass
 * it straight to net_send_udp_to_mac() when replying (see its doc comment
 * for why net_send_udp() can silently fail to reply from this context). */
typedef void (*net_udp_handler_t)(const uint8_t src_ip[4], const uint8_t src_mac[6],
                                   uint16_t src_port,
                                   const uint8_t *payload, uint16_t len);
int  net_udp_bind(uint16_t port, net_udp_handler_t handler);
void net_udp_unbind(uint16_t port);

/* DNS: resolve hostname to IPv4. Returns 1+fills out_ip on success, 0 on failure.
 * Also accepts dotted-quad strings directly (no query needed). */
int net_dns_resolve(const char *hostname, uint8_t out_ip[4]);

/* Blocking HTTP/1.0 GET, single connection at a time (see kernel/net.c for
 * the TCP client this is built on). Returns bytes written to out (>=0) on
 * success, negative on failure. */
int net_http_get(const char *host, uint16_t port, const char *path, char *out, uint32_t out_size);

/* LLM service token: empty string or "off" disables auth */
int net_llm_service_set_token(const char *token);

/* Remote shell (RSH) UDP service */
int net_rsh_set_enabled(int enabled);
int net_rsh_set_port(uint16_t port);
int net_rsh_set_token(const char *token);
const char *net_rsh_status_string(void);

/* Background kernel task: loops calling net_poll() with cooperative yields */
void net_poll_task(void);

#endif
