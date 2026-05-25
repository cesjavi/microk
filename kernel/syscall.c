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
#include <string.h>

extern struct kernel_metrics metrics;

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
                if (arg1) kprint((char *)arg1);
            }
            break;
        case 2: // Yield
            task_switch();
            break;
        case 3: // Get Metrics
            if (arg1) *((struct kernel_metrics *)arg1) = metrics;
            break;
        case 4: // Get Char
            if (arg1) *((char *)arg1) = keyboard_get_char();
            break;
        case 5: // LLM Query
            if (arg1 && arg2) llm_inference((char *)arg1, (char *)arg2);
            break;
        case 6: // Beep
            speaker_beep(arg1, arg2);
            break;
        case 7: // Read File (arg1=name, arg2=buffer)
            {
                if (!arg1 || !arg2) break;
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
                if (arg1) fat32_ls_path((char *)arg1);
                else fat32_ls();
            }
            break;
        case 14: // FAT32 CAT
            {
                extern void fat32_cat(const char *filename);
                if (arg1) fat32_cat((char *)arg1);
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
                if (arg1) extfs_cat((char *)arg1);
            }
            break;
        case 17: // LLM Load Model
            {
                extern void llm_load_model_module(uint32_t start, uint32_t end, const char *name);
                extern int llm_model_supported();
                // arg1 = addr, arg2 = size, arg3 = name_ptr
                if (arg1 && arg2) {
                    llm_load_model_module(arg1, arg1 + arg2, (char *)arg3);
                    retval = llm_model_supported();
                }
            }
            break;
        case 18: // Get LLM Status String
            {
                extern const char* llm_status_string();
                if (arg1) {
                    strncpy((char *)arg1, llm_status_string(), 255);
                    ((char *)arg1)[255] = '\0';
                }
            }
            break;
        case 19: // Get LLM Info String
            {
                extern const char* llm_info_string();
                if (arg1) {
                    strncpy((char *)arg1, llm_info_string(), 255);
                    ((char *)arg1)[255] = '\0';
                }
            }
            break;
        case 21: // FAT32 Create File
            {
                extern int fat32_create_file(const char *filename);
                if (arg1 && arg2) *((int *)arg2) = fat32_create_file((char *)arg1);
            }
            break;
        case 22: // FAT32 Write File
            {
                extern int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t size);
                if (arg1 && arg2) fat32_write_file((char *)arg1, (uint8_t *)arg2, arg3);
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
            if (arg1) {
                strncpy((char *)arg1, net_status_string(), 255);
                ((char *)arg1)[255] = '\0';
            }
            break;
        case 26: // Net Config DHCP
            if (arg1) *((int *)arg1) = net_config_dhcp();
            break;
        case 27: // Net Config Static
            if (arg1 && arg2) {
                char **args = (char **)arg1;
                *((int *)arg2) = net_config_static(args[0], args[1], args[2]);
            }
            break;
        case 28: // Memory Region Count
            if (arg1) *((uint32_t *)arg1) = pmm_memory_region_count();
            break;
        case 29: // Memory Region Get
            if (arg2 && arg3) *((int *)arg3) = pmm_get_memory_region(arg1, (pmm_memory_region_t *)arg2);
            break;
        case 30: // GPU Status String
            if (arg1) {
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
                retval = fat32_get_entry_by_index((char *)arg1, arg2, (char *)arg3, (int *)0);
            }
            break;
        case 34: // PMM Allocate Contiguous Region
            if (arg1) retval = (uint32_t)pmm_alloc_region(arg1);
            break;
        case 35: // LLM Load Model File
            {
                extern int llm_load_model_file(const char *name);
                if (arg1) retval = llm_load_model_file((char *)arg1);
            }
            break;
        case 36: // FAT32 Is Directory
            {
                extern int fat32_path_is_dir(const char *path);
                retval = fat32_path_is_dir((char *)arg1);
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
                if (arg1) {
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
            if (arg1) {
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
        default:
            break;
    }
    return retval;
}
