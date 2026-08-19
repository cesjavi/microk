#include "iwlwifi.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "fat32.h"
#include "video.h"
#include "string.h"
#include "net.h"
#include "wpa2_crypto.h"

#define IWL_VENDOR_INTEL 0x8086
#define IWL_MMIO_VMEM 0xFE200000u
#define IWL_FW_MAX_SIZE (2u * 1024u * 1024u)
#define IWL_FW_DMA_CHUNK_MAX 0x20000u
#define IWL_TLV_UCODE_MAGIC 0x0A4C5749u
#define IWL_TLV_SEC_RT 19u
#define IWL_TLV_SEC_INIT 20u
#define IWL_TLV_DEF_CALIB 22u
#define IWL_TLV_PHY_SKU 23u
#define IWL_TLV_SECURE_SEC_RT 24u
#define IWL_TLV_SECURE_SEC_INIT 25u
#define IWL_TLV_PAGING 32u
#define IWL_TLV_API_CHANGES_SET 29u
#define IWL_TLV_ENABLED_CAPABILITIES 30u
#define IWL_TLV_CMD_VERSIONS 48u
#define IWL_TLV_N_SCAN_CHANNELS 31u
#define IWL_MAX_CMD_VERSIONS 64u
#define IWL_MAX_FW_SECTIONS 32u
#define IWL_CPU1_CPU2_SEPARATOR 0xFFFFCCCCu
#define IWL_PAGING_SEPARATOR 0xAAAABBBBu
#define IWL_CSR_HW_REV 0x028u
#define IWL_CSR_GP_CNTRL 0x024u
#define IWL_CSR_RESET 0x020u
#define IWL_CSR_GIO_CHICKEN_BITS 0x100u
#define IWL_CSR_MAC_SHADOW_CTRL 0x0A8u
#define IWL_CSR_DBG_HPET_MEM 0x240u
#define IWL_CSR_UCODE_DRV_GP1_CLR 0x05Cu
#define IWL_CSR_MAC_ADDR0_OTP 0x380u
#define IWL_CSR_MAC_ADDR1_OTP 0x384u
#define IWL_CSR_MAC_ADDR0_STRAP 0x388u
#define IWL_CSR_MAC_ADDR1_STRAP 0x38Cu
#define IWL_RX_QUEUE_SIZE 512u
#define IWL_RX_BUFFER_SIZE 4096u
#define IWL_RX_METADATA_SIZE 8192u
#define IWL_CMD_QUEUE_ID 0u
#define IWL_CMD_QUEUE_SLOTS 32u
#define IWL_TFD_HW_SLOTS 256u
#define IWL_TFD_SIZE 128u
#define IWL_CMD_BUFFER_SIZE 512u
#define IWL_CMD_TFD_BYTES (IWL_TFD_HW_SLOTS * IWL_TFD_SIZE)
#define IWL_CMD_BUFFER_BYTES (IWL_CMD_QUEUE_SLOTS * IWL_CMD_BUFFER_SIZE)
#define IWL_AUX_TX_SLOTS 32u
#define IWL_AUX_TX_BUFFER_SIZE 2048u
#define IWL_AUX_TX_REGION_BYTES \
    (IWL_CMD_TFD_BYTES + IWL_AUX_TX_SLOTS * IWL_AUX_TX_BUFFER_SIZE)
#define IWL_KEEP_WARM_BYTES 4096u
#define IWL_TX_QUEUE_COUNT 31u
#define IWL_BC_ENTRIES_PER_QUEUE 320u
#define IWL_BC_TABLE_BYTES (IWL_TX_QUEUE_COUNT * IWL_BC_ENTRIES_PER_QUEUE * 2u)

#define IWL_CSR_HW_IF_CONFIG 0x000u
#define IWL_CSR_MBOX_SET 0x088u
#define IWL_HBUS_PRPH_WADDR 0x444u
#define IWL_HBUS_PRPH_RADDR 0x448u
#define IWL_HBUS_PRPH_WDAT 0x44Cu
#define IWL_HBUS_PRPH_RDAT 0x450u
#define IWL_HBUS_MEM_WADDR 0x410u
#define IWL_HBUS_MEM_WDAT 0x418u
#define IWL_HBUS_TARG_WRPTR 0x460u
#define IWL_FH_KW_MEM_ADDR 0x197Cu
#define IWL_FH_CBCC_QUEUE0 0x19D0u
#define IWL_FH_CBCC_QUEUE1 (IWL_FH_CBCC_QUEUE0 + 4u)
#define IWL_PCI_OWN_SET 0x00400000u
#define IWL_HAP_WAKE_L1A 0x00080000u
#define IWL_GIO_L1A_NO_L0S_RX 0x00800000u
#define IWL_DBG_HPET_MAX 0xFFFF0000u
#define IWL_INIT_DONE 0x00000004u
#define IWL_STOP_MASTER 0x00000200u
#define IWL_MASTER_DISABLED 0x00000100u
#define IWL_SW_RESET 0x00000080u
#define IWL_HW_RFKILL_SWITCH 0x08000000u
#define IWL_UCODE_RFKILL 0x00000002u
#define IWL_UCODE_CMD_BLOCKED 0x00000004u
#define IWL_OS_ALIVE (1u << 5)
#define IWL_MAC_ACCESS_REQ 0x00000008u
#define IWL_MAC_CLOCK_READY 0x00000001u
#define IWL_GOING_TO_SLEEP 0x00000010u

#define IWL_RFH_Q0_FRBDCB_BA 0xA08000u
#define IWL_RFH_Q0_FRBDCB_WIDX 0xA08080u
#define IWL_RFH_Q0_FRBDCB_WIDX_TRG 0x1C80u
#define IWL_RFH_Q0_FRBDCB_RIDX 0xA080C0u
#define IWL_RFH_Q0_URBDCB_BA 0xA08100u
#define IWL_RFH_Q0_URBDCB_WIDX 0xA08180u
#define IWL_RFH_Q0_URBD_STTS 0xA08200u
#define IWL_RFH_GEN_CFG 0xA09800u
#define IWL_RFH_RXQ_ACTIVE 0xA0980Cu
#define IWL_RFH_DMA_CFG 0xA09820u

#define IWL_RFH_DMA_CFG_4K_512 0x87940000u
#define IWL_RFH_GEN_CFG_INTEGRATED 0x00000003u
#define IWL_RFH_QUEUE0_ACTIVE 0x00010001u
#define IWL_SCD_BASE 0xA02C00u
#define IWL_SCD_GP_CTRL (IWL_SCD_BASE + 0x1A8u)
#define IWL_SCD_GP_CTRL_AUTO_ACTIVE (1u << 18)
#define IWL_SCD_GP_CTRL_31_QUEUES (1u << 0)
#define IWL_SCD_SRAM_BASE_ADDR (IWL_SCD_BASE + 0x000u)
#define IWL_SCD_DRAM_BASE_ADDR (IWL_SCD_BASE + 0x008u)
#define IWL_SCD_TXFACT (IWL_SCD_BASE + 0x010u)
#define IWL_SCD_QUEUE0_RDPTR (IWL_SCD_BASE + 0x068u)
#define IWL_SCD_QUEUE0_STATUS (IWL_SCD_BASE + 0x10Cu)
#define IWL_SCD_EN_CTRL (IWL_SCD_BASE + 0x254u)
#define IWL_SCD_CONTEXT_QUEUE0 0x600u
#define IWL_SCD_CONTEXT_CLEAR_END 0x81Cu
#define IWL_SCD_CMD_FIFO 7u
#define IWL_SCD_QUEUE_STATUS_MASK 0x017F0000u
#define IWL_SCD_QUEUE_STATUS_ACTIVE (1u << 3)
#define IWL_SCD_QUEUE_STATUS_WSL (1u << 4)
#define IWL_FH_TCSR_BASE 0x1D00u
#define IWL_FH_TX_DMA_ENABLE 0x80000008u
#define IWL_FH_TX_CHICKEN 0x1E98u
#define IWL_FH_TX_AUTO_RETRY (1u << 1)

#define IWL_CSR_INT 0x008u
#define IWL_CSR_INT_FH_TX (1u << 27)
#define IWL_FH_TFDIB_CTRL0 0x1948u
#define IWL_FH_TFDIB_CTRL1 0x194Cu
#define IWL_FH_SERVICE_SRAM_ADDR 0x19C8u
#define IWL_FH_UCODE_LOAD_STATUS 0x1AF0u
#define IWL_FH_SERVICE_CONFIG 0x1E20u
#define IWL_FH_SERVICE_BUF_STATUS 0x1E28u
#define IWL_FH_BUF_VALID 0x00101003u
#define IWL_FH_SERVICE_ENABLE 0x80100000u
#define IWL_RELEASE_CPU_RESET 0x300Cu
#define IWL_RELEASE_CPU_RESET_BIT (1u << 24)

#define IWL_UPLOAD_ERR_ACCESS 1u
#define IWL_UPLOAD_ERR_TIMEOUT 2u
#define IWL_UPLOAD_ERR_LAYOUT 3u
#define IWL_UPLOAD_ERR_ALIVE_TIMEOUT 4u
#define IWL_UPLOAD_ERR_ALIVE_PACKET 5u
#define IWL_UPLOAD_ERR_ALIVE_STATUS 6u

#define IWL_RX_FRAME_SIZE_MASK 0x00003FFFu
#define IWL_RX_FRAME_INVALID 0x55550000u
#define IWL_RX_FRAME_ALIGN 64u
#define IWL_UCODE_ALIVE_NTFY 0x01u
#define IWL_ECHO_CMD 0x03u
#define IWL_NVM_ACCESS_CMD 0x88u
#define IWL_ALIVE_STATUS_OK 0xCAFEu
#define IWL_FW_IMAGE_INIT 1u
#define IWL_FW_IMAGE_RUNTIME 2u
#define IWL_NVM_SECTION_COUNT 13u
#define IWL_NVM_CHUNK_SIZE 2048u
#define IWL_NVM_STORAGE_SIZE (32u * 1024u)
#define IWL_PHY_DB_STORAGE_SIZE (64u * 1024u)
#define IWL_INIT_COMPLETE_NOTIF 0x04u
#define IWL_PHY_CONFIGURATION_CMD 0x6Au
#define IWL_CALIB_RES_NOTIF_PHY_DB 0x6Bu
#define IWL_TX_ANT_CONFIGURATION_CMD 0x98u
#define IWL_FW_PAGING_BLOCK_CMD 0x4Fu
#define IWL_FW_PAGE_SIZE 4096u
#define IWL_FW_PAGING_BLOCK_SIZE (32u * 1024u)
#define IWL_FW_PAGING_MAX_BLOCKS 33u

#define IWL_LONG_GROUP 0x01u
#define IWL_SCAN_CFG_CMD 0x0Cu
#define IWL_SCAN_REQ_UMAC 0x0Du
#define IWL_ADD_STA_CMD 0x18u
#define IWL_PHY_CONTEXT_CMD 0x08u
#define IWL_MAC_CONTEXT_CMD 0x28u
#define IWL_BINDING_CONTEXT_CMD 0x2Bu
#define IWL_SCD_QUEUE_CFG_CMD 0x1Du
#define IWL_AUX_QUEUE_ID 1u
#define IWL_AUX_STA_ID 1u
#define IWL_AUX_MAC_ID 4u
#define IWL_AUX_STA_TYPE 4u
#define IWL_NON_QOS_TID 8u
#define IWL_TX_FIFO_MCAST 5u
#define IWL_FRAME_LIMIT 64u
#define IWL_ADD_STA_SUCCESS 1u
#define IWL_NVM_EXT_CHANNEL_COUNT 51u
#define IWL_NVM_CHANNEL_VALID (1u << 0)
#define IWL_NVM_CHANNEL_20MHZ (1u << 8)
#define IWL_PHY_CONTEXT_COUNT 3u
#define IWL_PHY_BAND_24 1u
#define IWL_PHY_RX_CHAIN_VALID_POS 1u
#define IWL_PHY_RX_CHAIN_COUNT_POS 10u
#define IWL_PHY_RX_CHAIN_MIMO_COUNT_POS 12u
#define IWL_SCAN_CONFIG_BASE_SIZE 36u
#define IWL_SCAN_REQUEST_V8_SIZE 52u
#define IWL_SCAN_CHANNEL_CONFIG_SIZE 8u
#define IWL_SCAN_TAIL_V1_SIZE 1220u
#define IWL_SCAN_DYNAMIC_MAX 2048u
#define IWL_REPLY_RX_PHY_CMD 0xC0u
#define IWL_REPLY_RX_MPDU_CMD 0xC1u
#define IWL_RX_MPDU_CRC_OK (1u << 0)
#define IWL_RX_MPDU_OVERRUN_OK (1u << 1)

typedef struct {
    uint8_t command;
    uint8_t group;
    uint8_t command_version;
    uint8_t notification_version;
} __attribute__((packed)) iwl_fw_cmd_version_t;

typedef struct {
    uint8_t token;
    uint8_t station_id;
    uint8_t tid;
    uint8_t queue;
    uint8_t action;
    uint8_t aggregate;
    uint8_t tx_fifo;
    uint8_t window;
    uint16_t sequence_number;
    uint16_t reserved;
} __attribute__((packed)) iwl_scd_queue_cfg_cmd_t;

typedef struct {
    uint8_t add_modify;
    uint8_t awake_acs;
    uint16_t tid_disable_tx;
    uint32_t mac_id_n_color;
    uint8_t address[6];
    uint16_t reserved2;
    uint8_t station_id;
    uint8_t modify_mask;
    uint16_t reserved3;
    uint32_t station_flags;
    uint32_t station_flags_mask;
    uint8_t add_immediate_ba_tid;
    uint8_t remove_immediate_ba_tid;
    uint16_t add_immediate_ba_ssn;
    uint16_t sleep_tx_count;
    uint8_t sleep_state_flags;
    uint8_t station_type;
    uint16_t association_id;
    uint16_t beamform_flags;
    uint32_t tfd_queue_mask;
    uint16_t rx_ba_window;
    uint8_t service_period_length;
    uint8_t uapsd_acs;
} __attribute__((packed)) iwl_add_sta_cmd_v10_t;

typedef struct {
    uint32_t channel;
    uint8_t band;
    uint8_t width;
    uint8_t control_position;
    uint8_t reserved;
} __attribute__((packed)) iwl_channel_info_v2_t;

typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t apply_time;
    uint32_t tx_param_color;
    iwl_channel_info_v2_t channel_info;
    uint32_t tx_chain_info;
    uint32_t rx_chain_info;
    uint32_t acquisition_data;
    uint32_t dsp_config_flags;
} __attribute__((packed)) iwl_phy_context_cmd_t;

typedef struct {
    uint16_t cw_min;
    uint16_t cw_max;
    uint8_t aifsn;
    uint8_t fifos_mask;
    uint16_t txop;
} __attribute__((packed)) iwl_ac_qos_t;

typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t mac_type;
    uint32_t tsf_id;
    uint8_t node_addr[6];
    uint16_t reserved_node;
    uint8_t bssid_addr[6];
    uint16_t reserved_bssid;
    uint32_t cck_rates;
    uint32_t ofdm_rates;
    uint32_t protection_flags;
    uint32_t short_preamble;
    uint32_t short_slot;
    uint32_t filter_flags;
    uint32_t qos_flags;
    iwl_ac_qos_t ac[4];
    uint32_t station_data[11];
} __attribute__((packed)) iwl_mac_context_cmd_t;

typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t macs[3];
    uint32_t phy;
} __attribute__((packed)) iwl_binding_context_cmd_v1_t;

typedef struct {
    uint16_t length;
    uint16_t offload_assist;
    uint32_t flags;
    uint8_t scratch[4];
    uint32_t rate_n_flags;
    uint8_t station_id;
    uint8_t security_control;
    uint8_t initial_rate_index;
    uint8_t reserved2;
    uint8_t key[16];
    uint32_t reserved3;
    uint32_t lifetime;
    uint32_t dram_lsb_ptr;
    uint8_t dram_msb_ptr;
    uint8_t rts_retry_limit;
    uint8_t data_retry_limit;
    uint8_t tid_tspec;
    uint16_t pm_frame_timeout;
    uint16_t reserved4;
} __attribute__((packed)) iwl_tx_cmd_v6_t;

typedef struct {
    uint8_t station_id;
    uint8_t key_offset;
    uint16_t key_flags;
    uint8_t key[32];
    uint8_t receive_sequence[16];
    uint8_t tkip_tsc_byte2;
    uint8_t reserved;
    uint16_t tkip_ttak[5];
} __attribute__((packed)) iwl_add_sta_key_cmd_v1_t;

