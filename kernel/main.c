#include "ai_hooks.h"
#include "boot_modules.h"
#include "extfs.h"
#include "fat32.h"
#include "gdt.h"
#include "gpu.h"
#include "idt.h"
#include "ipc.h"
#include "keyboard.h"
#include "kheap.h"
#include "multiboot.h"
#include "net.h"
#include "ntfs.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "shell.h"
#include "storage.h"
#include "task.h"
#include "timer.h"
#include "video.h"
#include "vmm.h"

#define ALIGN_UP(value, align) (((value) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(value, align) ((value) & ~((align) - 1))

struct kernel_metrics metrics = {0};

__attribute__((target("no-sse")))
static void enable_sse(void) {
    uint32_t cr0, cr4;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // clear EM (emulation)
    cr0 |= (1 << 1);  // set MP (monitor co-processor)
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));

    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // set OSFXSR (FXSAVE/FXRSTOR support)
    cr4 |= (1 << 10); // set OSXMMEXCPT (SIMD exception support)
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
    klog("SSE2/FPU Hardware Acceleration Enabled.\n");
}

static uint32_t detect_memory_size(multiboot_info_t *mbi) {
    uint32_t max_end = 0;

    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        uint32_t offset = 0;
        while (offset < mbi->mmap_length) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)(mbi->mmap_addr + offset);
            uint32_t base = entry->base_addr_low;
            uint32_t length = entry->length_low;
            uint32_t end;

            if (entry->base_addr_high != 0) {
                offset += entry->size + sizeof(entry->size);
                continue;
            }
            if (entry->length_high != 0) {
                length = 0xFFFFFFFF - base;
            }
            end = base + length;
            if (end < base) {
                end = 0xFFFFFFFF;
            }
            if (end > max_end) {
                max_end = end;
            }

            offset += entry->size + sizeof(entry->size);
        }
    }

    if (max_end > 0) {
        if (max_end >= 0xFFFFF000) {
            return 0xFFFFF000;
        }
        return ALIGN_UP(max_end, PAGE_SIZE);
    }

    if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        return (mbi->mem_upper + 1024) * 1024;
    }

    return 16 * 1024 * 1024;
}

static void free_usable_memory(multiboot_info_t *mbi, uint32_t mem_size) {
    pmm_reset_memory_regions();

    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        uint32_t offset = 0;
        while (offset < mbi->mmap_length) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)(mbi->mmap_addr + offset);
            pmm_record_high_memory_region(
                entry->base_addr_low,
                entry->base_addr_high,
                entry->length_low,
                entry->length_high,
                entry->type
            );
            if (entry->base_addr_high == 0) {
                uint32_t base = entry->base_addr_low;
                uint32_t length = entry->length_low;

                if (entry->length_high != 0) {
                    length = 0xFFFFFFFF - base;
                }
                if (base < mem_size && length > 0) {
                    if (base + length < base || base + length > mem_size) {
                        length = mem_size - base;
                    }
                    pmm_record_memory_region(base, length, entry->type);
                    if (entry->type == 1) {
                        pmm_mark_usable_region(base, length);
                    }
                }
            }
            offset += entry->size + sizeof(entry->size);
        }
        return;
    }

    if (mem_size > 0x100000) {
        pmm_record_memory_region(0, 0x100000, 2);
        pmm_record_memory_region(0x100000, mem_size - 0x100000, 1);
        pmm_mark_usable_region(0x100000, mem_size - 0x100000);
    }
}

static uint32_t reserve_kernel_and_modules(multiboot_info_t *mbi) {
    extern uint32_t kernel_end;
    uint32_t reserved_end = (uint32_t)&kernel_end;

    if (mbi->flags & MULTIBOOT_FLAG_MODS) {
        multiboot_module_t *mods = (multiboot_module_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            if (mods[i].mod_end > reserved_end) {
                reserved_end = mods[i].mod_end;
            }
        }
    }

    return ALIGN_UP(reserved_end, PAGE_SIZE);
}

static uint32_t align_down(uint32_t value) {
    return value & ~(PAGE_SIZE - 1);
}

static uint32_t multiboot_module_reserved_bytes(multiboot_info_t *mbi) {
    uint32_t bytes = 0;

    if (mbi->flags & MULTIBOOT_FLAG_MODS) {
        multiboot_module_t *mods = (multiboot_module_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            uint32_t start = align_down(mods[i].mod_start);
            uint32_t end = ALIGN_UP(mods[i].mod_end, PAGE_SIZE);
            bytes += end - start;
        }
    }

    return bytes;
}

