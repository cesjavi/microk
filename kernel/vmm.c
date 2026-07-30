#include "vmm.h"
#include "pmm.h"
#include "vmm_pae.h"
#include "video.h"
#include <string.h>

uint32_t *page_directory;
static pae_pdpt_t *pae_pdpt;
static int pae_active;

static uint32_t align_down_page(uint32_t value) {
    return value & ~(uint32_t)(4096 - 1);
}

static uint32_t align_up_page(uint32_t value) {
    return (value + 4096 - 1) & ~(uint32_t)(4096 - 1);
}

static int cpu_has_pae(void) {
    uint32_t flags;
    asm volatile(
        "pushfl\n"
        "pushfl\n"
        "xorl $0x200000, (%%esp)\n"
        "popfl\n"
        "pushfl\n"
        "popl %%eax\n"
        "xorl (%%esp), %%eax\n"
        "popfl\n"
        "andl $0x200000, %%eax\n"
        : "=a"(flags)
        :
        : "cc");
    if (!flags) return 0;

    uint32_t edx;
    asm volatile(
        "movl $1, %%eax\n"
        "cpuid\n"
        : "=d"(edx)
        :
        : "eax", "ebx", "ecx");
    return (edx & (1U << 6)) != 0;
}

static void vmm_map_page_legacy(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x03FF;

    if (!(page_directory[pd_index] & 0x01)) {
        uint32_t *new_table = (uint32_t *)pmm_alloc_block();
        if (!new_table) return;
        // Safe: all physical RAM is identity-mapped before enable_paging() is called
        // in vmm_init(), so physical address == virtual address here.
        memset(new_table, 0, 4096);
        page_directory[pd_index] = (uint32_t)(uintptr_t)new_table | 0x07; // Present, RW, User
        // Flush the TLB entry for the new page directory entry
        asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }

    uint32_t *table = (uint32_t *)(uintptr_t)(page_directory[pd_index] & 0xFFFFF000);
    table[pt_index] = physical_addr | flags;
    asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

static pae_page_table_t *vmm_pae_get_table(uint32_t virtual_addr, int create) {
    uint32_t pdpt_index = vmm_pae_pdpt_index(virtual_addr);
    uint32_t pd_index = vmm_pae_pd_index(virtual_addr);
    pae_page_directory_t *pd;
    pae_page_table_t *pt;

    if (!pae_pdpt) return 0;

    if (!(pae_pdpt->entries[pdpt_index] & VMM_PAE_PRESENT)) {
        if (!create) return 0;
        pd = (pae_page_directory_t *)pmm_alloc_block();
        if (!pd) return 0;
        memset(pd, 0, sizeof(*pd));
        pae_pdpt->entries[pdpt_index] = vmm_pae_make_entry((phys_addr_t)(uintptr_t)pd, VMM_PAE_PRESENT);
    }

    pd = (pae_page_directory_t *)(uintptr_t)vmm_pae_entry_addr(pae_pdpt->entries[pdpt_index]);
    if (!(pd->entries[pd_index] & VMM_PAE_PRESENT)) {
        if (!create) return 0;
        pt = (pae_page_table_t *)pmm_alloc_block();
        if (!pt) return 0;
        memset(pt, 0, sizeof(*pt));
        pd->entries[pd_index] = vmm_pae_make_entry((phys_addr_t)(uintptr_t)pt, VMM_PAE_PRESENT | VMM_PAE_WRITABLE | VMM_PAE_USER);
    }

    return (pae_page_table_t *)(uintptr_t)vmm_pae_entry_addr(pd->entries[pd_index]);
}

static void vmm_map_page_pae(uint32_t virtual_addr, phys_addr_t physical_addr, uint32_t flags) {
    pae_page_table_t *pt = vmm_pae_get_table(virtual_addr, 1);
    if (!pt) return;

    pt->entries[vmm_pae_pt_index(virtual_addr)] = vmm_pae_make_entry(physical_addr, (uint64_t)(flags & 0xFFF));
    asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

void vmm_map_page_ext(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    if (pae_active) {
        vmm_map_page_pae(virtual_addr, (phys_addr_t)physical_addr, flags);
    } else {
        vmm_map_page_legacy(virtual_addr, physical_addr, flags);
    }
}

void vmm_map_page(uint32_t virtual_addr, uint32_t physical_addr) {
    vmm_map_page_ext(virtual_addr, physical_addr, 0x03); // Present, RW, Supervisor
}

static int vmm_protect_range_legacy(uint32_t virtual_addr, uint32_t size, uint32_t flags) {
    if (!virtual_addr || !size) return 0;
    if (virtual_addr + size < virtual_addr) return 0;

    uint32_t page = align_down_page(virtual_addr);
    uint32_t end_page = align_down_page(virtual_addr + size - 1);

    while (1) {
        uint32_t pd_index = page >> 22;
        uint32_t pt_index = (page >> 12) & 0x03FF;
        uint32_t pde = page_directory[pd_index];
        if (!(pde & 0x01)) return 0;

        uint32_t *table = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000);
        uint32_t pte = table[pt_index];
        if (!(pte & 0x01)) return 0;

        table[pt_index] = (pte & 0xFFFFF000) | (flags & 0xFFF);
        asm volatile("invlpg (%0)" :: "r"(page) : "memory");

        if (page == end_page) break;
        page += 0x1000;
    }

    return 1;
}

static int vmm_protect_range_pae(uint32_t virtual_addr, uint32_t size, uint32_t flags) {
    if (!virtual_addr || !size) return 0;
    if (virtual_addr + size < virtual_addr) return 0;

    uint32_t page = align_down_page(virtual_addr);
    uint32_t end_page = align_down_page(virtual_addr + size - 1);

    while (1) {
        pae_page_table_t *pt = vmm_pae_get_table(page, 0);
        if (!pt) return 0;
        pae_entry_t pte = pt->entries[vmm_pae_pt_index(page)];
        if (!(pte & VMM_PAE_PRESENT)) return 0;

        pt->entries[vmm_pae_pt_index(page)] = vmm_pae_make_entry(vmm_pae_entry_addr(pte), (uint64_t)(flags & 0xFFF));
        asm volatile("invlpg (%0)" :: "r"(page) : "memory");

        if (page == end_page) break;
        page += 0x1000;
    }

    return 1;
}

int vmm_protect_range(uint32_t virtual_addr, uint32_t size, uint32_t flags) {
    return pae_active ? vmm_protect_range_pae(virtual_addr, size, flags)
                      : vmm_protect_range_legacy(virtual_addr, size, flags);
}

void vmm_init(uint32_t mem_size, multiboot_info_t *mbi) {
    if (cpu_has_pae()) {
        pae_pdpt = (pae_pdpt_t *)pmm_alloc_block();
        if (pae_pdpt) {
            memset(pae_pdpt, 0, 4096);
            page_directory = (uint32_t *)pae_pdpt;
            pae_active = 1;
        }
    }

    if (!pae_active) {
        page_directory = (uint32_t *)pmm_alloc_block();
        if (!page_directory) return;
        memset(page_directory, 0, 4096);

        for (int i = 0; i < 1024; i++) {
            page_directory[i] = 0 | 2; // Supervisor, Read/Write, Not Present
        }
    }

    if (mem_size < 0x1000000) {
        mem_size = 0x1000000;
    }

    extern uint32_t user_start;
    extern uint32_t user_end;
    uint32_t u_start = align_down_page((uint32_t)(uintptr_t)&user_start);
    uint32_t u_end = align_up_page((uint32_t)(uintptr_t)&user_end);

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

    if (pae_active) {
        extern void enable_pae_paging(uint32_t pdpt_phys);
        enable_pae_paging((uint32_t)pae_pdpt);
        klog("VMM: PAE paging enabled.\n");
    } else {
        extern void load_page_directory(uint32_t *);
        extern void enable_paging();
        load_page_directory(page_directory);
        enable_paging();
        klog("VMM: legacy 32-bit paging enabled.\n");
    }
}

static int vmm_user_ptr_ok_legacy(uint32_t addr, uint32_t len) {
    if (!addr) return 0;
    if (len == 0) len = 1;
    if (addr + len < addr) return 0; /* overflow */

    uint32_t start_page = addr & ~0xFFFU;
    uint32_t end_page = (addr + len - 1) & ~0xFFFU;
    uint32_t page_count = (end_page - start_page) / 0x1000 + 1;
    uint32_t page = start_page;
    for (uint32_t i = 0; i < page_count; i++, page += 0x1000) {
        uint32_t pd_index = page >> 22;
        uint32_t pt_index = (page >> 12) & 0x3FF;

        uint32_t pde = page_directory[pd_index];
        if (!(pde & 0x01)) return 0;               /* PDE not present */
        if (!(pde & 0x04)) return 0;               /* PDE supervisor-only */

        uint32_t *table = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000);
        uint32_t pte = table[pt_index];
        if (!(pte & 0x01)) return 0;               /* PTE not present */
        if (!(pte & 0x04)) return 0;               /* PTE supervisor-only */
    }
    return 1;
}

