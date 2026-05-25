#include "pmm.h"
#include <string.h>

static uint32_t *bitmap;
static uint32_t max_blocks;
static uint32_t used_blocks;
static uint32_t boot_bitmap_start;
static uint32_t boot_low_reserved_bytes;
static uint32_t boot_module_count;
static uint32_t boot_module_reserved_bytes;
static pmm_memory_region_t memory_regions[PMM_MAX_MEMORY_REGIONS];
static uint32_t memory_region_count;
static uint32_t invalid_memory_region_count;
static uint32_t high_memory_region_count;
static uint32_t high_usable_kib;
static uint32_t high_reserved_kib;

/* High Memory Pool Allocator structures */
static uint64_t high_pool_start_phys = 0x100000000ULL;
static uint32_t high_pool_total_blocks = 0;
static uint32_t high_pool_used_blocks = 0;
static uint32_t *high_bitmap = 0;

static uint32_t align_down(uint32_t value) {
    return value & ~(PAGE_SIZE - 1);
}

static uint32_t align_up(uint32_t value) {
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint32_t capped_add(uint32_t a, uint32_t b) {
    if (0xFFFFFFFF - a < b) {
        return 0xFFFFFFFF;
    }
    return a + b;
}

static uint32_t capped_add_kib(uint32_t a, uint64_t kib) {
    if (kib > 0xFFFFFFFFULL || 0xFFFFFFFF - a < (uint32_t)kib) {
        return 0xFFFFFFFF;
    }
    return a + (uint32_t)kib;
}

void pmm_init(uint32_t mem_size, uint32_t bitmap_addr) {
    max_blocks = mem_size / PAGE_SIZE;
    used_blocks = max_blocks;
    bitmap = (uint32_t *)bitmap_addr;
    boot_bitmap_start = bitmap_addr;

    for (uint32_t i = 0; i < pmm_get_bitmap_size() / sizeof(uint32_t); i++) {
        bitmap[i] = 0xFFFFFFFF;
    }

}

void pmm_record_boot_layout(uint32_t bitmap_start, uint32_t low_reserved_bytes, uint32_t module_count, uint32_t module_reserved_bytes) {
    boot_bitmap_start = bitmap_start;
    boot_low_reserved_bytes = low_reserved_bytes;
    boot_module_count = module_count;
    boot_module_reserved_bytes = module_reserved_bytes;
}

void pmm_reset_memory_regions(void) {
    memory_region_count = 0;
    invalid_memory_region_count = 0;
    high_memory_region_count = 0;
    high_usable_kib = 0;
    high_reserved_kib = 0;
}

void pmm_record_memory_region(uint32_t base, uint32_t size, uint32_t type) {
    if (size == 0) {
        invalid_memory_region_count++;
        return;
    }
    if (base + size < base) {
        invalid_memory_region_count++;
        size = 0xFFFFFFFF - base;
    }
    if (memory_region_count >= PMM_MAX_MEMORY_REGIONS) {
        invalid_memory_region_count++;
        return;
    }

    memory_regions[memory_region_count].base = base;
    memory_regions[memory_region_count].size = size;
    memory_regions[memory_region_count].type = type;
    memory_region_count++;
}

void pmm_record_high_memory_region(uint32_t base_low, uint32_t base_high, uint32_t length_low, uint32_t length_high, uint32_t type) {
    uint64_t base = ((uint64_t)base_high << 32) | base_low;
    uint64_t length = ((uint64_t)length_high << 32) | length_low;
    uint64_t end = base + length;
    uint64_t high_start = 0x100000000ULL;
    uint64_t high_kib;

    if (length == 0) {
        invalid_memory_region_count++;
        return;
    }
    if (end < base) {
        invalid_memory_region_count++;
        end = 0xFFFFFFFFFFFFFFFFULL;
    }
    if (end <= high_start) {
        return;
    }
    if (base > high_start) {
        high_start = base;
    }

    high_kib = (end - high_start) >> 10;
    if (high_kib == 0) {
        return;
    }

    high_memory_region_count++;
    if (type == 1) {
        high_usable_kib = capped_add_kib(high_usable_kib, high_kib);
    } else {
        high_reserved_kib = capped_add_kib(high_reserved_kib, high_kib);
    }
}

uint32_t pmm_memory_region_count(void) {
    return memory_region_count;
}

int pmm_get_memory_region(uint32_t index, pmm_memory_region_t *out) {
    if (!out || index >= memory_region_count) {
        return 0;
    }

    *out = memory_regions[index];
    return 1;
}

static uint32_t pmm_count_memory_region_overlaps(void) {
    uint32_t overlaps = 0;

    for (uint32_t i = 0; i < memory_region_count; i++) {
        uint32_t a_start = memory_regions[i].base;
        uint32_t a_end = memory_regions[i].base + memory_regions[i].size;

        for (uint32_t j = i + 1; j < memory_region_count; j++) {
            uint32_t b_start = memory_regions[j].base;
            uint32_t b_end = memory_regions[j].base + memory_regions[j].size;

            if (a_start < b_end && b_start < a_end) {
                overlaps++;
            }
        }
    }

    return overlaps;
}

void pmm_set_block(uint32_t bit) {
    if (bit >= max_blocks) return;
    if (!(bitmap[bit / 32] & (1 << (bit % 32)))) {
        bitmap[bit / 32] |= (1 << (bit % 32));
        used_blocks++;
    }
}

void pmm_unset_block(uint32_t bit) {
    if (bit >= max_blocks) return;
    if (bitmap[bit / 32] & (1 << (bit % 32))) {
        bitmap[bit / 32] &= ~(1 << (bit % 32));
        used_blocks--;
    }
}

void *pmm_alloc_block() {
    for (uint32_t i = 0; i < max_blocks / 32; i++) {
        if (bitmap[i] != 0xFFFFFFFF) {
            for (int j = 0; j < 32; j++) {
                if (!(bitmap[i] & (1 << j))) {
                    pmm_set_block(i * 32 + j);
                    return (void *)((i * 32 + j) * PAGE_SIZE);
                }
            }
        }
    }
    return 0; // Out of memory
}

void *pmm_alloc_region(uint32_t size) {
    uint32_t needed_blocks;
    uint32_t run_start = 0;
    uint32_t run_len = 0;

    if (size == 0) {
        return 0;
    }

    needed_blocks = align_up(size) / PAGE_SIZE;
    for (uint32_t block = 1; block < max_blocks; block++) {
        if (!(bitmap[block / 32] & (1 << (block % 32)))) {
            if (run_len == 0) {
                run_start = block;
            }
            run_len++;
            if (run_len >= needed_blocks) {
                for (uint32_t i = 0; i < needed_blocks; i++) {
                    pmm_set_block(run_start + i);
                }
                return (void *)(run_start * PAGE_SIZE);
            }
        } else {
            run_len = 0;
        }
    }

    return 0;
}

void pmm_free_block(void *ptr) {
    uint32_t addr = (uint32_t)ptr;
    uint32_t block = addr / PAGE_SIZE;
    pmm_unset_block(block);
}

void pmm_reserve_region(uint32_t base, uint32_t size) {
    uint32_t start = align_down(base) / PAGE_SIZE;
    uint32_t end = align_up(base + size) / PAGE_SIZE;
    for (uint32_t block = start; block < end; block++) {
        pmm_set_block(block);
    }
}

void pmm_free_region(uint32_t base, uint32_t size) {
    uint32_t start = align_down(base) / PAGE_SIZE;
    uint32_t end = align_up(base + size) / PAGE_SIZE;
    for (uint32_t block = start; block < end; block++) {
        pmm_unset_block(block);
    }
}

void pmm_mark_usable_region(uint32_t base, uint32_t size) {
    pmm_free_region(base, size);
}

uint32_t pmm_get_bitmap_size() {
    uint32_t words = (max_blocks + 31) / 32;
    return words * sizeof(uint32_t);
}

uint32_t pmm_largest_free_region_bytes(void) {
    uint32_t largest_run = 0;
    uint32_t current_run = 0;

    for (uint32_t block = 1; block < max_blocks; block++) {
        if (!(bitmap[block / 32] & (1 << (block % 32)))) {
            current_run++;
            if (current_run > largest_run) {
                largest_run = current_run;
            }
        } else {
            current_run = 0;
        }
    }

    return largest_run * PAGE_SIZE;
}

void pmm_get_stats(pmm_stats_t *out) {
    uint32_t usable_bytes = 0;
    uint32_t reserved_bytes = 0;

    if (!out) {
        return;
    }

    for (uint32_t i = 0; i < memory_region_count; i++) {
        if (memory_regions[i].type == 1) {
            usable_bytes = capped_add(usable_bytes, memory_regions[i].size);
        } else {
            reserved_bytes = capped_add(reserved_bytes, memory_regions[i].size);
        }
    }

    out->total_blocks = max_blocks;
    out->used_blocks = used_blocks;
    out->free_blocks = max_blocks - used_blocks;
    out->page_size = PAGE_SIZE;
    out->bitmap_size = pmm_get_bitmap_size();
    out->bitmap_start = boot_bitmap_start;
    out->low_reserved_bytes = boot_low_reserved_bytes;
    out->module_count = boot_module_count;
    out->module_reserved_bytes = boot_module_reserved_bytes;
    out->addressable_bytes = max_blocks * PAGE_SIZE;
    out->mmap_usable_bytes = usable_bytes;
    out->mmap_reserved_bytes = reserved_bytes;
    out->mmap_region_count = memory_region_count;
    out->mmap_invalid_regions = invalid_memory_region_count;
    out->mmap_overlap_count = pmm_count_memory_region_overlaps();
    out->mmap_high_region_count = high_memory_region_count;
    out->mmap_high_usable_kib = high_usable_kib;
    out->mmap_high_reserved_kib = high_reserved_kib;
    out->low_pool_total_blocks = max_blocks;
    out->low_pool_used_blocks = used_blocks;
    out->low_pool_free_blocks = max_blocks - used_blocks;
    out->high_pool_total_blocks = high_pool_total_blocks;
    out->high_pool_used_blocks = high_pool_used_blocks;
    out->high_pool_free_blocks = high_pool_total_blocks - high_pool_used_blocks;
    out->high_pool_allocatable = high_bitmap ? 1 : 0;
}

void pmm_init_high(void) {
    high_pool_total_blocks = high_usable_kib / 4; // 4 KiB per block
    if (high_pool_total_blocks == 0) return;

    // Allocate bitmap for high memory blocks
    uint32_t num_words = (high_pool_total_blocks + 31) / 32;
    uint32_t bitmap_size_bytes = num_words * 4;

    // Allocate from low physical memory pool before VMM/heap starts
    uint32_t num_low_blocks = (bitmap_size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *low_block = pmm_alloc_region(num_low_blocks * PAGE_SIZE);
    if (low_block) {
        high_bitmap = (uint32_t *)low_block;
        memset(high_bitmap, 0, bitmap_size_bytes);
    }
}

phys_addr_t pmm_alloc_high_block(void) {
    if (!high_bitmap || high_pool_total_blocks == 0) {
        return 0;
    }

    uint32_t num_words = (high_pool_total_blocks + 31) / 32;
    for (uint32_t i = 0; i < num_words; i++) {
        if (high_bitmap[i] != 0xFFFFFFFF) {
            for (int j = 0; j < 32; j++) {
                uint32_t block_index = i * 32 + j;
                if (block_index >= high_pool_total_blocks) break;
                if (!(high_bitmap[i] & (1 << j))) {
                    high_bitmap[i] |= (1 << j);
                    high_pool_used_blocks++;
                    return high_pool_start_phys + (uint64_t)block_index * PAGE_SIZE;
                }
            }
        }
    }
    return 0;
}

void pmm_free_high_block(phys_addr_t phys) {
    if (!high_bitmap || phys < high_pool_start_phys) return;
    uint64_t offset = phys - high_pool_start_phys;
    uint32_t block_index = (uint32_t)(offset / PAGE_SIZE);
    if (block_index >= high_pool_total_blocks) return;

    if (high_bitmap[block_index / 32] & (1 << (block_index % 32))) {
        high_bitmap[block_index / 32] &= ~(1 << (block_index % 32));
        high_pool_used_blocks--;
    }
}