static void reserve_multiboot_modules(multiboot_info_t *mbi) {
    if (mbi->flags & MULTIBOOT_FLAG_MODS) {
        multiboot_module_t *mods = (multiboot_module_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            pmm_reserve_region(mods[i].mod_start, mods[i].mod_end - mods[i].mod_start);
        }
    }
}

static void register_filesystems(void) {
    vfs_init();
    extfs_register();
    ntfs_register();
    fat32_register();
}

static void load_net_config_from_storage(void) {
    uint8_t config_buf[512];
    uint32_t config_size = 0;

    if (fat32_load_file("microk/net.cfg", config_buf, sizeof(config_buf) - 1, &config_size) ||
        fat32_load_file("net.cfg", config_buf, sizeof(config_buf) - 1, &config_size)) {
        config_buf[config_size] = '\0';
        if (net_load_config_text((const char *)config_buf, config_size)) {
            klog("Network config loaded from FAT32.");
        } else {
            klog("Network config found but not applied.");
        }
    }
}

void kernel_main(uint32_t magic, uint32_t mb_info) {
    (void)magic;
    // Enable SSE as early as possible because the compiler might use SSE instructions
    // in subsequent functions (optimized with -O3 -msse2)
    enable_sse();

    serial_init();
    multiboot_info_t *mbi = (multiboot_info_t *)mb_info;

    video_init(mbi);
    video_clear(0x00000000);

    klog("MicroK AI-Native OS Loading...");
    klog("Initializing GDT...");
    gdt_init();

    klog("Initializing Physical Memory Manager...");
    uint32_t mem_size = detect_memory_size(mbi);
    uint32_t bitmap_addr = reserve_kernel_and_modules(mbi);
    pmm_init(mem_size, bitmap_addr);
    free_usable_memory(mbi, mem_size);
    pmm_reserve_region(0, bitmap_addr + pmm_get_bitmap_size());
    reserve_multiboot_modules(mbi);
    pmm_init_high();
    pmm_record_boot_layout(
        bitmap_addr,
        bitmap_addr + pmm_get_bitmap_size(),
        (mbi->flags & MULTIBOOT_FLAG_MODS) ? mbi->mods_count : 0,
        multiboot_module_reserved_bytes(mbi)
    );

    klog("Initializing Virtual Memory Manager...");
    vmm_init(mem_size, mbi);

    klog("Initializing IDT & PIC...");
    idt_init();

    klog("Initializing System Timer (100Hz)...");
    timer_init(100);

    klog("Initializing Keyboard...");
    keyboard_init();

    klog("Initializing PCI Bus...");
    pci_init();

    klog("Initializing GPU Detection...");
    gpu_init();

    klog("Initializing IPC System...");
    ipc_init();

    klog("Initializing Task Scheduler...");
    task_init();

    klog("Initializing Kernel Heap...");
    kheap_init();

    klog("Initializing VFS Filesystem Drivers...");
    register_filesystems();

    klog("Initializing Storage Devices...");
    storage_init();

    klog("Initializing Network Config...");
    net_init();
    net_detect_pci();
    load_net_config_from_storage();

    boot_modules_load(mbi);
    
    // Set kernel stack for TSS (so we can return to kernel from Ring 3)
    extern uint32_t stack_top;
    tss_set_stack((uint32_t)&stack_top);

    klog("System Ready. Welcome to MicroK AI-Native OS.");
    klog("Starting Isolated Shell (Ring 3)...");
    
    extern void jump_usermode(uint32_t eip, uint32_t esp);
    
    // Allocate a temporary stack for the user mode shell
    uint8_t *user_stack = kmalloc(4096);
    if (!user_stack) {
        klog("Failed to allocate user shell stack.");
        while (1) {
        }
    }

    uint32_t stack_start = ALIGN_DOWN((uint32_t)user_stack, PAGE_SIZE);
    uint32_t stack_end = ALIGN_UP((uint32_t)user_stack + 4096, PAGE_SIZE);
    for (uint32_t page = stack_start; page < stack_end; page += PAGE_SIZE) {
        vmm_map_page_ext(page, page, 0x07);
    }

    uint32_t user_esp = (uint32_t)user_stack + 4096;
    
    jump_usermode((uint32_t)shell_task, user_esp);
}
