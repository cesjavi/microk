#include "syscall.h"
#include "task.h"
#include "video.h"
#include "ai_hooks.h"
#include "keyboard.h"
#include "llm.h"
#include "speaker.h"
#include "initrd.h"
#include "serial.h"
#include "pmm.h"
#include "kheap.h"
#include "net.h"
#include "gpu.h"
#include "vmm.h"
#include "ipc.h"
#include <string.h>

extern struct kernel_metrics metrics;

/* Validate a user pointer: non-null, no overflow, and every page has U/S=1. */
static int uptr_ok(uint32_t ptr, uint32_t len) {
    return ptr != 0 && vmm_user_ptr_ok(ptr, len);
}
/* Validate a user C-string pointer (check one page at minimum). */
static int ustr_ok(uint32_t ptr) {
    return ptr != 0 && vmm_user_ptr_ok(ptr, 1);
}

void syscall_init() {
}

void sys_print(char *msg) {
    if (msg) {
        klog(msg);
    }
}

uint32_t syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t retval = 0;
    switch (syscall_num) {
        case 1: // Print
            {
                extern void kprint(const char *msg);
                if (ustr_ok(arg1)) kprint((char *)arg1);
            }
            break;
        case 2: // Yield
            task_switch();
            break;
        case 3: // Get Metrics
            if (uptr_ok(arg1, sizeof(struct kernel_metrics)))
                *((struct kernel_metrics *)arg1) = metrics;
            break;
        case 4: // Get Char
            if (uptr_ok(arg1, 1)) *((char *)arg1) = keyboard_get_char();
            break;
        case 5: // LLM Query
            if (ustr_ok(arg1) && uptr_ok(arg2, 128))
                llm_inference((char *)arg1, (char *)arg2);
            break;
        case 6: // Beep
            speaker_beep(arg1, arg2);
            break;
        case 7: // Read File (arg1=name, arg2=buffer)
            {
                if (!ustr_ok(arg1) || !uptr_ok(arg2, 256)) break;
                vfs_node_t *node = initrd_find_node((char *)arg1);
                if (node) {
                    uint32_t size = node->length;
                    if (size > 255) size = 255;
                    uint32_t read = vfs_read(node, 0, size, (uint8_t *)arg2);
                    ((char *)arg2)[read] = '\0';
                }
            }
            break;
        case 8: // Put Char (Direct)
            {
                video_putc((char)arg1, 0x07);
                serial_putc((char)arg1);
            }
            break;
        case 9: // Clear Screen
            video_clear(0x07);
            break;
        case 10: // Physical Memory Stats
            if (arg1) pmm_get_stats((pmm_stats_t *)arg1);
            break;
        case 11: // Kernel Heap Stats
            if (arg1) kheap_get_stats((kheap_stats_t *)arg1);
            break;
        case 12: // Kernel Heap Selftest
            if (arg1) *((int *)arg1) = kheap_selftest();
            break;
        case 13: // FAT32 LS
            {
                extern void fat32_ls();
                extern void fat32_ls_path(const char *path);
                if (arg1 && ustr_ok(arg1)) fat32_ls_path((char *)arg1);
                else if (!arg1) fat32_ls();
            }
            break;
        case 14: // FAT32 CAT
            {
                extern void fat32_cat(const char *filename);
                if (ustr_ok(arg1)) fat32_cat((char *)arg1);
            }
            break;
        case 15: // EXT LS
            {
                extern void extfs_ls();
                extfs_ls();
            }
            break;
        case 16: // EXT CAT
            {
                extern void extfs_cat(const char *filename);
                if (ustr_ok(arg1)) extfs_cat((char *)arg1);
            }
            break;
        case 17: // LLM Load Model
            {
                extern void llm_load_model_module(uint32_t start, uint32_t end, const char *name);
                extern int llm_model_supported();
                // arg1 = addr, arg2 = size, arg3 = name_ptr
                if (uptr_ok(arg1, arg2) && arg1 + arg2 >= arg1 && (!arg3 || ustr_ok(arg3))) {
                    llm_load_model_module(arg1, arg1 + arg2, (char *)arg3);
                    retval = llm_model_supported();
                }
            }
            break;
        case 18: // Get LLM Status String
            {
                extern const char* llm_status_string();
                if (uptr_ok(arg1, 256)) {
                    strncpy((char *)arg1, llm_status_string(), 255);
                    ((char *)arg1)[255] = '\0';
                }
            }
            break;
        case 19: // Get LLM Info String
            {
                extern const char* llm_info_string();
                if (uptr_ok(arg1, 256)) {
                    strncpy((char *)arg1, llm_info_string(), 255);
                    ((char *)arg1)[255] = '\0';
                }
            }
            break;
        case 21: // FAT32 Create File
            {
                extern int fat32_create_file(const char *filename);
                if (ustr_ok(arg1) && uptr_ok(arg2, sizeof(int)))
                    *((int *)arg2) = fat32_create_file((char *)arg1);
            }
            break;
        case 22: // FAT32 Write File
            {
                extern int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t size);
                if (ustr_ok(arg1) && uptr_ok(arg2, arg3 ? arg3 : 1))
                    fat32_write_file((char *)arg1, (uint8_t *)arg2, arg3);
            }
            break;
        case 23: // FAT32 Delete File
            {
                extern int fat32_delete_file(const char *filename);
                if (arg1 && arg2) *((int *)arg2) = fat32_delete_file((char *)arg1);
            }
            break;
        case 24: // FAT32 MKDIR
            {
                extern int fat32_mkdir(const char *path);
                if (arg1 && arg2) *((int *)arg2) = fat32_mkdir((char *)arg1);
            }
            break;
        case 25: // Net Status String
            if (uptr_ok(arg1, 256)) {
                strncpy((char *)arg1, net_status_string(), 255);
                ((char *)arg1)[255] = '\0';
            }
            break;
        case 26: // Net Config DHCP
            if (uptr_ok(arg1, sizeof(int))) *((int *)arg1) = net_config_dhcp();
            break;
        case 27: // Net Config Static
            if (uptr_ok(arg1, sizeof(char *) * 3) && uptr_ok(arg2, sizeof(int))) {
                char **args = (char **)arg1;
                if (ustr_ok((uint32_t)(uintptr_t)args[0]) &&
                    ustr_ok((uint32_t)(uintptr_t)args[1]) &&
                    ustr_ok((uint32_t)(uintptr_t)args[2])) {
                    *((int *)arg2) = net_config_static(args[0], args[1], args[2]);
                }
            }
            break;
        case 28: // Memory Region Count
            if (uptr_ok(arg1, sizeof(uint32_t))) *((uint32_t *)arg1) = pmm_memory_region_count();
            break;
        case 29: // Memory Region Get
            if (uptr_ok(arg2, sizeof(pmm_memory_region_t)) && uptr_ok(arg3, sizeof(int)))
                *((int *)arg3) = pmm_get_memory_region(arg1, (pmm_memory_region_t *)arg2);
            break;
        case 30: // GPU Status String
            if (uptr_ok(arg1, 768)) {
                strncpy((char *)arg1, gpu_status_string(), 767);
                ((char *)arg1)[767] = '\0';
            }
            break;
        case 31: // FAT32 Get File Size
            {
                extern uint32_t fat32_get_size(const char *filename);
                if (arg1) retval = fat32_get_size((char *)arg1);
            }
            break;
        case 32: // FAT32 Read File
            {
                extern int fat32_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);
                uint32_t dummy_size;
                if (arg1 && arg2) {
                    retval = fat32_load_file((char *)arg1, (uint8_t *)arg2, arg3, &dummy_size);
                }
            }
            break;
        case 33: // FAT32 Get Entry by Index
            {
                extern int fat32_get_entry_by_index(const char *path, uint32_t index, char *out_name, int *out_is_dir);
                int is_dir = 0;
                if (arg1 && arg3) {
                    retval = fat32_get_entry_by_index((char *)arg1, arg2, (char *)arg3, &is_dir);
                }
            }
            break;
        case 34: // PMM Allocate Contiguous Region
            if (arg1) retval = (uint32_t)pmm_alloc_region(arg1);
            break;
        case 35: // LLM Load Model File
            {
                extern int llm_load_model_file(const char *name);
                if (ustr_ok(arg1)) retval = llm_load_model_file((char *)arg1);
            }
            break;
        case 36: // FAT32 Is Directory
            {
                extern int fat32_path_is_dir(const char *path);
                if (ustr_ok(arg1)) retval = fat32_path_is_dir((char *)arg1);
            }
            break;
        case 37: // LLM Trace Set
            {
                extern void llm_set_trace(int enabled);
                llm_set_trace((int)arg1);
            }
            break;
        case 38: // LLM Trace Status
            {
                extern int llm_trace_enabled(void);
                retval = (uint32_t)llm_trace_enabled();
            }
            break;
        case 39: // ARP Status String
            {
                extern const char* arp_status_string(void);
                if (uptr_ok(arg1, 512)) {
                    strncpy((char *)arg1, arp_status_string(), 511);
                    ((char *)arg1)[511] = '\0';
                }
            }
            break;
        case 40: // High Memory (PAE) Verification Test
            {
                klog("PAE Test: Initializing verification...");
                phys_addr_t phys = pmm_alloc_high_block();
                int real_alloc = 1;
                if (phys == 0) {
                    klog("PAE Test: No physical high RAM (>4GB) available. Emulating high block at 0x100005000.");
                    phys = 0x100005000ULL;
                    real_alloc = 0;
                } else {
                    klog("PAE Test: Successfully allocated physical high block above 4GB.");
                }
                
                extern void *vmm_temp_map_high(phys_addr_t phys);
                extern void vmm_temp_unmap_high(void);
                
                char *window = (char *)vmm_temp_map_high(phys);
                klog("PAE Test: Mapped physical page to temporal virtual window at 0xFF000000.");
                
                const char *sig = "MicroK PAE High Memory Verification Successful!";
                strncpy(window, sig, 127);
                window[127] = '\0';
                klog("PAE Test: Wrote verification signature to mapping.");
                
                char readback[128];
                strncpy(readback, window, 127);
                readback[127] = '\0';
                
                vmm_temp_unmap_high();
                klog("PAE Test: Unmapped virtual page and flushed TLB.");
                
                if (real_alloc) {
                    extern void pmm_free_high_block(phys_addr_t phys);
                    pmm_free_high_block(phys);
                    klog("PAE Test: Released physical high block.");
                }
                
                if (strcmp(readback, sig) == 0) {
                    klog("PAE Test: Readback verified perfectly!");
                    retval = 1;
                } else {
                    klog("PAE Test: Readback MISMATCH! Test failed.");
                    retval = 0;
                }
            }
            break;
        case 41: // LLM Network Service Enable
            retval = (uint32_t)net_llm_service_set_enabled((int)arg1);
            break;
        case 42: // LLM Network Service Status String
            if (uptr_ok(arg1, 256)) {
                strncpy((char *)arg1, net_llm_service_status_string(), 255);
                ((char *)arg1)[255] = '\0';
            }
            break;
        case 43: // Network Poll
            net_poll();
            break;
        case 44: // LLM Network Service Port
            retval = (uint32_t)net_llm_service_set_port((uint16_t)arg1);
            break;
        case 45: // ICMP ping — returns RTT ms, 0=timeout, <0=error
            if (ustr_ok(arg1)) {
                retval = (uint32_t)net_ping((const char *)arg1);
            }
            break;
        case 46: // DNS resolve — arg1=hostname, arg2=uint8_t[4] out_ip; returns 1 on success
            if (ustr_ok(arg1) && uptr_ok(arg2, 4)) {
                retval = (uint32_t)net_dns_resolve((const char *)arg1, (uint8_t *)arg2);
            }
            break;
        case 47: // LLM service set token — arg1=token string ("off" clears)
            if (ustr_ok(arg1)) retval = (uint32_t)net_llm_service_set_token((const char *)arg1);
            break;
        case 48: // RSH set enabled — arg1=0 or 1
            retval = (uint32_t)net_rsh_set_enabled((int)arg1);
            break;
        case 49: // RSH set port — arg1=port
            retval = (uint32_t)net_rsh_set_port((uint16_t)arg1);
            break;
        case 50: // RSH set token — arg1=token string ("off" clears)
            if (ustr_ok(arg1)) retval = (uint32_t)net_rsh_set_token((const char *)arg1);
            break;
        case 51: // RSH status string — arg1=buf (192 bytes)
            if (uptr_ok(arg1, 192)) {
                strncpy((char *)arg1, net_rsh_status_string(), 191);
                ((char *)arg1)[191] = '\0';
            }
            break;
        case 52: // GGUF selftest — 1=pass, -1=fail, 0=skip
            retval = (uint32_t)llm_gguf_selftest();
            break;
        case 53: // Net Config Save — persist current net_config to /net.cfg
            retval = (uint32_t)net_save_config();
            break;
        case 54: // IPC SHM Create — arg1=key, arg2=size, arg3=out_id ptr
            if (uptr_ok(arg3, sizeof(uint32_t)))
                retval = (uint32_t)ipc_shm_create(arg1, arg2, (uint32_t *)arg3);
            break;
        case 55: // IPC SHM Attach — arg1=id, arg2=out_addr ptr, arg3=out_size ptr
            if (uptr_ok(arg2, sizeof(uint32_t)) && (!arg3 || uptr_ok(arg3, sizeof(uint32_t))))
                retval = (uint32_t)ipc_shm_attach(arg1, (uint32_t *)arg2, arg3 ? (uint32_t *)arg3 : 0);
            break;
        case 56: // IPC SHM Detach — arg1=id
            retval = (uint32_t)ipc_shm_detach(arg1);
            break;
        case 57: // IPC SHM Destroy — arg1=id
            retval = (uint32_t)ipc_shm_destroy(arg1);
            break;
        case 58: // IPC SHM Selftest — 1=pass, 0=fail
            retval = (uint32_t)ipc_shm_selftest();
            break;
        case 59: // LLM Dump Logits — arg1=prompt str, arg2=response buf (128 bytes)
            if (ustr_ok(arg1) && uptr_ok(arg2, 128))
                llm_dump_logits((char *)arg1, (char *)arg2);
            break;
        case 60: // High-memory buffer (hbuf) selftest — 1=pass, -1=fail, 0=skip
            {
                extern int hbuf_selftest(void);
                retval = (uint32_t)hbuf_selftest();
            }
            break;
        case 61: // USB (UHCI) Status String
            {
                extern const char *uhci_status_string(void);
                if (uptr_ok(arg1, 256)) {
                    strncpy((char *)arg1, uhci_status_string(), 255);
                    ((char *)arg1)[255] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return retval;
}