typedef struct {
    uint32_t flags;
    uint32_t tx_chains;
    uint32_t rx_chains;
    uint32_t legacy_rates;
    uint32_t out_of_channel_time;
    uint32_t suspend_time;
    uint8_t dwell_active;
    uint8_t dwell_passive;
    uint8_t dwell_fragmented;
    uint8_t dwell_extended;
    uint8_t mac_address[6];
    uint8_t broadcast_station_id;
    uint8_t channel_flags;
    uint8_t channel_array[IWL_NVM_EXT_CHANNEL_COUNT];
} __attribute__((packed)) iwl_scan_config_v1_t;

typedef struct {
    uint32_t flags;
    uint32_t uid;
    uint32_t out_of_channel_priority;
    uint16_t general_flags;
    uint8_t reserved;
    uint8_t scan_start_mac_id;
    uint8_t active_dwell[2];
    uint8_t reserved2;
    uint8_t adaptive_default_aps;
    uint8_t adaptive_social_aps;
    uint8_t general_flags2;
    uint16_t adaptive_max_budget;
    uint32_t max_out_time[2];
    uint32_t suspend_time[2];
    uint32_t scan_priority;
    uint8_t passive_dwell[2];
    uint8_t fragment_count[2];
    uint8_t channel_flags;
    uint8_t channel_count;
    uint16_t channel_reserved;
} __attribute__((packed)) iwl_scan_request_v8_t;

typedef struct {
    uint8_t non_cfg_phy_count, cfg_phy_count, stat_id, reserved;
    uint32_t system_timestamp;
    uint64_t timestamp;
    uint32_t beacon_timestamp;
    uint16_t phy_flags, channel;
    uint32_t non_cfg_phy[8];
    uint32_t rate_n_flags, byte_count;
    uint8_t mac_active_mask, mac_context_info;
    uint16_t frame_time;
} __attribute__((packed)) iwl_rx_phy_info_t;

typedef struct {
    volatile uint16_t closed_rb_num;
    volatile uint16_t closed_fr_num;
    volatile uint16_t finished_rb_num;
    volatile uint16_t finished_fr_num;
    volatile uint32_t spare;
} __attribute__((packed)) iwl_rb_status_t;

typedef struct {
    uint32_t len_n_flags;
    uint8_t cmd;
    uint8_t group_id;
    uint16_t sequence;
    uint8_t data[];
} __attribute__((packed)) iwl_rx_packet_t;

typedef struct {
    uint32_t lo;
    uint16_t hi_n_len;
} __attribute__((packed)) iwl_tfd_tb_t;

typedef struct {
    uint8_t reserved[3];
    uint8_t num_tbs;
    iwl_tfd_tb_t tbs[20];
    uint32_t pad;
} __attribute__((packed)) iwl_tfd_t;

typedef struct {
    uint8_t op_code;
    uint8_t target;
    uint16_t type;
    uint16_t offset;
    uint16_t length;
} __attribute__((packed)) iwl_nvm_access_cmd_t;

typedef struct {
    uint16_t offset;
    uint16_t length;
    uint16_t type;
    uint16_t status;
    uint8_t data[];
} __attribute__((packed)) iwl_nvm_access_resp_t;

typedef struct {
    uint32_t flags;
    uint32_t block_size;
    uint32_t block_num;
    uint32_t device_phy_addr[IWL_FW_PAGING_MAX_BLOCKS];
} __attribute__((packed)) iwl_fw_paging_cmd_t;

typedef struct {
    uint32_t zero;
    uint32_t magic;
    uint8_t human_readable[64];
    uint32_t version;
    uint32_t build;
    uint64_t ignore;
} __attribute__((packed)) iwl_tlv_header_t;

typedef struct {
    uint32_t type;
    uint32_t length;
} __attribute__((packed)) iwl_tlv_t;

static iwlwifi_state_t state;
static uint8_t *firmware_data;
static const uint8_t *boot_firmware_data;
static uint32_t boot_firmware_size;

typedef struct {
    uint32_t device_offset;
    uint32_t data_offset;
    uint32_t length;
    int separator;
} iwl_fw_section_t;

static iwl_fw_section_t runtime_sections[IWL_MAX_FW_SECTIONS];
static iwl_fw_section_t init_sections[IWL_MAX_FW_SECTIONS];
static iwl_fw_cmd_version_t command_versions[IWL_MAX_CMD_VERSIONS];
static uint8_t *dma_staging;
static uint8_t *rx_region;
static uint8_t *command_region;
static uint8_t *aux_tx_region;
static uint32_t aux_tx_write_index;
static uint32_t aux_tx_read_index;
static uint8_t wpa2_pmk[32];
static uint8_t wpa2_ptk[64];
static uint8_t wpa2_anonce[32];
static uint8_t wpa2_snonce[32];
static uint8_t wpa2_replay_counter[8];
static int wpa2_handshake_active;
static int command_wait_active;
static uint8_t pending_eapol[384];
static uint32_t pending_eapol_length;
static uint64_t wpa2_tx_packet_number;
static int runtime_poll_processing;

static int handle_wpa2_eapol(const uint8_t *ethernet, uint32_t length);
static uint8_t *nvm_region;
static uint8_t *nvm_response;
static uint16_t nvm_section_offset[IWL_NVM_SECTION_COUNT];
static uint16_t nvm_section_length[IWL_NVM_SECTION_COUNT];
static uint16_t nvm_channel_flags[IWL_NVM_EXT_CHANNEL_COUNT];
static uint8_t *scan_dynamic_region;
static iwl_rx_phy_info_t last_rx_phy;
static int last_rx_phy_valid;
static const uint16_t nvm_ext_channels[IWL_NVM_EXT_CHANNEL_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177, 181
};
static uint8_t *phy_db_region;
static uint8_t *paging_blocks[IWL_FW_PAGING_MAX_BLOCKS];

static int request_mac_access(void);
static void release_mac_access(void);
static int program_rx_queue(void);
static int handle_runtime_notification(iwl_rx_packet_t *packet,
                                       uint32_t frame_length);

static uint32_t mmio_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(IWL_MMIO_VMEM + reg);
}

static void mmio_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(IWL_MMIO_VMEM + reg) = value;
}

static void prph_write32(uint32_t reg, uint32_t value) {
    /* Bits 25:24 select a full 32-bit transaction on family 9000. */
    mmio_write32(IWL_HBUS_PRPH_WADDR, (reg & 0x000FFFFFu) | (3u << 24));
    mmio_write32(IWL_HBUS_PRPH_WDAT, value);
}

static uint32_t prph_read32(uint32_t reg) {
    mmio_write32(IWL_HBUS_PRPH_RADDR, (reg & 0x000FFFFFu) | (3u << 24));
    return mmio_read32(IWL_HBUS_PRPH_RDAT);
}

static void prph_write64(uint32_t reg, uint64_t value) {
    prph_write32(reg, (uint32_t)value);
    prph_write32(reg + 4u, (uint32_t)(value >> 32));
}

static int is_9560_id(uint16_t vendor, uint16_t device) {
    if (vendor != IWL_VENDOR_INTEL) return 0;
    return device == 0x30DC || device == 0x31DC ||
           device == 0x9DF0 || device == 0xA370;
}

static int valid_mac_address(const uint8_t mac[6]) {
    uint8_t any = 0;
    uint8_t all_ff = 0xFFu;
    for (uint32_t i = 0; i < 6; i++) {
        any |= mac[i];
        all_ff &= mac[i];
    }
    return any != 0 && all_ff != 0xFFu && !(mac[0] & 1u);
}

static void decode_csr_mac(uint32_t low, uint32_t high, uint8_t mac[6]) {
    mac[0] = (uint8_t)(low >> 24);
    mac[1] = (uint8_t)(low >> 16);
    mac[2] = (uint8_t)(low >> 8);
    mac[3] = (uint8_t)low;
    mac[4] = (uint8_t)(high >> 8);
    mac[5] = (uint8_t)high;
}

static int read_csr_mac_address(void) {
    decode_csr_mac(mmio_read32(IWL_CSR_MAC_ADDR0_STRAP),
                   mmio_read32(IWL_CSR_MAC_ADDR1_STRAP), state.mac_address);
    if (!valid_mac_address(state.mac_address)) {
        decode_csr_mac(mmio_read32(IWL_CSR_MAC_ADDR0_OTP),
                       mmio_read32(IWL_CSR_MAC_ADDR1_OTP), state.mac_address);
    }
    state.mac_address_valid = valid_mac_address(state.mac_address);
    return state.mac_address_valid;
}

static int catalog_firmware_section(iwl_fw_section_t *sections,
                                    uint32_t *section_count,
                                    uint32_t *image_bytes,
                                    uint32_t *largest_section,
                                    uint32_t *separators,
                                    uint8_t *data, uint32_t data_offset,
                                    uint32_t tlv_length) {
    if (tlv_length < sizeof(uint32_t)) return 0;

    uint32_t device_offset = *(uint32_t *)(data + data_offset);
    uint32_t section_length = tlv_length - sizeof(uint32_t);
    int separator = device_offset == IWL_CPU1_CPU2_SEPARATOR ||
                    device_offset == IWL_PAGING_SEPARATOR;
    if (section_length == 0 && !separator) return 1;
    if (*section_count >= IWL_MAX_FW_SECTIONS) return 0;
    if (!separator && *image_bytes > IWL_FW_MAX_SIZE - section_length) return 0;

    iwl_fw_section_t *section = &sections[*section_count];
    section->device_offset = device_offset;
    section->data_offset = data_offset + sizeof(uint32_t);
    section->length = section_length;
    section->separator = separator;
    (*section_count)++;
    if (separator) {
        (*separators)++;
    } else {
        *image_bytes += section_length;
        if (section_length > *largest_section) *largest_section = section_length;
    }
    return 1;
}

static int parse_tlv_firmware(uint8_t *data, uint32_t size) {
    if (!data || size < sizeof(iwl_tlv_header_t)) return 0;

    iwl_tlv_header_t *header = (iwl_tlv_header_t *)data;
    if (header->zero != 0 || header->magic != IWL_TLV_UCODE_MAGIC) return 0;

    uint32_t offset = sizeof(iwl_tlv_header_t);
    uint32_t count = 0;
    uint32_t runtime = 0;
    uint32_t runtime_bytes = 0;
    uint32_t largest_runtime_section = 0;
    uint32_t cpu_separators = 0;
    uint32_t init = 0;
    uint32_t init_bytes = 0;
    uint32_t largest_init_section = 0;
    uint32_t init_separators = 0;
    while (offset < size) {
        if (size - offset < sizeof(iwl_tlv_t)) return 0;
        iwl_tlv_t *tlv = (iwl_tlv_t *)(data + offset);
        offset += sizeof(iwl_tlv_t);
        if (tlv->length > size - offset) return 0;
        if (tlv->type == IWL_TLV_SEC_RT || tlv->type == IWL_TLV_SECURE_SEC_RT) {
            if (!catalog_firmware_section(runtime_sections, &runtime,
                                          &runtime_bytes, &largest_runtime_section,
                                          &cpu_separators, data, offset,
                                          tlv->length)) return 0;
        } else if (tlv->type == IWL_TLV_SEC_INIT ||
                   tlv->type == IWL_TLV_SECURE_SEC_INIT) {
            if (!catalog_firmware_section(init_sections, &init, &init_bytes,
                                          &largest_init_section, &init_separators,
                                          data, offset, tlv->length)) return 0;
        } else if (tlv->type == IWL_TLV_PHY_SKU) {
            if (tlv->length != sizeof(uint32_t)) return 0;
            state.phy_config = *(uint32_t *)(data + offset);
            state.valid_tx_ant = (state.phy_config >> 16) & 0xFu;
            state.valid_rx_ant = (state.phy_config >> 20) & 0xFu;
        } else if (tlv->type == IWL_TLV_DEF_CALIB) {
            if (tlv->length != 3u * sizeof(uint32_t)) return 0;
            uint32_t ucode_type = *(uint32_t *)(data + offset);
            if (ucode_type > 1u) return 0;
            state.calib_flow_trigger[ucode_type] =
                *(uint32_t *)(data + offset + 4u);
            state.calib_event_trigger[ucode_type] =
                *(uint32_t *)(data + offset + 8u);
        } else if (tlv->type == IWL_TLV_PAGING) {
            if (tlv->length != sizeof(uint32_t)) return 0;
            state.paging_mem_size = *(uint32_t *)(data + offset);
            if (!state.paging_mem_size ||
                (state.paging_mem_size & (IWL_FW_PAGE_SIZE - 1u))) return 0;
        } else if (tlv->type == IWL_TLV_N_SCAN_CHANNELS) {
            if (tlv->length != sizeof(uint32_t)) return 0;
            state.max_scan_channels = *(uint32_t *)(data + offset);
            if (!state.max_scan_channels || state.max_scan_channels > 63u)
                return 0;
        } else if (tlv->type == IWL_TLV_API_CHANGES_SET ||
                   tlv->type == IWL_TLV_ENABLED_CAPABILITIES) {
            if (tlv->length != 2u * sizeof(uint32_t)) return 0;
            uint32_t index = *(uint32_t *)(data + offset);
            uint32_t bits = *(uint32_t *)(data + offset + 4u);
            if (index >= 4u) return 0;
            if (tlv->type == IWL_TLV_API_CHANGES_SET)
                state.api_bits[index] = bits;
            else
                state.capability_bits[index] = bits;
        } else if (tlv->type == IWL_TLV_CMD_VERSIONS) {
            if ((tlv->length % sizeof(iwl_fw_cmd_version_t)) != 0) return 0;
            uint32_t entries = tlv->length / sizeof(iwl_fw_cmd_version_t);
            if (entries > IWL_MAX_CMD_VERSIONS || state.command_versions != 0)
                return 0;
            memcpy(command_versions, data + offset, tlv->length);
            state.command_versions = entries;
        }
        count++;
        uint32_t padded = (tlv->length + 3u) & ~3u;
        if (padded < tlv->length || padded > size - offset) return 0;
        offset += padded;
    }

    /* Family 9000 predates unified firmware and requires both images. */
    if (offset != size || count == 0 || runtime == 0 || init == 0) return 0;
    state.firmware_version = header->version;
    state.firmware_build = header->build;
    state.tlv_count = count;
    state.runtime_sections = runtime;
    state.runtime_bytes = runtime_bytes;
    state.largest_runtime_section = largest_runtime_section;
    state.runtime_catalog_ready = 1;
    state.cpu_separators = cpu_separators;
    state.init_sections = init;
    state.init_bytes = init_bytes;
    state.largest_init_section = largest_init_section;
    state.init_separators = init_separators;
    state.init_catalog_ready = 1;
    for (uint32_t i = 0; i < 64; i++) {
        uint8_t c = header->human_readable[i];
        if (c == 0) break;
        state.firmware_description[i] = (c >= 32 && c <= 126) ? (char)c : '?';
    }
    state.firmware_description[64] = '\0';

    for (uint32_t i = 0; i < state.command_versions; i++) {
        iwl_fw_cmd_version_t *version = &command_versions[i];
        if (version->group != IWL_LONG_GROUP) continue;
        if (version->command == IWL_SCAN_CFG_CMD)
            state.scan_config_version = version->command_version;
        else if (version->command == IWL_SCAN_REQ_UMAC)
            state.scan_request_version = version->command_version;
        else if (version->command == IWL_ADD_STA_CMD)
            state.add_station_version = version->command_version;
        else if (version->command == IWL_PHY_CONTEXT_CMD)
            state.phy_context_version = version->command_version;
    }
    if (state.scan_config_version != 2u || state.scan_request_version != 8u ||
        state.add_station_version != 10u || state.phy_context_version != 2u)
        return 0;
    if (!state.max_scan_channels) state.max_scan_channels = 40u;
    return 1;
}

static int load_firmware(void) {
    static const char *paths[] = {
        "microk/iwlwifi-9000-pu-b0-jf-b0-46.ucode",
        "iwlwifi-9000-pu-b0-jf-b0-46.ucode",
        "firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode"
    };

    firmware_data = (uint8_t *)pmm_alloc_region(IWL_FW_MAX_SIZE);
    if (!firmware_data) return 0;
    if (boot_firmware_data && boot_firmware_size <= IWL_FW_MAX_SIZE) {
        memcpy(firmware_data, boot_firmware_data, boot_firmware_size);
        if (parse_tlv_firmware(firmware_data, boot_firmware_size)) {
            state.firmware_size = boot_firmware_size;
            strncpy(state.firmware_name, "multiboot:iwlwifi-api46",
                    sizeof(state.firmware_name) - 1u);
            state.firmware_valid = 1;
            return 1;
        }
        return 0; /* explicit boot firmware was present but invalid */
    }

    for (uint32_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint32_t size = 0;
        if (!fat32_load_file(paths[i], firmware_data, IWL_FW_MAX_SIZE, &size)) continue;
        if (!parse_tlv_firmware(firmware_data, size)) {
            klog("WiFi: Intel 9560 firmware found but TLV validation failed.");
            return 0;
        }
        state.firmware_size = size;
        strncpy(state.firmware_name, paths[i], sizeof(state.firmware_name) - 1);
        state.firmware_valid = 1;
        klog("WiFi: Intel 9560 firmware TLV image validated.");
        return 1;
    }
    klog("WiFi: iwlwifi-9000-pu-b0-jf-b0-46.ucode not found on FAT32.");
    return 0;
}