static int vmm_user_ptr_ok_pae(uint32_t addr, uint32_t len) {
    if (!addr) return 0;
    if (len == 0) len = 1;
    if (addr + len < addr) return 0;

    uint32_t start_page = addr & ~0xFFFU;
    uint32_t end_page = (addr + len - 1) & ~0xFFFU;
    uint32_t page_count = (end_page - start_page) / 0x1000 + 1;
    uint32_t page = start_page;
    for (uint32_t i = 0; i < page_count; i++, page += 0x1000) {
        pae_page_table_t *pt = vmm_pae_get_table(page, 0);
        if (!pt) return 0;

        pae_entry_t pte = pt->entries[vmm_pae_pt_index(page)];
        if (!(pte & VMM_PAE_PRESENT)) return 0;
        if (!(pte & VMM_PAE_USER)) return 0;
    }
    return 1;
}

int vmm_user_ptr_ok(uint32_t addr, uint32_t len) {
    return pae_active ? vmm_user_ptr_ok_pae(addr, len)
                      : vmm_user_ptr_ok_legacy(addr, len);
}

void *vmm_temp_map_high_slot(int slot, phys_addr_t phys) {
    if (slot < 0 || slot >= HIGH_MEM_WINDOW_SLOTS) return 0;
    uint32_t va = HIGH_MEM_WINDOW_VMEM + (uint32_t)slot * PAGE_SIZE;

    if (pae_active) {
        vmm_map_page_pae(va, phys, 0x03);
    } else {
        vmm_map_page_ext(va, (uint32_t)phys, 0x03);
    }
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    return (void *)va;
}

void vmm_temp_unmap_high_slot(int slot) {
    if (slot < 0 || slot >= HIGH_MEM_WINDOW_SLOTS) return;
    uint32_t va = HIGH_MEM_WINDOW_VMEM + (uint32_t)slot * PAGE_SIZE;

    if (pae_active) {
        pae_page_table_t *pt = vmm_pae_get_table(va, 0);
        if (pt) {
            pt->entries[vmm_pae_pt_index(va)] = 0;
        }
    } else {
        vmm_map_page_ext(va, 0, 0x02); // Not present + RW
    }
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void *vmm_temp_map_high(phys_addr_t phys) {
    return vmm_temp_map_high_slot(0, phys);
}

void vmm_temp_unmap_high(void) {
    vmm_temp_unmap_high_slot(0);
}

int vmm_pae_active(void) {
    return pae_active;
}
