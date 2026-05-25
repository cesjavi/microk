#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#include "multiboot.h"

void vmm_init(uint32_t mem_size, multiboot_info_t *mbi);
void vmm_map_page(uint32_t virtual_addr, uint32_t physical_addr);
void vmm_map_page_ext(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);

#define HIGH_MEM_WINDOW_VMEM 0xFF000000

#include "pmm.h"
void *vmm_temp_map_high(phys_addr_t phys);
void vmm_temp_unmap_high(void);

extern uint32_t *page_directory;

#endif