void iwlwifi_set_boot_firmware(const uint8_t *data, uint32_t size) {
    boot_firmware_data = data;
    boot_firmware_size = size;
}

static int prepare_dma_staging(void) {
    dma_staging = (uint8_t *)pmm_alloc_region(IWL_FW_DMA_CHUNK_MAX);
    if (!dma_staging) {
        klog("WiFi: unable to allocate the 128 KiB firmware DMA staging buffer.");
        return 0;
    }
    state.dma_staging_phys = (uint32_t)(uintptr_t)dma_staging;
    state.dma_staging_ready = 1;
    return 1;
}

/* Family 9000 uses 512 64-bit free RBD entries. The low 12 address bits are
 * available because every receive buffer is page-aligned; iwlwifi stores a
 * non-zero virtual buffer ID there so completions can identify ownership. */
static int prepare_rx_queue(void) {
    uint32_t buffers_size = IWL_RX_QUEUE_SIZE * IWL_RX_BUFFER_SIZE;
    uint32_t total_size = IWL_RX_METADATA_SIZE + buffers_size;
    rx_region = (uint8_t *)pmm_alloc_region(total_size);
    if (!rx_region) {
        klog("WiFi: unable to allocate Intel 9560 RX queue memory.");
        return 0;
    }
    memset(rx_region, 0, total_size);

    uint64_t *free_rbd = (uint64_t *)rx_region;
    uint32_t base = (uint32_t)(uintptr_t)rx_region;
    uint32_t buffers_base = base + IWL_RX_METADATA_SIZE;
    for (uint32_t i = 0; i < IWL_RX_QUEUE_SIZE; i++) {
        uint32_t buffer_phys = buffers_base + i * IWL_RX_BUFFER_SIZE;
        free_rbd[i] = (uint64_t)buffer_phys | (uint64_t)(i + 1u);
        *(uint32_t *)(uintptr_t)buffer_phys = IWL_RX_FRAME_INVALID;
    }

    /* Free RBD table is 4096 bytes; the 512-entry used table takes 2048
     * bytes. Keep status at 6144 and align the buffers to the next page. */
    state.rx_descriptor_phys = base;
    state.rx_used_descriptor_phys = base + 4096u;
    state.rx_status_phys = base + 6144u;
    state.rx_buffer_count = IWL_RX_QUEUE_SIZE;
    state.rx_buffer_size = IWL_RX_BUFFER_SIZE;
    state.rx_queue_ready = 1;
    return 1;
}

/* Family 9000 exposes 256 hardware TFD slots even though the software host
 * command window is only 32 entries. Keep every command in one contiguous
 * 512-byte DMA buffer so a TFD can describe it with a single transfer block. */
static int prepare_command_queue(void) {
    uint32_t total_size = IWL_CMD_TFD_BYTES + IWL_CMD_BUFFER_BYTES +
                          IWL_KEEP_WARM_BYTES + IWL_BC_TABLE_BYTES;
    command_region = (uint8_t *)pmm_alloc_region(total_size);
    aux_tx_region = (uint8_t *)pmm_alloc_region(IWL_AUX_TX_REGION_BYTES);
    if (!command_region || !aux_tx_region) {
        klog("WiFi: unable to allocate Intel 9560 command queue DMA memory.");
        return 0;
    }
    memset(command_region, 0, total_size);
    memset(aux_tx_region, 0, IWL_AUX_TX_REGION_BYTES);
    uint32_t base = (uint32_t)(uintptr_t)command_region;
    if (base & 0xFFFu) {
        klog("WiFi: command queue DMA allocation is not page aligned.");
        return 0;
    }
    if ((uintptr_t)aux_tx_region & 0xFFFu) {
        klog("WiFi: auxiliary TX DMA allocation is not page aligned.");
        return 0;
    }
    state.command_tfd_phys = base;
    state.command_buffer_phys = base + IWL_CMD_TFD_BYTES;
    state.keep_warm_phys = base + IWL_CMD_TFD_BYTES + IWL_CMD_BUFFER_BYTES;
    state.command_byte_count_phys = state.keep_warm_phys + IWL_KEEP_WARM_BYTES;
    state.command_queue_allocated = 1;
    return 1;
}

static int acquire_nic_access(pci_device_t *dev) {
    uint32_t command = pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
    command |= 0x00000006u; /* memory decoding + bus mastering */
    pci_config_write32(dev->bus, dev->slot, dev->function, 0x04, command);

    mmio_write32(IWL_CSR_HW_IF_CONFIG,
                 mmio_read32(IWL_CSR_HW_IF_CONFIG) | IWL_PCI_OWN_SET);
    for (uint32_t timeout = 0; timeout < 100000u; timeout++) {
        if (mmio_read32(IWL_CSR_HW_IF_CONFIG) & IWL_PCI_OWN_SET) break;
        if (timeout == 99999u) return 0;
    }
    mmio_write32(IWL_CSR_MBOX_SET, IWL_OS_ALIVE);

    /* Minimal family-9000 APM bring-up from iwl_finish_nic_init(). */
    mmio_write32(IWL_CSR_GIO_CHICKEN_BITS,
                 mmio_read32(IWL_CSR_GIO_CHICKEN_BITS) |
                 IWL_GIO_L1A_NO_L0S_RX);
    mmio_write32(IWL_CSR_DBG_HPET_MEM,
                 mmio_read32(IWL_CSR_DBG_HPET_MEM) | IWL_DBG_HPET_MAX);
    mmio_write32(IWL_CSR_HW_IF_CONFIG,
                 mmio_read32(IWL_CSR_HW_IF_CONFIG) | IWL_HAP_WAKE_L1A);
    mmio_write32(IWL_CSR_GP_CNTRL,
                 mmio_read32(IWL_CSR_GP_CNTRL) | IWL_INIT_DONE);

    mmio_write32(IWL_CSR_GP_CNTRL,
                 mmio_read32(IWL_CSR_GP_CNTRL) | IWL_MAC_ACCESS_REQ);
    for (uint32_t timeout = 0; timeout < 1000000u; timeout++) {
        uint32_t value = mmio_read32(IWL_CSR_GP_CNTRL);
        if ((value & (IWL_MAC_CLOCK_READY | IWL_GOING_TO_SLEEP)) ==
            IWL_MAC_CLOCK_READY) {
            state.nic_access_ready = 1;
            state.hardware_rfkill = !(value & IWL_HW_RFKILL_SWITCH);
            return 1;
        }
    }
    return 0;
}

static void reset_rx_queue_memory(void) {
    uint64_t *free_rbd = (uint64_t *)rx_region;
    uint32_t buffers_base = (uint32_t)(uintptr_t)rx_region + IWL_RX_METADATA_SIZE;
    memset((void *)(uintptr_t)state.rx_used_descriptor_phys, 0, 2048u);
    memset((void *)(uintptr_t)state.rx_status_phys, 0, sizeof(iwl_rb_status_t));
    for (uint32_t i = 0; i < IWL_RX_QUEUE_SIZE; i++) {
        uint32_t buffer_phys = buffers_base + i * IWL_RX_BUFFER_SIZE;
        free_rbd[i] = (uint64_t)buffer_phys | (uint64_t)(i + 1u);
        *(uint32_t *)(uintptr_t)buffer_phys = IWL_RX_FRAME_INVALID;
    }
    state.rx_read_index = 0;
    state.rx_write_index = 0;
    state.rx_write_actual = 0;
}

static int restart_transport(pci_device_t *dev) {
    state.transport_restart_started = 1;
    if (request_mac_access()) {
        prph_write32(IWL_RFH_DMA_CFG, 0);
        prph_write32(IWL_RFH_RXQ_ACTIVE, 0);
        release_mac_access();
    }
    mmio_write32(IWL_CSR_RESET, mmio_read32(IWL_CSR_RESET) | IWL_STOP_MASTER);
    for (uint32_t timeout = 0; timeout < 100000u; timeout++) {
        if (mmio_read32(IWL_CSR_RESET) & IWL_MASTER_DISABLED) break;
        if (timeout == 99999u) {
            state.transport_restart_error = 1;
            return 0;
        }
    }
    mmio_write32(IWL_CSR_RESET, mmio_read32(IWL_CSR_RESET) | IWL_SW_RESET);
    for (volatile uint32_t delay = 0; delay < 5000000u; delay++) {
        __asm__ volatile ("" ::: "memory");
    }
    mmio_write32(IWL_CSR_GP_CNTRL,
                 mmio_read32(IWL_CSR_GP_CNTRL) & ~IWL_INIT_DONE);

    state.nic_access_ready = 0;
    state.rx_hw_programmed = 0;
    state.command_queue_dma_programmed = 0;
    state.command_queue_active = 0;
    state.command_write_index = 0;
    state.command_read_index = 0;
    state.command_echo_sent = 0;
    state.command_echo_complete = 0;
    state.command_error = 0;
    state.scd_base_addr = 0;
    memset(command_region, 0, IWL_CMD_TFD_BYTES + IWL_CMD_BUFFER_BYTES +
                              IWL_KEEP_WARM_BYTES + IWL_BC_TABLE_BYTES);
    memset(aux_tx_region, 0, IWL_AUX_TX_REGION_BYTES);
    aux_tx_write_index = 0;
    aux_tx_read_index = 0;
    reset_rx_queue_memory();
    if (!acquire_nic_access(dev)) {
        state.transport_restart_error = 2;
        return 0;
    }
    mmio_write32(IWL_CSR_MAC_SHADOW_CTRL,
                 mmio_read32(IWL_CSR_MAC_SHADOW_CTRL) | 0x800FFFFFu);
    if (!program_rx_queue()) {
        state.transport_restart_error = 3;
        return 0;
    }
    state.transport_restart_complete = 1;
    return 1;
}

static int request_mac_access(void) {
    mmio_write32(IWL_CSR_GP_CNTRL,
                 mmio_read32(IWL_CSR_GP_CNTRL) | IWL_MAC_ACCESS_REQ);
    for (uint32_t timeout = 0; timeout < 1000000u; timeout++) {
        uint32_t value = mmio_read32(IWL_CSR_GP_CNTRL);
        if ((value & (IWL_MAC_CLOCK_READY | IWL_GOING_TO_SLEEP)) ==
            IWL_MAC_CLOCK_READY) return 1;
    }
    return 0;
}

static void release_mac_access(void) {
    mmio_write32(IWL_CSR_GP_CNTRL,
                 mmio_read32(IWL_CSR_GP_CNTRL) & ~IWL_MAC_ACCESS_REQ);
}

static int program_rx_queue(void) {
    if (!state.nic_access_ready || !state.rx_queue_ready ||
        !state.command_queue_allocated) return 0;

    prph_write32(IWL_RFH_DMA_CFG, 0);
    prph_write32(IWL_RFH_RXQ_ACTIVE, 0);
    prph_write64(IWL_RFH_Q0_FRBDCB_BA, state.rx_descriptor_phys);
    prph_write64(IWL_RFH_Q0_URBDCB_BA, state.rx_used_descriptor_phys);
    prph_write64(IWL_RFH_Q0_URBD_STTS, state.rx_status_phys);
    prph_write32(IWL_RFH_Q0_FRBDCB_WIDX, 0);
    prph_write32(IWL_RFH_Q0_FRBDCB_RIDX, 0);
    prph_write32(IWL_RFH_Q0_URBDCB_WIDX, 0);
    prph_write32(IWL_RFH_DMA_CFG, IWL_RFH_DMA_CFG_4K_512);
    prph_write32(IWL_RFH_GEN_CFG, IWL_RFH_GEN_CFG_INTEGRATED);
    prph_write32(IWL_RFH_RXQ_ACTIVE, IWL_RFH_QUEUE0_ACTIVE);
    /* Announce the command TFD ring and keep-warm page before firmware
     * starts. Queue 0 is the MVM DQA host-command queue on this device. */
    mmio_write32(IWL_FH_CBCC_QUEUE0, state.command_tfd_phys >> 8);
    mmio_write32(IWL_FH_CBCC_QUEUE1,
                 (uint32_t)(uintptr_t)aux_tx_region >> 8);
    mmio_write32(IWL_FH_KW_MEM_ADDR, state.keep_warm_phys >> 4);
    prph_write32(IWL_SCD_GP_CTRL,
                 IWL_SCD_GP_CTRL_AUTO_ACTIVE | IWL_SCD_GP_CTRL_31_QUEUES);
    release_mac_access();

    /* Keep one slot empty, as required by the circular queue, and publish
     * only a multiple of eight through the family-9000 shadow register. */
    state.rx_write_index = IWL_RX_QUEUE_SIZE - 1u;
    state.rx_write_actual = state.rx_write_index & ~7u;
    __asm__ volatile ("" ::: "memory");
    mmio_write32(IWL_RFH_Q0_FRBDCB_WIDX_TRG, state.rx_write_actual);
    state.rx_hw_programmed = 1;
    state.command_queue_dma_programmed = 1;
    return 1;
}

static int recycle_rx_buffer(uint32_t completion_index) {
    volatile uint32_t *used =
        (volatile uint32_t *)(uintptr_t)state.rx_used_descriptor_phys;
    uint32_t virtual_id = used[completion_index] & 0x0FFFu;
    if (virtual_id == 0 || virtual_id > IWL_RX_QUEUE_SIZE) return 0;

    uint32_t buffer_phys = (uint32_t)(uintptr_t)rx_region +
                           IWL_RX_METADATA_SIZE +
                           (virtual_id - 1u) * IWL_RX_BUFFER_SIZE;
    *(volatile uint32_t *)(uintptr_t)buffer_phys = IWL_RX_FRAME_INVALID;
    ((volatile uint64_t *)rx_region)[state.rx_write_index] =
        (uint64_t)buffer_phys | virtual_id;
    state.rx_write_index = (state.rx_write_index + 1u) &
                           (IWL_RX_QUEUE_SIZE - 1u);
    uint32_t publish = state.rx_write_index & ~7u;
    __asm__ volatile ("" ::: "memory");
    if (publish != state.rx_write_actual) {
        mmio_write32(IWL_RFH_Q0_FRBDCB_WIDX_TRG, publish);
        state.rx_write_actual = publish;
    }
    return 1;
}

static int inspect_alive_buffer(uint32_t completion_index) {
    volatile uint32_t *used = (volatile uint32_t *)(uintptr_t)state.rx_used_descriptor_phys;
    uint32_t virtual_id = used[completion_index] & 0x0FFFu;
    if (virtual_id == 0 || virtual_id > IWL_RX_QUEUE_SIZE) return 0;

    uint8_t *buffer = rx_region + IWL_RX_METADATA_SIZE +
                      (virtual_id - 1u) * IWL_RX_BUFFER_SIZE;
    for (uint32_t offset = 0;
         offset + sizeof(iwl_rx_packet_t) <= IWL_RX_BUFFER_SIZE;) {
        iwl_rx_packet_t *packet = (iwl_rx_packet_t *)(buffer + offset);
        uint32_t frame_word = packet->len_n_flags;
        if (frame_word == IWL_RX_FRAME_INVALID) break;
        uint32_t frame_length = frame_word & IWL_RX_FRAME_SIZE_MASK;
        if (frame_length < 4u || frame_length > IWL_RX_BUFFER_SIZE - offset - 4u) return 0;

        state.firmware_notifications++;
        if (packet->cmd == IWL_UCODE_ALIVE_NTFY && packet->group_id == 0) {
            /* status/flags (4), LMAC fixed fields (16), then six 32-bit
             * debug pointers; scd_base_ptr is the sixth one. */
            if (frame_length < 48u) return 0;
            state.firmware_alive_status = *(uint16_t *)packet->data;
            state.firmware_alive_flags = *(uint16_t *)(packet->data + 2u);
            state.scd_base_addr = *(uint32_t *)(packet->data + 40u);
            if (state.firmware_alive_status != IWL_ALIVE_STATUS_OK) {
                state.firmware_upload_error = IWL_UPLOAD_ERR_ALIVE_STATUS;
                return -1;
            }
            if (!state.scd_base_addr) {
                state.firmware_upload_error = IWL_UPLOAD_ERR_ALIVE_PACKET;
                return -1;
            }
            state.firmware_alive = 1;
            return 1;
        }

        uint32_t consumed = (frame_length + 4u + IWL_RX_FRAME_ALIGN - 1u) &
                            ~(IWL_RX_FRAME_ALIGN - 1u);
        if (consumed == 0 || consumed > IWL_RX_BUFFER_SIZE - offset) return 0;
        offset += consumed;
    }
    return 0;
}

