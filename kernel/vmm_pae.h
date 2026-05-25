#ifndef VMM_PAE_H
#define VMM_PAE_H

#include <stdint.h>
#include "pmm.h"

#define VMM_PAE_PDPT_ENTRIES 4
#define VMM_PAE_PD_ENTRIES   512
#define VMM_PAE_PT_ENTRIES   512

#define VMM_PAE_PRESENT      0x001ULL
#define VMM_PAE_WRITABLE     0x002ULL
#define VMM_PAE_USER         0x004ULL
#define VMM_PAE_WRITE_THROUGH 0x008ULL
#define VMM_PAE_CACHE_DISABLE 0x010ULL
#define VMM_PAE_ACCESSED     0x020ULL
#define VMM_PAE_DIRTY        0x040ULL
#define VMM_PAE_LARGE_PAGE   0x080ULL
#define VMM_PAE_GLOBAL       0x100ULL

#define VMM_PAE_ADDR_MASK    0x000FFFFFFFFFF000ULL

typedef uint64_t pae_entry_t;

typedef struct {
    pae_entry_t entries[VMM_PAE_PDPT_ENTRIES];
} __attribute__((aligned(32))) pae_pdpt_t;

typedef struct {
    pae_entry_t entries[VMM_PAE_PD_ENTRIES];
} __attribute__((aligned(4096))) pae_page_directory_t;

typedef struct {
    pae_entry_t entries[VMM_PAE_PT_ENTRIES];
} __attribute__((aligned(4096))) pae_page_table_t;

pae_entry_t vmm_pae_make_entry(phys_addr_t physical_addr, uint64_t flags);
phys_addr_t vmm_pae_entry_addr(pae_entry_t entry);
uint32_t vmm_pae_pdpt_index(uint32_t virtual_addr);
uint32_t vmm_pae_pd_index(uint32_t virtual_addr);
uint32_t vmm_pae_pt_index(uint32_t virtual_addr);
uint32_t vmm_pae_page_offset(uint32_t virtual_addr);
int vmm_pae_entry_present(pae_entry_t entry);
int vmm_pae_selftest(void);

#endif
