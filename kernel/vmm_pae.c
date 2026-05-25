#include "vmm_pae.h"

pae_entry_t vmm_pae_make_entry(phys_addr_t physical_addr, uint64_t flags) {
    return (physical_addr & VMM_PAE_ADDR_MASK) | flags;
}

phys_addr_t vmm_pae_entry_addr(pae_entry_t entry) {
    return entry & VMM_PAE_ADDR_MASK;
}

uint32_t vmm_pae_pdpt_index(uint32_t virtual_addr) {
    return (virtual_addr >> 30) & 0x3;
}

uint32_t vmm_pae_pd_index(uint32_t virtual_addr) {
    return (virtual_addr >> 21) & 0x1FF;
}

uint32_t vmm_pae_pt_index(uint32_t virtual_addr) {
    return (virtual_addr >> 12) & 0x1FF;
}

uint32_t vmm_pae_page_offset(uint32_t virtual_addr) {
    return virtual_addr & 0xFFF;
}

int vmm_pae_entry_present(pae_entry_t entry) {
    return (entry & VMM_PAE_PRESENT) != 0;
}

int vmm_pae_selftest(void) {
    pae_entry_t entry = vmm_pae_make_entry(0x12345000ULL, VMM_PAE_PRESENT | VMM_PAE_WRITABLE);

    if (!vmm_pae_entry_present(entry)) {
        return 0;
    }
    if (vmm_pae_entry_addr(entry) != 0x12345000ULL) {
        return 0;
    }
    if (vmm_pae_pdpt_index(0xC0000000) != 3) {
        return 0;
    }
    if (vmm_pae_pd_index(0xC0201000) != 1) {
        return 0;
    }
    if (vmm_pae_pt_index(0xC0201000) != 1) {
        return 0;
    }
    if (vmm_pae_page_offset(0xC0201ABC) != 0xABC) {
        return 0;
    }

    return 1;
}