static int wait_for_firmware_alive(void) {
    volatile iwl_rb_status_t *status =
        (volatile iwl_rb_status_t *)(uintptr_t)state.rx_status_phys;
    command_wait_active = 1;
    for (uint32_t timeout = 0; timeout < 10000000u; timeout++) {
        __asm__ volatile ("" ::: "memory");
        uint32_t closed = status->closed_rb_num & 0x0FFFu;
        closed &= IWL_RX_QUEUE_SIZE - 1u;
        while (state.rx_read_index != closed) {
            int result = inspect_alive_buffer(state.rx_read_index);
            if (!recycle_rx_buffer(state.rx_read_index)) return 0;
            state.rx_read_index = (state.rx_read_index + 1u) &
                                  (IWL_RX_QUEUE_SIZE - 1u);
            if (result > 0) return 1;
            if (result < 0) return 0;
        }
    }
    if (!state.firmware_upload_error) {
        state.firmware_upload_error = state.firmware_notifications ?
                                      IWL_UPLOAD_ERR_ALIVE_PACKET :
                                      IWL_UPLOAD_ERR_ALIVE_TIMEOUT;
    }
    return 0;
}

static int activate_command_queue(void) {
    if (!state.firmware_alive || !state.command_queue_dma_programmed ||
        !state.scd_base_addr) return 0;
    if (!request_mac_access()) return 0;

    uint32_t hardware_scd = prph_read32(IWL_SCD_SRAM_BASE_ADDR);
    if (hardware_scd != state.scd_base_addr) {
        release_mac_access();
        state.firmware_upload_error = IWL_UPLOAD_ERR_ALIVE_PACKET;
        return 0;
    }

    /* Clear scheduler context/status/translation SRAM, then connect its
     * byte-count table. The table base is expressed in 1 KiB units. */
    mmio_write32(IWL_HBUS_MEM_WADDR, state.scd_base_addr + IWL_SCD_CONTEXT_QUEUE0);
    for (uint32_t address = IWL_SCD_CONTEXT_QUEUE0;
         address < IWL_SCD_CONTEXT_CLEAR_END; address += 4u) {
        mmio_write32(IWL_HBUS_MEM_WDAT, 0);
    }
    prph_write32(IWL_SCD_DRAM_BASE_ADDR, state.command_byte_count_phys >> 10);

    prph_write32(IWL_SCD_EN_CTRL, 0);
    prph_write32(IWL_SCD_QUEUE0_STATUS, 1u << 19); /* inactive while configuring */
    mmio_write32(IWL_HBUS_TARG_WRPTR, IWL_CMD_QUEUE_ID << 8);
    prph_write32(IWL_SCD_QUEUE0_RDPTR, 0);
    mmio_write32(IWL_HBUS_MEM_WADDR, state.scd_base_addr + IWL_SCD_CONTEXT_QUEUE0);
    mmio_write32(IWL_HBUS_MEM_WDAT, 0);
    mmio_write32(IWL_HBUS_MEM_WDAT, 64u | (64u << 16));
    prph_write32(IWL_SCD_QUEUE0_STATUS,
                 IWL_SCD_QUEUE_STATUS_ACTIVE |
                 (IWL_SCD_CMD_FIFO << 0) |
                 IWL_SCD_QUEUE_STATUS_WSL |
                 IWL_SCD_QUEUE_STATUS_MASK);
    prph_write32(IWL_SCD_EN_CTRL, 1u << IWL_CMD_QUEUE_ID);
    prph_write32(IWL_SCD_TXFACT, 0xFFu);

    for (uint32_t channel = 0; channel < 8u; channel++) {
        mmio_write32(IWL_FH_TCSR_BASE + channel * 0x20u,
                     IWL_FH_TX_DMA_ENABLE);
    }
    mmio_write32(IWL_FH_TX_CHICKEN,
                 mmio_read32(IWL_FH_TX_CHICKEN) | IWL_FH_TX_AUTO_RETRY);
    release_mac_access();
    state.command_queue_active = 1;
    return 1;
}

static int handle_init_notification(iwl_rx_packet_t *packet,
                                    uint32_t frame_length) {
    if (packet->group_id != 0) return 1;
    if (packet->cmd == IWL_INIT_COMPLETE_NOTIF) {
        state.init_complete = 1;
        return 1;
    }
    if (packet->cmd != IWL_CALIB_RES_NOTIF_PHY_DB) return 1;
    if (!phy_db_region || frame_length < 8u) return 0;

    uint32_t payload_length = frame_length - 4u;
    uint16_t data_length = *(uint16_t *)(packet->data + 2u);
    uint32_t record_length = 4u + data_length;
    if (record_length > payload_length ||
        state.phy_db_bytes > IWL_PHY_DB_STORAGE_SIZE - record_length) return 0;
    memcpy(phy_db_region + state.phy_db_bytes, packet->data, record_length);
    state.phy_db_bytes += record_length;
    state.phy_db_sections++;
    return 1;
}

static int inspect_command_response(uint32_t completion_index,
                                    uint8_t expected_cmd,
                                    uint8_t expected_group,
                                    uint16_t expected_sequence,
                                    void *response, uint32_t response_capacity,
                                    uint32_t *response_length) {
    volatile uint32_t *used = (volatile uint32_t *)(uintptr_t)state.rx_used_descriptor_phys;
    uint32_t virtual_id = used[completion_index] & 0x0FFFu;
    if (virtual_id == 0 || virtual_id > IWL_RX_QUEUE_SIZE) return -1;
    uint8_t *buffer = rx_region + IWL_RX_METADATA_SIZE +
                      (virtual_id - 1u) * IWL_RX_BUFFER_SIZE;
    for (uint32_t offset = 0;
         offset + sizeof(iwl_rx_packet_t) <= IWL_RX_BUFFER_SIZE;) {
        iwl_rx_packet_t *packet = (iwl_rx_packet_t *)(buffer + offset);
        uint32_t frame_word = packet->len_n_flags;
        if (frame_word == IWL_RX_FRAME_INVALID) break;
        uint32_t frame_length = frame_word & IWL_RX_FRAME_SIZE_MASK;
        if (frame_length < 4u || frame_length > IWL_RX_BUFFER_SIZE - offset - 4u) return -1;
        state.firmware_notifications++;
        if (!handle_init_notification(packet, frame_length)) {
            state.calibration_error = 1;
            return -1;
        }
        if (state.runtime_configured &&
            !handle_runtime_notification(packet, frame_length)) {
            state.command_error = 5;
            return -1;
        }
        if (packet->cmd == expected_cmd && packet->group_id == expected_group &&
            packet->sequence == expected_sequence) {
            uint32_t header_length = expected_group ? 8u : 4u;
            if (frame_length < header_length) return -1;
            uint32_t payload_length = frame_length - header_length;
            uint8_t *payload = packet->data + (expected_group ? 4u : 0u);
            if (response_length) *response_length = payload_length;
            if (response && payload_length) {
                if (payload_length > response_capacity) return -1;
                memcpy(response, payload, payload_length);
            }
            return 1;
        }
        uint32_t consumed = (frame_length + 4u + IWL_RX_FRAME_ALIGN - 1u) &
                            ~(IWL_RX_FRAME_ALIGN - 1u);
        if (!consumed || consumed > IWL_RX_BUFFER_SIZE - offset) return -1;
        offset += consumed;
    }
    return 0;
}

static int send_command_sync_parts(uint8_t opcode, uint8_t group,
                                   uint8_t version, const void *payload,
                                   uint32_t payload_length,
                                   const void *second_part,
                                   uint32_t second_length, void *response,
                                   uint32_t response_capacity,
                                   uint32_t *response_length) {
    uint32_t header_length = group ? 8u : 4u;
    if (!state.command_queue_active ||
        payload_length > IWL_CMD_BUFFER_SIZE - header_length)
        return 0;
    if ((second_length && !second_part) || second_length > 0x0FFFu) return 0;
    if (state.command_write_index - state.command_read_index >=
        IWL_CMD_QUEUE_SLOTS) return 0;

    uint32_t hw_index = state.command_write_index & (IWL_TFD_HW_SLOTS - 1u);
    uint32_t sw_index = state.command_write_index & (IWL_CMD_QUEUE_SLOTS - 1u);
    uint16_t sequence = (uint16_t)((IWL_CMD_QUEUE_ID << 8) | hw_index);
    uint32_t command_length = header_length + payload_length;
    uint8_t *command = command_region + IWL_CMD_TFD_BYTES +
                       sw_index * IWL_CMD_BUFFER_SIZE;
    iwl_tfd_t *tfd = (iwl_tfd_t *)(command_region + hw_index * IWL_TFD_SIZE);
    memset(command, 0, IWL_CMD_BUFFER_SIZE);
    memset(tfd, 0, sizeof(*tfd));
    command[0] = opcode;
    command[1] = group;
    *(uint16_t *)(command + 2u) = sequence;
    if (group) {
        *(uint16_t *)(command + 4u) = (uint16_t)payload_length;
        command[6] = 0;
        command[7] = version;
    }
    if (payload_length) memcpy(command + header_length, payload, payload_length);
    tfd->num_tbs = second_length ? 2 : 1;
    tfd->tbs[0].lo = state.command_buffer_phys +
                     sw_index * IWL_CMD_BUFFER_SIZE;
    tfd->tbs[0].hi_n_len = (uint16_t)(command_length << 4);
    if (second_length) {
        uintptr_t second_phys = (uintptr_t)second_part;
        if (second_phys > 0xFFFFFFFFu) return 0;
        tfd->tbs[1].lo = (uint32_t)second_phys;
        tfd->tbs[1].hi_n_len = (uint16_t)(second_length << 4);
    }
    __asm__ volatile ("" ::: "memory");
    state.command_write_index++;
    mmio_write32(IWL_HBUS_TARG_WRPTR,
                 (state.command_write_index & (IWL_TFD_HW_SLOTS - 1u)) |
                 (IWL_CMD_QUEUE_ID << 8));

    volatile iwl_rb_status_t *status =
        (volatile iwl_rb_status_t *)(uintptr_t)state.rx_status_phys;
    for (uint32_t timeout = 0; timeout < 10000000u; timeout++) {
        __asm__ volatile ("" ::: "memory");
        uint32_t closed = status->closed_rb_num & (IWL_RX_QUEUE_SIZE - 1u);
        while (state.rx_read_index != closed) {
            int result = inspect_command_response(state.rx_read_index,
                                                  opcode, group, sequence,
                                                  response, response_capacity,
                                                  response_length);
            if (!recycle_rx_buffer(state.rx_read_index)) {
                state.command_error = 4;
                command_wait_active = 0;
                return 0;
            }
            state.rx_read_index = (state.rx_read_index + 1u) &
                                  (IWL_RX_QUEUE_SIZE - 1u);
            if (result > 0) {
                state.command_read_index++;
                memset(tfd, 0, sizeof(*tfd));
                command_wait_active = 0;
                return 1;
            }
            if (result < 0) {
                state.command_error = 2;
                command_wait_active = 0;
                return 0;
            }
        }
    }
    state.command_error = 1;
    command_wait_active = 0;
    return 0;
}

static int send_command_sync(uint8_t opcode, const void *payload,
                             uint32_t payload_length, void *response,
                             uint32_t response_capacity,
                             uint32_t *response_length) {
    return send_command_sync_parts(opcode, 0, 0, payload, payload_length,
                                   NULL, 0,
                                   response, response_capacity,
                                   response_length);
}

static int send_command_wide_sync(uint8_t opcode, uint8_t group,
                                  uint8_t version, const void *payload,
                                  uint32_t payload_length, void *response,
                                  uint32_t response_capacity,
                                  uint32_t *response_length) {
    if (!group) return 0;
    return send_command_sync_parts(opcode, group, version, payload,
                                   payload_length, NULL, 0, response,
                                   response_capacity, response_length);
}

static int send_echo_command(void) {
    state.command_echo_sent = 1;
    if (!send_command_sync(IWL_ECHO_CMD, NULL, 0, NULL, 0, NULL)) return 0;
    state.command_echo_complete = 1;
    return 1;
}

