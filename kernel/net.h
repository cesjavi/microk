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
const net_config_t *net_get_config(void);
const char *net_status_string(void);
int net_config_dhcp(void);
int net_config_static(const char *ip, const char *netmask, const char *gateway);
int net_config_dns(const char *dns);
int net_config_hostname(const char *hostname);
int net_load_config_text(const char *text, uint32_t size);
int net_llm_service_set_enabled(int enabled);
int net_llm_service_enabled(void);
int net_llm_service_set_port(uint16_t port);
const char *net_llm_service_status_string(void);
int net_llm_service_handle_text(const char *request, uint32_t request_len, char *response, uint32_t response_size);

/* ARP Cache and diagnostics */
const char *arp_status_string(void);
void arp_cache_add(const uint8_t ip[4], const uint8_t mac[6]);

#endif
