#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#include "multiboot.h"

void vmm_init(uint32_t mem_size, multiboot_info_t *mbi);
void vmm_map_page(uint32_t virtual_addr, uint32_t physical_addr);
void vmm_map_page_ext(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);
int vmm_protect_range(uint32_t virtual_addr, uint32_t size, uint32_t flags);
int vmm_pae_active(void);

/* Returns 1 if every page in [addr, addr+len) is present and user-accessible (U/S=1).
 * Returns 0 for NULL, overflow, unmapped pages, or supervisor-only pages. */
int vmm_user_ptr_ok(uint32_t addr, uint32_t len);

#define HIGH_MEM_WINDOW_VMEM 0xFF000000
#define HIGH_MEM_WINDOW_SLOTS 4

#include "pmm.h"
/* slot 0 window, kept for the standalone PAE verification test (syscall 40) */
void *vmm_temp_map_high(phys_addr_t phys);
void vmm_temp_unmap_high(void);

/* Same as above but with an explicit window slot in [0, HIGH_MEM_WINDOW_SLOTS),
 * so more than one high page can stay mapped at once (e.g. hbuf_t copies use
 * slot 1, separate from the syscall 40 test's slot 0). */
void *vmm_temp_map_high_slot(int slot, phys_addr_t phys);
void vmm_temp_unmap_high_slot(int slot);

extern uint32_t *page_directory;

#endif