static int read_nvm_sections(void) {
    if (state.active_firmware_image != IWL_FW_IMAGE_INIT ||
        !state.command_echo_complete) return 0;
    nvm_region = (uint8_t *)pmm_alloc_region(IWL_NVM_STORAGE_SIZE);
    nvm_response = (uint8_t *)pmm_alloc_region(IWL_RX_BUFFER_SIZE);
    if (!nvm_region || !nvm_response) return 0;
    memset(nvm_region, 0, IWL_NVM_STORAGE_SIZE);
    memset(nvm_section_offset, 0, sizeof(nvm_section_offset));
    memset(nvm_section_length, 0, sizeof(nvm_section_length));

    uint32_t storage_offset = 0;
    for (uint32_t section = 0; section < IWL_NVM_SECTION_COUNT; section++) {
        uint32_t section_start = storage_offset;
        uint32_t section_offset = 0;
        for (;;) {
            iwl_nvm_access_cmd_t command;
            command.op_code = 0; /* IWL_NVM_READ */
            command.target = 0;  /* firmware NVM cache */
            command.type = (uint16_t)section;
            command.offset = (uint16_t)section_offset;
            command.length = IWL_NVM_CHUNK_SIZE;
            uint32_t response_length = 0;
            if (!send_command_sync(IWL_NVM_ACCESS_CMD, &command,
                                   sizeof(command), nvm_response,
                                   IWL_RX_BUFFER_SIZE, &response_length)) {
                state.nvm_error = 1;
                return 0;
            }
            if (response_length < sizeof(iwl_nvm_access_resp_t)) {
                state.nvm_error = 2;
                return 0;
            }
            iwl_nvm_access_resp_t *response =
                (iwl_nvm_access_resp_t *)nvm_response;
            uint32_t bytes = response->length;
            if (response->type != section || response->offset != section_offset ||
                bytes > IWL_NVM_CHUNK_SIZE ||
                bytes > response_length - sizeof(*response)) {
                state.nvm_error = 3;
                return 0;
            }
            if (response->status != 0) {
                /* A missing section is normal; an error after valid chunks
                 * also terminates a section whose size is exactly 2 KiB. */
                if (section_offset == 0) storage_offset = section_start;
                break;
            }
            if (storage_offset > IWL_NVM_STORAGE_SIZE - bytes) {
                state.nvm_error = 4;
                return 0;
            }
            memcpy(nvm_region + storage_offset, response->data, bytes);
            storage_offset += bytes;
            section_offset += bytes;
            if (bytes < IWL_NVM_CHUNK_SIZE) break;
        }
        if (section_offset) {
            nvm_section_offset[section] = (uint16_t)section_start;
            nvm_section_length[section] = (uint16_t)section_offset;
            state.nvm_section_mask |= 1u << section;
        }
    }
    state.nvm_bytes = storage_offset;
    /* Family 9000 NVM_EXT requires SW(1), regulatory(3), MAC override or
     * HW(10), and PHY_SKU(12). Hardware section 10 is the configured one. */
    if (!(state.nvm_section_mask & (1u << 1)) ||
        !(state.nvm_section_mask & (1u << 3)) ||
        !(state.nvm_section_mask & ((1u << 10) | (1u << 11))) ||
        !(state.nvm_section_mask & (1u << 12))) {
        state.nvm_error = 5;
        return 0;
    }
    if (nvm_section_length[3] < sizeof(nvm_channel_flags)) {
        state.nvm_error = 6;
        return 0;
    }
    memcpy(nvm_channel_flags, nvm_region + nvm_section_offset[3],
           sizeof(nvm_channel_flags));
    for (uint32_t i = 0; i < IWL_NVM_EXT_CHANNEL_COUNT; i++) {
        uint16_t flags = nvm_channel_flags[i];
        if ((flags & (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) !=
            (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) continue;
        state.nvm_valid_channels++;
        if (!state.nvm_initial_channel && nvm_ext_channels[i] <= 14u)
            state.nvm_initial_channel = (uint8_t)nvm_ext_channels[i];
    }
    if (!state.nvm_initial_channel) {
        state.nvm_error = 7;
        return 0;
    }
    state.nvm_ready = 1;
    return 1;
}

static int wait_for_init_calibration(void) {
    volatile iwl_rb_status_t *status =
        (volatile iwl_rb_status_t *)(uintptr_t)state.rx_status_phys;
    for (uint32_t timeout = 0; timeout < 20000000u; timeout++) {
        __asm__ volatile ("" ::: "memory");
        uint32_t closed = status->closed_rb_num & (IWL_RX_QUEUE_SIZE - 1u);
        while (state.rx_read_index != closed) {
            volatile uint32_t *used =
                (volatile uint32_t *)(uintptr_t)state.rx_used_descriptor_phys;
            uint32_t virtual_id = used[state.rx_read_index] & 0x0FFFu;
            if (virtual_id == 0 || virtual_id > IWL_RX_QUEUE_SIZE) {
                state.calibration_error = 2;
                return 0;
            }
            uint8_t *buffer = rx_region + IWL_RX_METADATA_SIZE +
                              (virtual_id - 1u) * IWL_RX_BUFFER_SIZE;
            for (uint32_t offset = 0;
                 offset + sizeof(iwl_rx_packet_t) <= IWL_RX_BUFFER_SIZE;) {
                iwl_rx_packet_t *packet = (iwl_rx_packet_t *)(buffer + offset);
                uint32_t frame_word = packet->len_n_flags;
                if (frame_word == IWL_RX_FRAME_INVALID) break;
                uint32_t frame_length = frame_word & IWL_RX_FRAME_SIZE_MASK;
                if (frame_length < 4u ||
                    frame_length > IWL_RX_BUFFER_SIZE - offset - 4u ||
                    !handle_init_notification(packet, frame_length)) {
                    state.calibration_error = 3;
                    return 0;
                }
                state.firmware_notifications++;
                uint32_t consumed = (frame_length + 4u + IWL_RX_FRAME_ALIGN - 1u) &
                                    ~(IWL_RX_FRAME_ALIGN - 1u);
                if (!consumed || consumed > IWL_RX_BUFFER_SIZE - offset) break;
                offset += consumed;
            }
            if (!recycle_rx_buffer(state.rx_read_index)) {
                state.calibration_error = 5;
                return 0;
            }
            state.rx_read_index = (state.rx_read_index + 1u) &
                                  (IWL_RX_QUEUE_SIZE - 1u);
            if (state.init_complete) {
                state.phy_db_ready = state.phy_db_sections != 0;
                return state.phy_db_ready;
            }
        }
    }
    state.calibration_error = 4;
    return 0;
}

static int run_init_calibration(void) {
    if (!state.nvm_ready || !state.valid_tx_ant || !state.valid_rx_ant) return 0;
    if (state.hardware_rfkill) {
        state.calibration_error = 7;
        return 0;
    }
    phy_db_region = (uint8_t *)pmm_alloc_region(IWL_PHY_DB_STORAGE_SIZE);
    if (!phy_db_region) return 0;
    memset(phy_db_region, 0, IWL_PHY_DB_STORAGE_SIZE);
    state.phy_db_sections = 0;
    state.phy_db_bytes = 0;
    state.init_complete = 0;

    uint32_t valid_tx_ant = state.valid_tx_ant;
    if (!send_command_sync(IWL_TX_ANT_CONFIGURATION_CMD, &valid_tx_ant,
                           sizeof(valid_tx_ant), NULL, 0, NULL)) {
        state.calibration_error = 5;
        return 0;
    }
    uint32_t phy_command[3];
    phy_command[0] = state.phy_config;
    phy_command[1] = state.calib_flow_trigger[1]; /* IWL_UCODE_INIT */
    phy_command[2] = state.calib_event_trigger[1];
    if (!send_command_sync(IWL_PHY_CONFIGURATION_CMD, phy_command,
                           sizeof(phy_command), NULL, 0, NULL)) {
        state.calibration_error = 6;
        return 0;
    }
    return wait_for_init_calibration();
}

static int configure_runtime_paging(void) {
    if (state.active_firmware_image != IWL_FW_IMAGE_RUNTIME ||
        !state.paging_mem_size) return 0;
    uint32_t separator = 0;
    while (separator < state.runtime_sections &&
           runtime_sections[separator].device_offset != IWL_PAGING_SEPARATOR)
        separator++;
    if (separator + 2u >= state.runtime_sections) {
        state.paging_error = 1;
        return 0;
    }
    iwl_fw_section_t *css = &runtime_sections[separator + 1u];
    iwl_fw_section_t *paging = &runtime_sections[separator + 2u];
    if (css->separator || paging->separator || css->length > IWL_FW_PAGE_SIZE ||
        paging->length != state.paging_mem_size) {
        state.paging_error = 2;
        return 0;
    }

    uint32_t pages = state.paging_mem_size / IWL_FW_PAGE_SIZE;
    uint32_t data_blocks = (pages + 7u) / 8u;
    uint32_t last_pages = pages - 8u * (data_blocks - 1u);
    if (!data_blocks || data_blocks + 1u > IWL_FW_PAGING_MAX_BLOCKS) {
        state.paging_error = 3;
        return 0;
    }
    paging_blocks[0] = (uint8_t *)pmm_alloc_region(IWL_FW_PAGE_SIZE);
    if (!paging_blocks[0]) {
        state.paging_error = 4;
        return 0;
    }
    memset(paging_blocks[0], 0, IWL_FW_PAGE_SIZE);
    memcpy(paging_blocks[0], firmware_data + css->data_offset, css->length);

    uint32_t copied = 0;
    for (uint32_t block = 1; block <= data_blocks; block++) {
        paging_blocks[block] =
            (uint8_t *)pmm_alloc_region(IWL_FW_PAGING_BLOCK_SIZE);
        if (!paging_blocks[block]) {
            state.paging_error = 5;
            return 0;
        }
        memset(paging_blocks[block], 0, IWL_FW_PAGING_BLOCK_SIZE);
        uint32_t remaining = paging->length - copied;
        uint32_t length = remaining < IWL_FW_PAGING_BLOCK_SIZE ?
                          remaining : IWL_FW_PAGING_BLOCK_SIZE;
        memcpy(paging_blocks[block],
               firmware_data + paging->data_offset + copied, length);
        copied += length;
    }
    if (copied != paging->length) {
        state.paging_error = 6;
        return 0;
    }

    iwl_fw_paging_cmd_t command;
    memset(&command, 0, sizeof(command));
    command.flags = (1u << 9) | (1u << 8) | last_pages;
    command.block_size = 15u; /* log2(32 KiB) */
    command.block_num = data_blocks;
    for (uint32_t block = 0; block <= data_blocks; block++) {
        uintptr_t address = (uintptr_t)paging_blocks[block];
        if (address > 0xFFFFFFFFu || (address & (IWL_FW_PAGE_SIZE - 1u))) {
            state.paging_error = 7;
            return 0;
        }
        command.device_phy_addr[block] = (uint32_t)(address >> 12);
    }
    if (!send_command_wide_sync(IWL_FW_PAGING_BLOCK_CMD, IWL_LONG_GROUP, 0,
                                &command, sizeof(command), NULL, 0, NULL)) {
        state.paging_error = 8;
        return 0;
    }
    state.paging_blocks = data_blocks;
    state.paging_last_pages = last_pages;
    state.paging_ready = 1;
    return 1;
}

static int configure_auxiliary_station(void) {
    if (!state.dqa_enabled || sizeof(iwl_scd_queue_cfg_cmd_t) != 12u ||
        sizeof(iwl_add_sta_cmd_v10_t) != 48u) return 0;

    /* DQA queue 1 is reserved for scan/ROC traffic. With the old TX API,
     * firmware owns its scheduler configuration after its write pointer is
     * initialized by the transport. */
    mmio_write32(IWL_HBUS_TARG_WRPTR, IWL_AUX_QUEUE_ID << 8);
    iwl_scd_queue_cfg_cmd_t queue;
    memset(&queue, 0, sizeof(queue));
    queue.station_id = IWL_AUX_STA_ID;
    queue.tid = IWL_NON_QOS_TID;
    queue.queue = IWL_AUX_QUEUE_ID;
    queue.action = 1u; /* SCD_CFG_ENABLE_QUEUE */
    queue.tx_fifo = IWL_TX_FIFO_MCAST;
    queue.window = IWL_FRAME_LIMIT;
    if (!send_command_sync(IWL_SCD_QUEUE_CFG_CMD, &queue, sizeof(queue),
                           NULL, 0, NULL)) return 0;
    state.auxiliary_queue_ready = 1;

    iwl_add_sta_cmd_v10_t station;
    memset(&station, 0, sizeof(station));
    station.add_modify = state.ap_station_ready ? 1u : 0u;
    station.tid_disable_tx = 0xFFFFu;
    station.mac_id_n_color = IWL_AUX_MAC_ID;
    station.station_id = IWL_AUX_STA_ID;
    station.station_type = IWL_AUX_STA_TYPE;
    station.tfd_queue_mask = 1u << IWL_AUX_QUEUE_ID;
    uint32_t response = 0;
    uint32_t response_length = 0;
    if (!send_command_sync(IWL_ADD_STA_CMD, &station, sizeof(station),
                           &response, sizeof(response), &response_length) ||
        response_length < sizeof(response) ||
        (response & 0xFFu) != IWL_ADD_STA_SUCCESS) return 0;
    state.auxiliary_station_id = IWL_AUX_STA_ID;
    state.auxiliary_station_ready = 1;
    state.management_tx_ready = 1;
    return 1;
}

static int configure_phy_contexts(void) {
    if (!state.auxiliary_station_ready || !state.nvm_initial_channel ||
        sizeof(iwl_phy_context_cmd_t) != 40u) return 0;
    for (uint32_t context = 0; context < IWL_PHY_CONTEXT_COUNT; context++) {
        iwl_phy_context_cmd_t command;
        memset(&command, 0, sizeof(command));
        command.id_and_color = context;
        command.action = 1u; /* FW_CTXT_ACTION_ADD */
        command.channel_info.channel = state.nvm_initial_channel;
        command.channel_info.band = IWL_PHY_BAND_24;
        command.channel_info.width = 0; /* PHY_VHT_CHANNEL_MODE20 */
        command.channel_info.control_position = 0;
        command.tx_chain_info = state.valid_tx_ant;
        command.rx_chain_info =
            ((uint32_t)state.valid_rx_ant << IWL_PHY_RX_CHAIN_VALID_POS) |
            (1u << IWL_PHY_RX_CHAIN_COUNT_POS) |
            (1u << IWL_PHY_RX_CHAIN_MIMO_COUNT_POS);
        if (!send_command_sync(IWL_PHY_CONTEXT_CMD, &command,
                               sizeof(command), NULL, 0, NULL)) return 0;
        state.phy_contexts_ready++;
    }
    return state.phy_contexts_ready == IWL_PHY_CONTEXT_COUNT;
}

static int configure_umac_scan(void) {
    if (state.phy_contexts_ready != IWL_PHY_CONTEXT_COUNT ||
        !(state.capability_bits[0] & (1u << 2)) || /* UMAC_SCAN */
        sizeof(iwl_scan_config_v1_t) !=
            IWL_SCAN_CONFIG_BASE_SIZE + IWL_NVM_EXT_CHANNEL_COUNT) return 0;

    iwl_scan_config_v1_t config;
    memset(&config, 0, sizeof(config));
    uint32_t channels = 0;
    for (uint32_t i = 0; i < IWL_NVM_EXT_CHANNEL_COUNT; i++) {
        uint16_t flags = nvm_channel_flags[i];
        if ((flags & (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) !=
            (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) continue;
        if (channels >= state.max_scan_channels) break;
        config.channel_array[channels++] = (uint8_t)nvm_ext_channels[i];
    }
    if (!channels || channels > 63u) {
        state.scan_error = 1;
        return 0;
    }

    config.flags = (1u << 0) | (1u << 3) | (1u << 8) | (1u << 9) |
                   (1u << 10) | (1u << 11) | (1u << 13) | (1u << 14) |
                   (1u << 15) | (1u << 17) | (channels << 26);
    config.tx_chains = state.valid_tx_ant;
    config.rx_chains = state.valid_rx_ant;
    config.legacy_rates = 0x0FFF0FFFu;
    config.dwell_active = 10u;
    config.dwell_passive = 110u;
    config.dwell_fragmented = 44u;
    config.dwell_extended = 90u;
    memcpy(config.mac_address, state.mac_address, sizeof(config.mac_address));
    config.broadcast_station_id = state.auxiliary_station_id;
    config.channel_flags = 0x0Fu;
    uint32_t length = IWL_SCAN_CONFIG_BASE_SIZE + channels;
    if (!send_command_wide_sync(IWL_SCAN_CFG_CMD, IWL_LONG_GROUP, 0,
                                &config, length, NULL, 0, NULL)) {
        state.scan_error = 2;
        return 0;
    }
    state.scan_config_channels = (uint8_t)channels;
    state.scan_configured = 1;
    return 1;
}

int iwlwifi_start_scan(void) {
    if (!state.scan_configured || state.scan_active ||
        sizeof(iwl_scan_request_v8_t) != IWL_SCAN_REQUEST_V8_SIZE) return 0;
    state.scan_error = 0;
    uint32_t dynamic_length =
        state.max_scan_channels * IWL_SCAN_CHANNEL_CONFIG_SIZE +
        IWL_SCAN_TAIL_V1_SIZE;
    if (dynamic_length > IWL_SCAN_DYNAMIC_MAX) {
        state.scan_error = 3;
        return 0;
    }
    if (!scan_dynamic_region)
        scan_dynamic_region = (uint8_t *)pmm_alloc_region(IWL_SCAN_DYNAMIC_MAX);
    if (!scan_dynamic_region) {
        state.scan_error = 4;
        return 0;
    }
    memset(scan_dynamic_region, 0, IWL_SCAN_DYNAMIC_MAX);

    uint32_t channels = 0;
    for (uint32_t i = 0; i < IWL_NVM_EXT_CHANNEL_COUNT &&
                         channels < state.scan_config_channels; i++) {
        uint16_t flags = nvm_channel_flags[i];
        if ((flags & (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) !=
            (IWL_NVM_CHANNEL_VALID | IWL_NVM_CHANNEL_20MHZ)) continue;
        uint8_t *channel = scan_dynamic_region +
                           channels * IWL_SCAN_CHANNEL_CONFIG_SIZE;
        channel[4] = (uint8_t)nvm_ext_channels[i];
        channel[5] = 1u; /* one iteration */
        channels++;
    }
    if (channels != state.scan_config_channels) {
        state.scan_error = 5;
        return 0;
    }

    /* The tail follows the firmware's maximum-sized channel array, not the
     * number of channels selected for this individual request. */
    uint8_t *tail = scan_dynamic_region +
                    state.max_scan_channels * IWL_SCAN_CHANNEL_CONFIG_SIZE;
    *(uint16_t *)(tail + 0u) = 0; /* first schedule interval */
    tail[2] = 1u;                 /* one scan iteration */

    iwl_scan_request_v8_t request;
    memset(&request, 0, sizeof(request));
    request.flags = 1u << 1; /* IWL_UMAC_SCAN_FLAG_START_NOTIF */
    request.uid = 0;
    request.out_of_channel_priority = 6u;
    request.general_flags = (1u << 2) | (1u << 3) | (1u << 13);
    request.active_dwell[0] = 10u;
    request.adaptive_default_aps = 2u;
    request.adaptive_social_aps = 10u;
    request.general_flags2 = 1u << 1; /* allow channel reorder */
    request.adaptive_max_budget = 300u;
    request.scan_priority = 6u;
    request.passive_dwell[0] = 110u;
    request.channel_count = (uint8_t)channels;

    state.scan_complete = 0;
    state.scan_complete_status = 0;
    state.scan_result_count = 0;
    state.scan_frames = 0;
    memset(state.scan_results, 0, sizeof(state.scan_results));
    last_rx_phy_valid = 0;
    if (!send_command_sync_parts(IWL_SCAN_REQ_UMAC, IWL_LONG_GROUP, 0,
                                 &request, sizeof(request),
                                 scan_dynamic_region, dynamic_length,
                                 NULL, 0, NULL)) {
        state.scan_error = 6;
        return 0;
    }
    state.scan_started = 1;
    state.scan_active = 1;
    return 1;
}

int iwlwifi_prepare_association(uint8_t network_index) {
    int modifying = state.association_context_ready;
    state.association_context_ready = 0;
    state.association_error = 0;
    if (!state.runtime_configured || state.scan_active ||
        network_index >= state.scan_result_count) {
        state.association_error = 1;
        return 0;
    }
    const iwlwifi_scan_result_t *network =
        &state.scan_results[network_index];
    if (!network->channel || sizeof(iwl_mac_context_cmd_t) != 136u ||
        sizeof(iwl_binding_context_cmd_v1_t) != 24u) {
        state.association_error = 2;
        return 0;
    }

    iwl_phy_context_cmd_t phy;
    memset(&phy, 0, sizeof(phy));
    phy.id_and_color = 0;
    phy.action = 2u; /* FW_CTXT_ACTION_MODIFY */
    phy.channel_info.channel = network->channel;
    phy.channel_info.band = network->channel <= 14u ? IWL_PHY_BAND_24 : 0u;
    phy.tx_chain_info = state.valid_tx_ant;
    phy.rx_chain_info =
        ((uint32_t)state.valid_rx_ant << IWL_PHY_RX_CHAIN_VALID_POS) |
        (1u << IWL_PHY_RX_CHAIN_COUNT_POS) |
        (1u << IWL_PHY_RX_CHAIN_MIMO_COUNT_POS);
    if (!send_command_sync(IWL_PHY_CONTEXT_CMD, &phy, sizeof(phy),
                           NULL, 0, NULL)) {
        state.association_error = 3;
        return 0;
    }

    iwl_mac_context_cmd_t mac;
    memset(&mac, 0, sizeof(mac));
    mac.id_and_color = 0;
    mac.action = modifying ? 2u : 1u;
    mac.mac_type = 5u; /* FW_MAC_TYPE_BSS_STA */
    mac.tsf_id = 0;
    memcpy(mac.node_addr, state.mac_address, 6);
    memcpy(mac.bssid_addr, network->bssid, 6);
    mac.cck_rates = network->channel <= 14u ? 0x0Fu : 0u;
    mac.ofdm_rates = 0xFFu;
    mac.filter_flags = 1u << 6; /* MAC_FILTER_IN_BEACON */
    for (uint32_t ac = 0; ac < 4u; ac++) {
        mac.ac[ac].cw_min = 15u;
        mac.ac[ac].cw_max = 1023u;
        mac.ac[ac].aifsn = ac < 2u ? 7u : (ac == 2u ? 2u : 2u);
        mac.ac[ac].fifos_mask = (uint8_t)(1u << ac);
    }
    if (!send_command_sync(IWL_MAC_CONTEXT_CMD, &mac, sizeof(mac),
                           NULL, 0, NULL)) {
        state.association_error = 4;
        return 0;
    }

    iwl_binding_context_cmd_v1_t binding;
    memset(&binding, 0, sizeof(binding));
    binding.id_and_color = 0;
    binding.action = modifying ? 2u : 1u;
    binding.macs[0] = 0;
    binding.macs[1] = 0xFFFFFFFFu;
    binding.macs[2] = 0xFFFFFFFFu;
    binding.phy = 0;
    uint32_t status = 0xFFFFFFFFu;
    uint32_t status_length = 0;
    if (!send_command_sync(IWL_BINDING_CONTEXT_CMD, &binding,
                           sizeof(binding), &status, sizeof(status),
                           &status_length) || status_length < sizeof(status) ||
        status != 0) {
        state.association_error = 5;
        return 0;
    }
    state.association_network = network_index;
    state.association_context_ready = 1;
    return 1;
}

static int send_wifi_frame(const uint8_t *frame, uint16_t frame_length,
                           uint8_t station_id) {
    if (!state.management_tx_ready || !aux_tx_region || !frame ||
        frame_length < 24u ||
        4u + sizeof(iwl_tx_cmd_v6_t) + frame_length >
            IWL_AUX_TX_BUFFER_SIZE) {
        state.management_tx_error = 1;
        return 0;
    }
    if (aux_tx_write_index - aux_tx_read_index >= IWL_AUX_TX_SLOTS) {
        state.management_tx_error = 2;
        return 0;
    }
    uint32_t hw_index = aux_tx_write_index & (IWL_TFD_HW_SLOTS - 1u);
    uint32_t sw_index = aux_tx_write_index & (IWL_AUX_TX_SLOTS - 1u);
    uint8_t *buffer = aux_tx_region + IWL_CMD_TFD_BYTES +
                      sw_index * IWL_AUX_TX_BUFFER_SIZE;
    iwl_tfd_t *tfd = (iwl_tfd_t *)(aux_tx_region +
                                   hw_index * IWL_TFD_SIZE);
    memset(buffer, 0, IWL_AUX_TX_BUFFER_SIZE);
    memset(tfd, 0, sizeof(*tfd));
    buffer[0] = 0x1Cu; /* TX_CMD */
    buffer[1] = 0;
    *(uint16_t *)(buffer + 2u) =
        (uint16_t)((IWL_AUX_QUEUE_ID << 8) | hw_index);
    iwl_tx_cmd_v6_t *tx = (iwl_tx_cmd_v6_t *)(buffer + 4u);
    tx->length = frame_length;
    tx->flags = (1u << 3) | (1u << 13); /* ACK + firmware sequence */
    uint8_t antenna = state.valid_tx_ant & (uint8_t)(-state.valid_tx_ant);
    int band_24 = state.association_network < state.scan_result_count &&
                  state.scan_results[state.association_network].channel <= 14u;
    tx->rate_n_flags = (band_24 ? (10u | (1u << 9)) : 13u) |
                       ((uint32_t)antenna << 14); /* 1M CCK or 6M OFDM */
    tx->station_id = station_id;
    if (*(uint16_t *)frame & 0x4000u) {
        tx->security_control = 2u; /* TX_CMD_SEC_CCM */
        memcpy(tx->key, wpa2_ptk + 32u, 16u);
    }
    tx->lifetime = 0xFFFFFFFFu;
    uintptr_t scratch = (uintptr_t)&tx->scratch[0];
    tx->dram_lsb_ptr = (uint32_t)scratch;
    tx->dram_msb_ptr = 0; /* MicroK's DMA allocator is below 4 GiB */
    tx->rts_retry_limit = 7u;
    tx->data_retry_limit = 15u;
    tx->tid_tspec = IWL_NON_QOS_TID;
    memcpy(buffer + 4u + sizeof(*tx), frame, frame_length);

    uint32_t counted_length = frame_length + 8u;
    if (tx->security_control == 2u) counted_length += 8u;
    uint16_t byte_count = (uint16_t)((counted_length + 3u) / 4u);
    byte_count |= (uint16_t)(station_id << 12);
    volatile uint16_t *table =
        (volatile uint16_t *)(uintptr_t)state.command_byte_count_phys;
    uint32_t table_index = IWL_AUX_QUEUE_ID * IWL_BC_ENTRIES_PER_QUEUE +
                           hw_index;
    table[table_index] = byte_count;
    if (hw_index < 64u)
        table[table_index + IWL_TFD_HW_SLOTS] = byte_count;

    uint32_t total_length = 4u + sizeof(*tx) + frame_length;
    tfd->num_tbs = 1;
    tfd->tbs[0].lo = (uint32_t)(uintptr_t)buffer;
    tfd->tbs[0].hi_n_len = (uint16_t)(total_length << 4);
    __asm__ volatile ("" ::: "memory");
    aux_tx_write_index++;
    mmio_write32(IWL_HBUS_TARG_WRPTR,
                 (aux_tx_write_index & (IWL_TFD_HW_SLOTS - 1u)) |
                 (IWL_AUX_QUEUE_ID << 8));
    state.management_tx_count++;
    return 1;
}

static int send_management_frame(const uint8_t *frame,
                                 uint16_t frame_length) {
    return send_wifi_frame(frame, frame_length, state.auxiliary_station_id);
}

static void build_management_header(uint8_t *frame, uint16_t control,
                                    const uint8_t bssid[6]) {
    memset(frame, 0, 24u);
    *(uint16_t *)frame = control;
    memcpy(frame + 4u, bssid, 6);
    memcpy(frame + 10u, state.mac_address, 6);
    memcpy(frame + 16u, bssid, 6);
}

static int finalize_firmware_association(
    const iwlwifi_scan_result_t *network) {
    iwl_add_sta_cmd_v10_t station;
    memset(&station, 0, sizeof(station));
    station.mac_id_n_color = 0;
    memcpy(station.address, network->bssid, 6);
    station.station_id = 0; /* first non-auxiliary firmware station */
    station.station_flags = (1u << 14) | (1u << 15);
    station.station_flags_mask = (1u << 14) | (1u << 15);
    station.station_type = 0; /* IWL_STA_LINK */
    station.tfd_queue_mask = 1u << IWL_AUX_QUEUE_ID;
    if (station.add_modify) station.modify_mask = 1u << 7;
    uint32_t response = 0;
    uint32_t response_length = 0;
    if (!send_command_sync(IWL_ADD_STA_CMD, &station, sizeof(station),
                           &response, sizeof(response), &response_length) ||
        response_length < sizeof(response) ||
        (response & 0xFFu) != IWL_ADD_STA_SUCCESS) return 0;

    iwl_scd_queue_cfg_cmd_t queue;
    memset(&queue, 0, sizeof(queue));
    queue.station_id = 0;
    queue.tid = IWL_NON_QOS_TID;
    queue.queue = IWL_AUX_QUEUE_ID;
    queue.action = 1u;
    queue.tx_fifo = 1u; /* IWL_MVM_TX_FIFO_BE for the associated link */
    queue.window = IWL_FRAME_LIMIT;
    if (!send_command_sync(IWL_SCD_QUEUE_CFG_CMD, &queue, sizeof(queue),
                           NULL, 0, NULL)) return 0;

    iwl_mac_context_cmd_t mac;
    memset(&mac, 0, sizeof(mac));
    mac.id_and_color = 0;
    mac.action = 2u; /* FW_CTXT_ACTION_MODIFY */
    mac.mac_type = 5u;
    memcpy(mac.node_addr, state.mac_address, 6);
    memcpy(mac.bssid_addr, network->bssid, 6);
    mac.cck_rates = network->channel <= 14u ? 0x0Fu : 0u;
    mac.ofdm_rates = 0xFFu;
    mac.filter_flags = (1u << 6) | (network->rsn ? 0u : (1u << 2));
    for (uint32_t ac = 0; ac < 4u; ac++) {
        mac.ac[ac].cw_min = 15u;
        mac.ac[ac].cw_max = 1023u;
        mac.ac[ac].aifsn = ac < 2u ? 7u : 2u;
        mac.ac[ac].fifos_mask = (uint8_t)(1u << ac);
    }
    mac.station_data[0] = 1u;
    mac.station_data[4] = network->beacon_interval;
    mac.station_data[6] = (uint32_t)network->beacon_interval *
                          (network->dtim_period ? network->dtim_period : 1u);
    mac.station_data[8] = 10u;
    mac.station_data[9] = state.association_id;
    if (!send_command_sync(IWL_MAC_CONTEXT_CMD, &mac, sizeof(mac),
                           NULL, 0, NULL)) return 0;
    state.ap_station_ready = 1;
    return 1;
}

int iwlwifi_associate_open(uint8_t network_index) {
    if (!iwlwifi_prepare_association(network_index)) return 0;
    const iwlwifi_scan_result_t *network =
        &state.scan_results[network_index];
    state.link_association_started = 1;
    state.link_associated = 0;
    state.link_security_ready = 0;
    state.authentication_response = 0;
    state.association_response = 0;

    uint8_t frame[160];
    build_management_header(frame, 0x00B0u, network->bssid);
    *(uint16_t *)(frame + 24u) = 0; /* open-system algorithm */
    *(uint16_t *)(frame + 26u) = 1; /* transaction 1 */
    *(uint16_t *)(frame + 28u) = 0;
    if (!send_management_frame(frame, 30u)) return 0;
    for (uint32_t timeout = 0; timeout < 10000000u &&
                               !state.authentication_response; timeout++)
        iwlwifi_poll();
    if (!state.authentication_response || state.authentication_status) {
        state.association_error = 6;
        return 0;
    }

    build_management_header(frame, 0x0000u, network->bssid);
    *(uint16_t *)(frame + 24u) = network->capability;
    *(uint16_t *)(frame + 26u) = 10u; /* listen interval */
    uint32_t length = 28u;
    frame[length++] = 0u;
    frame[length++] = network->ssid_length;
    memcpy(frame + length, network->ssid, network->ssid_length);
    length += network->ssid_length;
    frame[length++] = 1u; /* supported rates */
    if (network->channel <= 14u) {
        frame[length++] = 4u;
        frame[length++] = 0x82u; frame[length++] = 0x84u;
        frame[length++] = 0x8Bu; frame[length++] = 0x96u;
    } else {
        frame[length++] = 8u;
        frame[length++] = 0x8Cu; frame[length++] = 0x12u;
        frame[length++] = 0x98u; frame[length++] = 0x24u;
        frame[length++] = 0xB0u; frame[length++] = 0x48u;
        frame[length++] = 0x60u; frame[length++] = 0x6Cu;
    }
    if (network->rsn_ie_length &&
        length + network->rsn_ie_length <= sizeof(frame)) {
        memcpy(frame + length, network->rsn_ie, network->rsn_ie_length);
        length += network->rsn_ie_length;
    }
    if (!send_management_frame(frame, (uint16_t)length)) return 0;
    for (uint32_t timeout = 0; timeout < 10000000u &&
                               !state.association_response; timeout++)
        iwlwifi_poll();
    if (!state.association_response || state.association_status) {
        state.association_error = 7;
        return 0;
    }
    if (!finalize_firmware_association(network)) {
        state.association_error = 8;
        return 0;
    }
    state.link_associated = 1;
    state.link_security_ready = !network->rsn;
    return 1;
}

int iwlwifi_send_ethernet(const uint8_t *ethernet, uint16_t length) {
    if (!state.link_associated || !state.ap_station_ready || !ethernet ||
        length < 14u || length > 1514u) return 0;
    uint8_t frame[1536];
    const iwlwifi_scan_result_t *network =
        &state.scan_results[state.association_network];
    int encrypted = state.link_security_ready && network->rsn;
    build_management_header(frame, encrypted ? 0x4108u : 0x0108u,
                            network->bssid);
    memcpy(frame + 16u, ethernet, 6); /* Ethernet destination is addr3 */
    uint32_t payload_offset = 24u;
    if (encrypted) {
        uint64_t pn = ++wpa2_tx_packet_number;
        uint8_t *ccmp = frame + payload_offset;
        ccmp[0] = (uint8_t)pn; ccmp[1] = (uint8_t)(pn >> 8);
        ccmp[2] = 0; ccmp[3] = 0x20u;
        ccmp[4] = (uint8_t)(pn >> 16); ccmp[5] = (uint8_t)(pn >> 24);
        ccmp[6] = (uint8_t)(pn >> 32); ccmp[7] = (uint8_t)(pn >> 40);
        payload_offset += 8u;
    }
    uint8_t *llc = frame + payload_offset;
    llc[0] = 0xAAu;
    llc[1] = 0xAAu;
    llc[2] = 0x03u;
    llc[3] = llc[4] = llc[5] = 0;
    llc[6] = ethernet[12];
    llc[7] = ethernet[13];
    memcpy(llc + 8u, ethernet + 14u, length - 14u);
    if (!send_wifi_frame(frame,
                         (uint16_t)(length + 18u + (encrypted ? 8u : 0u)),
                         0)) return 0;
    state.wifi_tx_packets++;
    return 1;
}

static uint16_t read_be16(const uint8_t *value) {
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static void write_be16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static int send_wpa2_message2(const uint8_t *message1,
                              uint32_t message1_length) {
    if (message1_length < 99u) return 0;
    const iwlwifi_scan_result_t *network =
        &state.scan_results[state.association_network];
    uint8_t ethernet[14u + 99u + 66u];
    memset(ethernet, 0, sizeof(ethernet));
    memcpy(ethernet, network->bssid, 6);
    memcpy(ethernet + 6u, state.mac_address, 6);
    ethernet[12] = 0x88u; ethernet[13] = 0x8Eu;
    uint8_t *eapol = ethernet + 14u;
    uint8_t *key = eapol + 4u;
    eapol[0] = message1[0];
    eapol[1] = 3u;
    uint16_t data_length = network->rsn_ie_length;
    write_be16(eapol + 2u, (uint16_t)(95u + data_length));
    key[0] = message1[4u];
    uint16_t key_info = read_be16(message1 + 5u);
    key_info &= 0x0007u; /* preserve descriptor version */
    key_info |= (1u << 3) | (1u << 8); /* pairwise + MIC */
    write_be16(key + 1u, key_info);
    memcpy(key + 3u, message1 + 7u, 2u); /* key length */
    memcpy(key + 5u, wpa2_replay_counter, 8u);
    memcpy(key + 13u, wpa2_snonce, 32u);
    write_be16(key + 93u, data_length);
    if (data_length) memcpy(key + 95u, network->rsn_ie, data_length);
    uint32_t eapol_length = 99u + data_length;
    uint8_t mic[20];
    wpa2_hmac_sha1(wpa2_ptk, 16u, eapol, eapol_length, mic);
    memcpy(key + 77u, mic, 16u);
    memset(mic, 0, sizeof(mic));
    return iwlwifi_send_ethernet(ethernet,
                                 (uint16_t)(14u + eapol_length));
}

static int install_pairwise_ccmp_key(void) {
    iwl_add_sta_key_cmd_v1_t command;
    memset(&command, 0, sizeof(command));
    if (sizeof(command) != 64u) return 0;
    command.station_id = 0;
    command.key_offset = 0;
    command.key_flags = 2u; /* STA_KEY_FLG_CCM, pairwise key index 0 */
    memcpy(command.key, wpa2_ptk + 32u, 16u);
    uint32_t response = 0;
    uint32_t response_length = 0;
    int success = send_command_sync(0x17u, &command, sizeof(command),
                                    &response, sizeof(response),
                                    &response_length) &&
                  response_length >= sizeof(response) &&
                  (response & 0xFFu) == IWL_ADD_STA_SUCCESS;
    state.wpa2_pairwise_key_ready = success;
    return success;
}

static int install_group_ccmp_key(const uint8_t gtk[16], uint8_t key_id,
                                  const uint8_t receive_sequence[8]) {
    iwl_add_sta_key_cmd_v1_t command;
    memset(&command, 0, sizeof(command));
    command.station_id = 0;
    command.key_offset = 1;
    command.key_flags = (uint16_t)(2u | ((key_id & 3u) << 8) | (1u << 14));
    memcpy(command.key, gtk, 16u);
    memcpy(command.receive_sequence, receive_sequence, 8u);
    uint32_t response = 0, response_length = 0;
    int success = send_command_sync(0x17u, &command, sizeof(command),
                                    &response, sizeof(response),
                                    &response_length) &&
                  response_length >= sizeof(response) &&
                  (response & 0xFFu) == IWL_ADD_STA_SUCCESS;
    if (success) {
        state.wpa2_group_key_id = key_id;
        state.wpa2_group_key_ready = 1;
    }
    return success;
}

static int unwrap_and_install_gtk(const uint8_t *key, uint32_t eapol_length) {
    uint16_t encrypted_length = read_be16(key + 93u);
    if (!encrypted_length || encrypted_length > 256u ||
        99u + encrypted_length > eapol_length ||
        (encrypted_length & 7u)) return 0;
    uint8_t plain[256];
    uint32_t plain_length = sizeof(plain);
    if (!wpa2_aes_key_unwrap(wpa2_ptk + 16u, key + 95u,
                             encrypted_length, plain, &plain_length)) return 0;
    int installed = 0;
    for (uint32_t offset = 0; offset + 2u <= plain_length;) {
        uint8_t id = plain[offset], length = plain[offset + 1u];
        if (length > plain_length - offset - 2u) break;
        const uint8_t *data = plain + offset + 2u;
        if (id == 0xDDu && length >= 22u && data[0] == 0x00u &&
            data[1] == 0x0Fu && data[2] == 0xACu && data[3] == 0x01u) {
            uint8_t key_id = data[4] & 3u;
            installed = install_group_ccmp_key(data + 6u, key_id,
                                               key + 61u);
            break;
        }
        offset += 2u + length;
    }
    memset(plain, 0, sizeof(plain));
    return installed;
}

static int enable_wpa2_group_receive(void) {
    const iwlwifi_scan_result_t *network =
        &state.scan_results[state.association_network];
    iwl_mac_context_cmd_t mac;
    memset(&mac, 0, sizeof(mac));
    mac.id_and_color = 0; mac.action = 2u; mac.mac_type = 5u;
    memcpy(mac.node_addr, state.mac_address, 6);
    memcpy(mac.bssid_addr, network->bssid, 6);
    mac.cck_rates = network->channel <= 14u ? 0x0Fu : 0u;
    mac.ofdm_rates = 0xFFu;
    mac.filter_flags = (1u << 2) | (1u << 6);
    for (uint32_t ac = 0; ac < 4u; ac++) {
        mac.ac[ac].cw_min = 15u; mac.ac[ac].cw_max = 1023u;
        mac.ac[ac].aifsn = ac < 2u ? 7u : 2u;
        mac.ac[ac].fifos_mask = (uint8_t)(1u << ac);
    }
    mac.station_data[0] = 1u;
    mac.station_data[4] = network->beacon_interval;
    mac.station_data[6] = (uint32_t)network->beacon_interval *
                          (network->dtim_period ? network->dtim_period : 1u);
    mac.station_data[8] = 10u;
    mac.station_data[9] = state.association_id;
    return send_command_sync(IWL_MAC_CONTEXT_CMD, &mac, sizeof(mac),
                             NULL, 0, NULL);
}

static int send_wpa2_message4(const uint8_t *message3) {
    const iwlwifi_scan_result_t *network =
        &state.scan_results[state.association_network];
    uint8_t ethernet[14u + 99u];
    memset(ethernet, 0, sizeof(ethernet));
    memcpy(ethernet, network->bssid, 6);
    memcpy(ethernet + 6u, state.mac_address, 6);
    ethernet[12] = 0x88u; ethernet[13] = 0x8Eu;
    uint8_t *eapol = ethernet + 14u;
    uint8_t *key = eapol + 4u;
    eapol[0] = message3[0]; eapol[1] = 3u;
    write_be16(eapol + 2u, 95u);
    key[0] = message3[4u];
    uint16_t info = read_be16(message3 + 5u) & 0x0007u;
    info |= (1u << 3) | (1u << 8) | (1u << 9);
    write_be16(key + 1u, info);
    memcpy(key + 3u, message3 + 7u, 2u);
    memcpy(key + 5u, message3 + 9u, 8u);
    uint8_t mic[20];
    wpa2_hmac_sha1(wpa2_ptk, 16u, eapol, 99u, mic);
    memcpy(key + 77u, mic, 16u); memset(mic, 0, sizeof(mic));
    return iwlwifi_send_ethernet(ethernet, sizeof(ethernet));
}

static int handle_wpa2_eapol(const uint8_t *ethernet, uint32_t length) {
    if (!wpa2_handshake_active || length < 14u + 99u) return 0;
    if (command_wait_active || runtime_poll_processing) {
        if (length <= sizeof(pending_eapol)) {
            memcpy(pending_eapol, ethernet, length);
            pending_eapol_length = length;
        } else {
            state.wpa2_error = 10;
        }
        return 1;
    }
    const uint8_t *eapol = ethernet + 14u;
    uint32_t eapol_length = 4u + read_be16(eapol + 2u);
    if (eapol[1] != 3u || eapol_length < 99u ||
        eapol_length > length - 14u) return 1;
    const uint8_t *key = eapol + 4u;
    uint16_t key_info = read_be16(key + 1u);
    if (key[0] != 2u || (key_info & 7u) != 2u) {
        state.wpa2_error = 12;
        return 1;
    }
    if (!(key_info & (1u << 3))) return 1; /* only pairwise handshake */

    if ((key_info & (1u << 7)) && !(key_info & (1u << 8)) &&
        state.wpa2_stage == 0u) {
        memcpy(wpa2_replay_counter, key + 5u, 8u);
        memcpy(wpa2_anonce, key + 13u, 32u);
        uint8_t seed[47], digest[20];
        memcpy(seed, wpa2_anonce, 32u);
        memcpy(seed + 32u, state.mac_address, 6u);
        memcpy(seed + 38u, wpa2_replay_counter, 8u);
        seed[46] = 0u;
        wpa2_hmac_sha1(wpa2_pmk, sizeof(wpa2_pmk), seed,
                       sizeof(seed), digest);
        memcpy(wpa2_snonce, digest, 20u);
        seed[46] = 1u;
        wpa2_hmac_sha1(wpa2_pmk, sizeof(wpa2_pmk), seed,
                       sizeof(seed), digest);
        memcpy(wpa2_snonce + 20u, digest, 12u);
        const iwlwifi_scan_result_t *network =
            &state.scan_results[state.association_network];
        wpa2_derive_ptk(wpa2_pmk, network->bssid, state.mac_address,
                        wpa2_anonce, wpa2_snonce, wpa2_ptk);
        memset(seed, 0, sizeof(seed)); memset(digest, 0, sizeof(digest));
        if (!send_wpa2_message2(eapol, eapol_length)) {
            state.wpa2_error = 3;
            return 1;
        }
        state.wpa2_stage = 2u;
        return 1;
    }
    if ((key_info & (1u << 7)) && (key_info & (1u << 8)) &&
        (key_info & (1u << 6)) && state.wpa2_stage == 2u) {
        if (memcmp(key + 5u, wpa2_replay_counter, 8u) < 0 ||
            memcmp(key + 13u, wpa2_anonce, 32u)) {
            state.wpa2_error = 13;
            return 1;
        }
        uint8_t copy[384], received_mic[16], calculated[20];
        if (eapol_length > sizeof(copy)) {
            state.wpa2_error = 5;
            return 1;
        }
        memcpy(copy, eapol, eapol_length);
        memcpy(received_mic, copy + 81u, 16u);
        memset(copy + 81u, 0, 16u);
        wpa2_hmac_sha1(wpa2_ptk, 16u, copy, eapol_length, calculated);
        if (!wpa2_constant_time_equal(received_mic, calculated, 16u)) {
            state.wpa2_error = 6;
            memset(copy, 0, sizeof(copy));
            return 1;
        }
        state.wpa2_mic_valid = 1;
        memcpy(wpa2_replay_counter, key + 5u, 8u);
        if (!install_pairwise_ccmp_key()) {
            state.wpa2_error = 7;
            memset(copy, 0, sizeof(copy));
            return 1;
        }
        if ((key_info & (1u << 12)) &&
            !unwrap_and_install_gtk(key, eapol_length)) {
            state.wpa2_error = 9;
            memset(copy, 0, sizeof(copy));
            return 1;
        }
        if (!state.wpa2_group_key_ready || !enable_wpa2_group_receive()) {
            state.wpa2_error = 11;
            memset(copy, 0, sizeof(copy));
            return 1;
        }
        if (!send_wpa2_message4(eapol)) {
            state.wpa2_error = 8;
            memset(copy, 0, sizeof(copy));
            return 1;
        }
        memset(copy, 0, sizeof(copy)); memset(calculated, 0, sizeof(calculated));
        state.wpa2_stage = 4u;
        state.link_security_ready = 1;
        wpa2_handshake_active = 0;
        return 1;
    }
    return 1;
}

int iwlwifi_connect_wpa2(uint8_t network_index, const char *passphrase) {
    if (network_index >= state.scan_result_count ||
        !state.scan_results[network_index].wpa2_psk_ccmp) return 0;
    const iwlwifi_scan_result_t *network = &state.scan_results[network_index];
    memset(wpa2_pmk, 0, sizeof(wpa2_pmk));
    memset(wpa2_ptk, 0, sizeof(wpa2_ptk));
    state.wpa2_stage = 0;
    state.wpa2_error = 0;
    state.wpa2_mic_valid = 0;
    state.wpa2_pairwise_key_ready = 0;
    state.wpa2_group_key_ready = 0;
    wpa2_tx_packet_number = 0;
    if (!wpa2_derive_pmk(passphrase, (const uint8_t *)network->ssid,
                         network->ssid_length, wpa2_pmk)) {
        state.wpa2_error = 1;
        return 0;
    }
    wpa2_handshake_active = 1;
    if (!iwlwifi_associate_open(network_index)) {
        state.wpa2_error = 2;
        wpa2_handshake_active = 0;
        return 0;
    }
    for (uint32_t timeout = 0; timeout < 20000000u &&
                               state.wpa2_stage < 4u; timeout++)
        iwlwifi_poll();
    if (state.wpa2_stage < 4u) {
        if (!state.wpa2_error) state.wpa2_error = 4;
        return 0;
    }
    return 1;
}

static int scan_result_index(const uint8_t bssid[6]) {
    for (uint32_t i = 0; i < state.scan_result_count; i++)
        if (!memcmp(state.scan_results[i].bssid, bssid, 6)) return (int)i;
    return -1;
}

static int rsn_is_wpa2_psk_ccmp(const uint8_t *rsn, uint32_t length) {
    if (length < 18u || rsn[0] != 1u || rsn[1] != 0u) return 0;
    uint32_t offset = 6u;
    uint16_t pairwise_count = rsn[offset] | ((uint16_t)rsn[offset + 1u] << 8);
    offset += 2u;
    if (!pairwise_count || pairwise_count > (length - offset) / 4u) return 0;
    int ccmp = 0;
    for (uint32_t i = 0; i < pairwise_count; i++, offset += 4u)
        if (rsn[offset] == 0x00u && rsn[offset + 1u] == 0x0Fu &&
            rsn[offset + 2u] == 0xACu && rsn[offset + 3u] == 4u) ccmp = 1;
    if (offset + 2u > length) return 0;
    uint16_t akm_count = rsn[offset] | ((uint16_t)rsn[offset + 1u] << 8);
    offset += 2u;
    if (!akm_count || akm_count > (length - offset) / 4u) return 0;
    int psk = 0;
    for (uint32_t i = 0; i < akm_count; i++, offset += 4u)
        if (rsn[offset] == 0x00u && rsn[offset + 1u] == 0x0Fu &&
            rsn[offset + 2u] == 0xACu && rsn[offset + 3u] == 2u) psk = 1;
    return ccmp && psk;
}

static int handle_scan_mpdu(const uint8_t *data, uint32_t data_length) {
    if (!last_rx_phy_valid || data_length < 8u) return 1;
    uint16_t frame_length;
    memcpy(&frame_length, data, sizeof(frame_length));
    if (frame_length < 24u || frame_length > data_length - 8u) return 1;
    const uint8_t *frame = data + 4u;
    uint32_t rx_status;
    memcpy(&rx_status, frame + frame_length, sizeof(rx_status));
    if ((rx_status & (IWL_RX_MPDU_CRC_OK | IWL_RX_MPDU_OVERRUN_OK)) !=
        (IWL_RX_MPDU_CRC_OK | IWL_RX_MPDU_OVERRUN_OK)) return 1;
    uint16_t frame_control;
    memcpy(&frame_control, frame, sizeof(frame_control));
    uint16_t subtype = frame_control & 0x00FCu;
    if ((subtype == 0x00A0u || subtype == 0x00C0u) &&
        frame_length >= 26u && !memcmp(frame + 4u, state.mac_address, 6)) {
        memcpy(&state.disconnect_reason, frame + 24u,
               sizeof(state.disconnect_reason));
        state.link_associated = 0;
        state.link_security_ready = 0;
        wpa2_handshake_active = 0;
        return 1;
    }
    if ((frame_control & 0x000Cu) == 0x0008u) {
        int group_destination = (frame[4] & 1u) != 0;
        if ((frame_control & 0x0300u) != 0x0200u ||
            (memcmp(frame + 4u, state.mac_address, 6) &&
             !(group_destination && state.link_security_ready))) return 1;
        uint32_t header_length = (subtype & 0x0080u) ? 26u : 24u;
        uint32_t crypto_header = 0, crypto_trailer = 0;
        if (frame_control & 0x4000u) {
            if (!(rx_status & (1u << 6)) || !(rx_status & (1u << 11)))
                return 1;
            crypto_header = 8u;
            crypto_trailer = 8u;
        }
        if (frame_length < header_length + crypto_header + 8u +
                           crypto_trailer) return 1;
        header_length += crypto_header;
        const uint8_t *llc = frame + header_length;
        if (llc[0] != 0xAAu || llc[1] != 0xAAu || llc[2] != 0x03u ||
            llc[3] || llc[4] || llc[5]) return 1;
        uint32_t payload_length = frame_length - header_length - 8u -
                                  crypto_trailer;
        if (payload_length > 1500u) return 1;
        uint8_t ethernet[1514];
        memcpy(ethernet, frame + 4u, 6);
        memcpy(ethernet + 6u, frame + 16u, 6);
        ethernet[12] = llc[6];
        ethernet[13] = llc[7];
        memcpy(ethernet + 14u, llc + 8u, payload_length);
        state.wifi_rx_packets++;
        if (ethernet[12] == 0x88u && ethernet[13] == 0x8Eu &&
            handle_wpa2_eapol(ethernet, payload_length + 14u)) return 1;
        net_handle_packet(ethernet, payload_length + 14u);
        return 1;
    }
    if (subtype == 0x00B0u && frame_length >= 30u &&
        !memcmp(frame + 4u, state.mac_address, 6)) {
        uint16_t transaction;
        memcpy(&transaction, frame + 26u, sizeof(transaction));
        if (transaction == 2u) {
            memcpy(&state.authentication_status, frame + 28u,
                   sizeof(state.authentication_status));
            state.authentication_response = 1;
        }
        return 1;
    }
    if (subtype == 0x0010u && frame_length >= 30u &&
        !memcmp(frame + 4u, state.mac_address, 6)) {
        memcpy(&state.association_status, frame + 26u,
               sizeof(state.association_status));
        memcpy(&state.association_id, frame + 28u,
               sizeof(state.association_id));
        state.association_id &= 0x3FFFu;
        state.association_response = 1;
        return 1;
    }
    if ((subtype != 0x0080u && subtype != 0x0050u) ||
        frame_length < 36u) return 1;

    state.scan_frames++;
    int index = scan_result_index(frame + 16u);
    if (index < 0) {
        if (state.scan_result_count >= IWLWIFI_MAX_SCAN_RESULTS) return 1;
        index = state.scan_result_count++;
        memcpy(state.scan_results[index].bssid, frame + 16u, 6);
    }
    iwlwifi_scan_result_t *result = &state.scan_results[index];
    memcpy(&result->capability, frame + 34u, sizeof(result->capability));
    memcpy(&result->beacon_interval, frame + 32u,
           sizeof(result->beacon_interval));
    result->channel = (uint8_t)last_rx_phy.channel;
    uint32_t energy = last_rx_phy.non_cfg_phy[1];
    uint8_t best = 0xFFu;
    for (uint32_t shift = 0; shift < 24u; shift += 8u) {
        uint8_t antenna = (uint8_t)(energy >> shift);
        if (antenna && antenna < best) best = antenna;
    }
    result->rssi_dbm = best == 0xFFu ? 0 : -(int8_t)best;
    result->rsn = 0;
    for (uint32_t offset = 36u; offset + 2u <= frame_length;) {
        uint8_t id = frame[offset];
        uint8_t length = frame[offset + 1u];
        offset += 2u;
        if (length > frame_length - offset) break;
        if (id == 0u && length <= 32u) {
            memcpy(result->ssid, frame + offset, length);
            result->ssid[length] = '\0';
            result->ssid_length = length;
        } else if (id == 3u && length == 1u) {
            result->channel = frame[offset];
        } else if (id == 48u) {
            result->rsn = 1;
            if (length <= 64u) {
                result->rsn_ie[0] = id;
                result->rsn_ie[1] = length;
                memcpy(result->rsn_ie + 2u, frame + offset, length);
                result->rsn_ie_length = length + 2u;
                result->wpa2_psk_ccmp =
                    rsn_is_wpa2_psk_ccmp(frame + offset, length);
            }
        } else if (id == 5u && length >= 2u) {
            result->dtim_period = frame[offset + 1u];
        }
        offset += length;
    }
    return 1;
}

static int handle_runtime_notification(iwl_rx_packet_t *packet,
                                       uint32_t frame_length) {
    uint32_t data_length = frame_length - 4u;
    if (packet->cmd == 0x1Cu && packet->group_id == 0 &&
        (packet->sequence >> 8) == IWL_AUX_QUEUE_ID) {
        uint32_t completed = (packet->sequence & 0xFFu) + 1u;
        uint32_t base = aux_tx_read_index & ~0xFFu;
        if (completed <= (aux_tx_read_index & 0xFFu)) base += 256u;
        if (base + completed <= aux_tx_write_index)
            aux_tx_read_index = base + completed;
        return 1;
    }
    if (packet->cmd == IWL_REPLY_RX_PHY_CMD && packet->group_id == 0) {
        if (data_length < sizeof(last_rx_phy)) return 0;
        memcpy(&last_rx_phy, packet->data, sizeof(last_rx_phy));
        last_rx_phy_valid = 1;
    } else if (packet->cmd == IWL_REPLY_RX_MPDU_CMD && packet->group_id == 0) {
        return handle_scan_mpdu(packet->data, data_length);
    }
    if (packet->cmd == 0x0Fu && packet->group_id == 0) {
        /* struct iwl_umac_scan_complete: UID(4), schedule, iteration,
         * status, EBS status, elapsed(4), reserved(4). */
        if (frame_length < 20u) return 0;
        uint32_t uid = *(uint32_t *)packet->data;
        if (uid != 0) return 1;
        state.scan_complete_status = packet->data[6];
        state.scan_active = 0;
        state.scan_complete = 1;
    }
    return 1;
}

void iwlwifi_poll(void) {
    if (!state.runtime_configured || !state.rx_hw_programmed) return;
    volatile iwl_rb_status_t *status =
        (volatile iwl_rb_status_t *)(uintptr_t)state.rx_status_phys;
    __asm__ volatile ("" ::: "memory");
    uint32_t closed = status->closed_rb_num & (IWL_RX_QUEUE_SIZE - 1u);
    runtime_poll_processing = 1;
    while (state.rx_read_index != closed) {
        volatile uint32_t *used =
            (volatile uint32_t *)(uintptr_t)state.rx_used_descriptor_phys;
        uint32_t virtual_id = used[state.rx_read_index] & 0x0FFFu;
        if (!virtual_id || virtual_id > IWL_RX_QUEUE_SIZE) {
            state.scan_error = 7;
            state.rx_read_index = (state.rx_read_index + 1u) &
                                  (IWL_RX_QUEUE_SIZE - 1u);
            continue;
        }
        uint8_t *buffer = rx_region + IWL_RX_METADATA_SIZE +
                          (virtual_id - 1u) * IWL_RX_BUFFER_SIZE;
        int malformed = 0;
        for (uint32_t offset = 0;
             offset + sizeof(iwl_rx_packet_t) <= IWL_RX_BUFFER_SIZE;) {
            iwl_rx_packet_t *packet = (iwl_rx_packet_t *)(buffer + offset);
            uint32_t frame_word = packet->len_n_flags;
            if (frame_word == IWL_RX_FRAME_INVALID) break;
            uint32_t frame_length = frame_word & IWL_RX_FRAME_SIZE_MASK;
            if (frame_length < 4u ||
                frame_length > IWL_RX_BUFFER_SIZE - offset - 4u ||
                !handle_runtime_notification(packet, frame_length)) {
                state.scan_error = 8;
                malformed = 1;
                break;
            }
            state.firmware_notifications++;
            uint32_t consumed =
                (frame_length + 4u + IWL_RX_FRAME_ALIGN - 1u) &
                ~(IWL_RX_FRAME_ALIGN - 1u);
            if (!consumed || consumed > IWL_RX_BUFFER_SIZE - offset) {
                state.scan_error = 9;
                malformed = 1;
                break;
            }
            offset += consumed;
        }
        if (!recycle_rx_buffer(state.rx_read_index)) {
            state.scan_error = 10;
            runtime_poll_processing = 0;
            return;
        }
        state.rx_read_index = (state.rx_read_index + 1u) &
                              (IWL_RX_QUEUE_SIZE - 1u);
        if (malformed) continue;
    }
    runtime_poll_processing = 0;
    if (pending_eapol_length && !command_wait_active) {
        uint32_t length = pending_eapol_length;
        pending_eapol_length = 0;
        handle_wpa2_eapol(pending_eapol, length);
        memset(pending_eapol, 0, length);
    }
}

static int configure_runtime_firmware(void) {
    if (state.active_firmware_image != IWL_FW_IMAGE_RUNTIME ||
        !state.command_echo_complete || !state.phy_db_ready) return 0;

    uint32_t valid_tx_ant = state.valid_tx_ant;
    if (!send_command_sync(IWL_TX_ANT_CONFIGURATION_CMD, &valid_tx_ant,
                           sizeof(valid_tx_ant), NULL, 0, NULL)) {
        state.runtime_config_error = 1;
        return 0;
    }

    uint32_t offset = 0;
    while (offset < state.phy_db_bytes) {
        if (state.phy_db_bytes - offset < 4u) {
            state.runtime_config_error = 2;
            return 0;
        }
        uint8_t *record = phy_db_region + offset;
        uint16_t length = *(uint16_t *)(record + 2u);
        if ((uint32_t)length > state.phy_db_bytes - offset - 4u) {
            state.runtime_config_error = 3;
            return 0;
        }
        if (!send_command_sync_parts(0x6Cu, 0, 0, record, 4u,
                                     record + 4u, length, NULL, 0, NULL)) {
            state.runtime_config_error = 4;
            return 0;
        }
        offset += 4u + length;
        state.phy_db_sections_sent++;
    }

    uint32_t phy_command[3];
    phy_command[0] = state.phy_config;
    phy_command[1] = state.calib_flow_trigger[0]; /* IWL_UCODE_REGULAR */
    phy_command[2] = state.calib_event_trigger[0];
    if (!send_command_sync(IWL_PHY_CONFIGURATION_CMD, phy_command,
                           sizeof(phy_command), NULL, 0, NULL)) {
        state.runtime_config_error = 5;
        return 0;
    }
    if (state.capability_bits[0] & (1u << 12)) { /* DQA_SUPPORT */
        uint32_t command_queue = IWL_CMD_QUEUE_ID;
        if (!send_command_wide_sync(0x00u, 0x05u, 0, &command_queue,
                                    sizeof(command_queue), NULL, 0, NULL)) {
            state.runtime_config_error = 6;
            return 0;
        }
        state.dqa_enabled = 1;
    }
    if (!configure_auxiliary_station()) {
        state.runtime_config_error = 7;
        return 0;
    }
    if (!configure_phy_contexts()) {
        state.runtime_config_error = 8;
        return 0;
    }
    if (!configure_umac_scan()) {
        state.runtime_config_error = 9;
        return 0;
    }
    state.runtime_configured = 1;
    return 1;
}

static int upload_firmware_chunk(uint32_t destination, const uint8_t *source,
                                 uint32_t length) {
    if (!source || length == 0 || length > IWL_FW_DMA_CHUNK_MAX) return 0;
    memcpy(dma_staging, source, length);
    __asm__ volatile ("" ::: "memory");

    if (!request_mac_access()) {
        state.firmware_upload_error = IWL_UPLOAD_ERR_ACCESS;
        return 0;
    }
    mmio_write32(IWL_CSR_INT, IWL_CSR_INT_FH_TX);
    mmio_write32(IWL_FH_SERVICE_CONFIG, 0);
    mmio_write32(IWL_FH_SERVICE_SRAM_ADDR, destination);
    mmio_write32(IWL_FH_TFDIB_CTRL0, state.dma_staging_phys);
    mmio_write32(IWL_FH_TFDIB_CTRL1, length);
    mmio_write32(IWL_FH_SERVICE_BUF_STATUS, IWL_FH_BUF_VALID);
    mmio_write32(IWL_FH_SERVICE_CONFIG, IWL_FH_SERVICE_ENABLE);
    release_mac_access();

    for (uint32_t timeout = 0; timeout < 5000000u; timeout++) {
        uint32_t interrupt = mmio_read32(IWL_CSR_INT);
        if (interrupt & IWL_CSR_INT_FH_TX) {
            mmio_write32(IWL_CSR_INT, IWL_CSR_INT_FH_TX);
            state.firmware_chunks_loaded++;
            return 1;
        }
    }
    state.firmware_upload_error = IWL_UPLOAD_ERR_TIMEOUT;
    return 0;
}

static int upload_section(iwl_fw_section_t *sections, uint32_t section_index) {
    iwl_fw_section_t *section = &sections[section_index];
    for (uint32_t offset = 0; offset < section->length; offset += IWL_FW_DMA_CHUNK_MAX) {
        uint32_t remaining = section->length - offset;
        uint32_t size = remaining < IWL_FW_DMA_CHUNK_MAX ? remaining : IWL_FW_DMA_CHUNK_MAX;
        state.firmware_error_section = section_index;
        state.firmware_error_offset = offset;
        if (!upload_firmware_chunk(section->device_offset + offset,
                                   firmware_data + section->data_offset + offset,
                                   size)) return 0;
    }
    return 1;
}

static int set_ucode_load_status(uint32_t value) {
    if (!request_mac_access()) return 0;
    mmio_write32(IWL_FH_UCODE_LOAD_STATUS, value);
    release_mac_access();
    return 1;
}

static int upload_firmware_image(iwl_fw_section_t *sections,
                                 uint32_t section_count,
                                 uint32_t image_type) {
    if (!state.rx_hw_programmed || !state.dma_staging_ready || !sections ||
        section_count == 0) return 0;
    state.firmware_upload_started = 1;
    state.firmware_upload_complete = 0;
    state.firmware_alive = 0;
    state.firmware_alive_status = 0;
    state.firmware_alive_flags = 0;
    state.firmware_upload_error = 0;
    state.firmware_chunks_loaded = 0;
    state.firmware_error_section = 0;
    state.firmware_error_offset = 0;
    mmio_write32(IWL_CSR_UCODE_DRV_GP1_CLR,
                 IWL_UCODE_RFKILL | IWL_UCODE_CMD_BLOCKED);

    if (!request_mac_access()) {
        state.firmware_upload_error = IWL_UPLOAD_ERR_ACCESS;
        return 0;
    }
    prph_write32(IWL_RELEASE_CPU_RESET, IWL_RELEASE_CPU_RESET_BIT);
    release_mac_access();

    uint32_t index = 0;
    uint32_t section_bits = 1;
    uint32_t shift = 0;
    int saw_cpu_separator = 0;
    int saw_paging_separator = 0;
    while (index < section_count) {
        iwl_fw_section_t *section = &sections[index];
        if (section->separator) {
            if (section->device_offset == IWL_CPU1_CPU2_SEPARATOR &&
                !saw_cpu_separator && !saw_paging_separator) {
                if (!set_ucode_load_status(0x0000FFFFu)) return 0;
                saw_cpu_separator = 1;
                shift = 16;
                section_bits = 1;
                index++;
                continue;
            }
            if (section->device_offset == IWL_PAGING_SEPARATOR &&
                saw_cpu_separator && !saw_paging_separator) {
                saw_paging_separator = 1;
                break; /* paging is installed later through FW_PAGING_BLOCK_CMD */
            }
            state.firmware_upload_error = IWL_UPLOAD_ERR_LAYOUT;
            return 0;
        }
        if (!upload_section(sections, index)) return 0;
        if (!request_mac_access()) return 0;
        uint32_t status = mmio_read32(IWL_FH_UCODE_LOAD_STATUS);
        mmio_write32(IWL_FH_UCODE_LOAD_STATUS, status | (section_bits << shift));
        release_mac_access();
        section_bits = (section_bits << 1) | 1u;
        index++;
    }
    if (image_type == IWL_FW_IMAGE_RUNTIME &&
        (!saw_cpu_separator || !saw_paging_separator)) {
        state.firmware_upload_error = IWL_UPLOAD_ERR_LAYOUT;
        return 0;
    }
    /* INIT API-46 has no separator; Linux's secured family-8000 loader
     * completes CPU1 then performs an empty CPU2 pass and writes all bits. */
    if (!set_ucode_load_status(0xFFFFFFFFu)) return 0;

    state.firmware_upload_complete = 1;
    if (!wait_for_firmware_alive()) return 0;
    if (!activate_command_queue()) return 0;
    state.active_firmware_image = image_type;
    return send_echo_command();
}

void iwlwifi_probe(void) {
    memset(&state, 0, sizeof(state));
    int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev || !is_9560_id(dev->vendor_id, dev->device_id)) continue;

        state.present = 1;
        state.device_id = dev->device_id;
        state.bus = dev->bus;
        state.slot = dev->slot;
        state.function = dev->function;

        uint32_t bar0 = dev->bar[0];
        uint32_t bar_type = (bar0 >> 1) & 3u;
        state.mmio_bar_64bit = bar_type == 2u;
        /* A 64-bit BAR is still usable by the 32-bit VMM when its upper
         * dword is zero. Reject only addresses that are actually above 4 GiB. */
        if (state.mmio_bar_64bit && dev->bar[1] != 0) {
            state.mmio_above_4g = 1;
            klog("WiFi: Intel 9560 BAR0 is above 4 GiB and cannot be mapped.");
        } else if (bar0 && bar0 != 0xFFFFFFFFu && !(bar0 & 1u) &&
                   (bar_type == 0u || bar_type == 2u)) {
            state.mmio_phys = bar0 & ~0x0Fu;
            /* CSR occupies the first page; the legacy FH service channel used
             * by family 9000 extends through offsets 0x1000..0x1fff. */
            for (uint32_t offset = 0; offset < 0x3000; offset += 0x1000) {
                vmm_map_page_ext(IWL_MMIO_VMEM + offset, state.mmio_phys + offset, 0x1B);
            }
            state.mmio_mapped = 1;
            state.hw_revision = mmio_read32(IWL_CSR_HW_REV);
            state.csr_gp_control = mmio_read32(IWL_CSR_GP_CNTRL);
            read_csr_mac_address();
        }

        klog("WiFi: Intel Wireless-AC 9560 PCI device detected.");
        if (!state.mmio_mapped) {
            klog("WiFi: Intel 9560 has no usable MMIO BAR; initialization stopped.");
            return;
        }
        if (load_firmware()) {
            if (prepare_dma_staging()) {
                if (prepare_rx_queue() && prepare_command_queue() &&
                    acquire_nic_access(dev)) {
                    if (program_rx_queue()) {
                        if (upload_firmware_image(init_sections,
                                                  state.init_sections,
                                                  IWL_FW_IMAGE_INIT)) {
                            if (read_nvm_sections() && run_init_calibration() &&
                                restart_transport(dev) &&
                                upload_firmware_image(runtime_sections,
                                                      state.runtime_sections,
                                                      IWL_FW_IMAGE_RUNTIME)) {
                                if (configure_runtime_paging())
                                    configure_runtime_firmware();
                            }
                        }
                    }
                }
            }
        }
        return;
    }
}

const iwlwifi_state_t *iwlwifi_get_state(void) {
    return &state;
}
