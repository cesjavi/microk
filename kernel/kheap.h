#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>
#include <stddef.h>

void kheap_init();
void *kmalloc(size_t size);
void kfree(void *ptr);

typedef struct {
    uint32_t start;
    uint32_t size;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t blocks;
    uint32_t free_blocks;
} kheap_stats_t;

void kheap_get_stats(kheap_stats_t *out);
int kheap_selftest(void);

#endif
