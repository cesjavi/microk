#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define PMM_MAX_MEMORY_REGIONS 32

typedef uint64_t phys_addr_t;

#define PMM_ALLOC_LOW     0x01
#define PMM_ALLOC_HIGH_OK 0x02
#define PMM_ALLOC_DMA32   0x04

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t type;
} pmm_memory_region_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t page_size;
    uint32_t bitmap_size;
    uint32_t bitmap_start;
    uint32_t low_reserved_bytes;
    uint32_t module_count;
    uint32_t module_reserved_bytes;
    uint32_t addressable_bytes;
    uint32_t mmap_usable_bytes;
    uint32_t mmap_reserved_bytes;
    uint32_t mmap_region_count;
    uint32_t mmap_invalid_regions;
    uint32_t mmap_overlap_count;
    uint32_t mmap_high_region_count;
    uint32_t mmap_high_usable_kib;
    uint32_t mmap_high_reserved_kib;
    uint32_t low_pool_total_blocks;
    uint32_t low_pool_used_blocks;
    uint32_t low_pool_free_blocks;
    uint32_t high_pool_total_blocks;
    uint32_t high_pool_used_blocks;
    uint32_t high_pool_free_blocks;
    uint32_t high_pool_allocatable;
} pmm_stats_t;

void pmm_init(uint32_t mem_size, uint32_t bitmap_start);
void pmm_record_boot_layout(uint32_t bitmap_start, uint32_t low_reserved_bytes, uint32_t module_count, uint32_t module_reserved_bytes);
void pmm_reset_memory_regions(void);
void pmm_record_memory_region(uint32_t base, uint32_t size, uint32_t type);
void pmm_record_high_memory_region(uint32_t base_low, uint32_t base_high, uint32_t length_low, uint32_t length_high, uint32_t type);
uint32_t pmm_memory_region_count(void);
int pmm_get_memory_region(uint32_t index, pmm_memory_region_t *out);
void pmm_mark_usable_region(uint32_t base, uint32_t size);
void *pmm_alloc_block();
void *pmm_alloc_region(uint32_t size);
void pmm_free_block(void *ptr);
void pmm_reserve_region(uint32_t base, uint32_t size);
void pmm_free_region(uint32_t base, uint32_t size);
uint32_t pmm_get_bitmap_size();
uint32_t pmm_largest_free_region_bytes(void);
void pmm_get_stats(pmm_stats_t *out);

/* High Memory Pool Allocator (PAE) */
void pmm_init_high(void);
phys_addr_t pmm_alloc_high_block(void);
void pmm_free_high_block(phys_addr_t phys);

#endif
