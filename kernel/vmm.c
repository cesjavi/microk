#include "vmm.h"
#include "pmm.h"
#include <string.h>

uint32_t *page_directory;

static uint32_t align_down_page(uint32_t value) {
    return value & ~(uint32_t)(4096 - 1);
}

static uint32_t align_up_page(uint32_t value) {
    return (value + 4096 - 1) & ~(uint32_t)(4096 - 1);
}

void vmm_map_page_ext(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x03FF;

    if (!(page_directory[pd_index] & 0x01)) {
        uint32_t *new_table = (uint32_t *)pmm_alloc_block();
        if (!new_table) return;
        memset(new_table, 0, 4096);
        page_directory[pd_index] = (uint32_t)new_table | 0x07; // Present, RW, User
    }

    uint32_t *table = (uint32_t *)(page_directory[pd_index] & 0xFFFFF000);
    table[pt_index] = physical_addr | flags;
}

void vmm_map_page(uint32_t virtual_addr, uint32_t physical_addr) {
    vmm_map_page_ext(virtual_addr, physical_addr, 0x03); // Present, RW, Supervisor
}

void vmm_init(uint32_t mem_size, multiboot_info_t *mbi) {
    (void)mbi;
    page_directory = (uint32_t *)pmm_alloc_block();
    if (!page_directory) return;
    memset(page_directory, 0, 4096);

    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0 | 2; // Supervisor, Read/Write, Not Present
    }

    if (mem_size < 0x1000000) {
        mem_size = 0x1000000;
    }

    extern uint32_t user_start;
    extern uint32_t user_end;
    uint32_t u_start = align_down_page((uint32_t)&user_start);
    uint32_t u_end = align_up_page((uint32_t)&user_end);

    // Identity map memory with protection
    for (uint32_t i = 0; i < mem_size; i += 4096) {
        if (i >= u_start && i < u_end) {
            // User section (Shell)
            vmm_map_page_ext(i, i, 0x07); // Present, RW, User
        } else if (i < (uint32_t)pmm_get_bitmap_size() + 0x1000000) { 
            // Kernel region (approx first 16MB or up to bitmap)
            // For now, let's be conservative and map the kernel as Supervisor
            vmm_map_page_ext(i, i, 0x03); // Present, RW, Supervisor
        } else {
            // Rest of memory as User-accessible (heap, modules, etc)
            vmm_map_page_ext(i, i, 0x07);
        }
    }

    // Identity map framebuffer if available
    if (mbi && (mbi->flags & 0x800)) {
        uint32_t fb_phys = (uint32_t)mbi->framebuffer_addr;
        uint32_t fb_size = mbi->framebuffer_height * mbi->framebuffer_pitch;
        if (fb_phys != 0 && fb_size > 0 && fb_size < 128 * 1024 * 1024) {
            for (uint32_t offset = 0; offset < fb_size; offset += 4096) {
                vmm_map_page_ext(fb_phys + offset, fb_phys + offset, 0x07); // Present, RW, User
            }
        }
    }

    extern void load_page_directory(uint32_t *);
    extern void enable_paging();
    load_page_directory(page_directory);
    enable_paging();
}

void *vmm_temp_map_high(phys_addr_t phys) {
    // Under PAE, this maps the 64-bit physical address.
    // In our 32-bit fallback VMM, it maps the low 32-bits to ensure stability.
    vmm_map_page_ext(HIGH_MEM_WINDOW_VMEM, (uint32_t)phys, 0x03); // Present + RW
    asm volatile("invlpg (%0)" :: "r"(HIGH_MEM_WINDOW_VMEM) : "memory");
    return (void *)HIGH_MEM_WINDOW_VMEM;
}

void vmm_temp_unmap_high(void) {
    vmm_map_page_ext(HIGH_MEM_WINDOW_VMEM, 0, 0x02); // Not present + RW
    asm volatile("invlpg (%0)" :: "r"(HIGH_MEM_WINDOW_VMEM) : "memory");
}
