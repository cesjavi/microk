#ifndef HIGHMEM_H
#define HIGHMEM_H

#include <stdint.h>
#include "pmm.h"

/* General-purpose high-memory (>4GB, PAE) buffer: a logically contiguous
 * byte range backed by physical pages that are NOT assumed to be contiguous
 * (the high pool is built from possibly-disjoint Multiboot mmap segments).
 * Access goes through temporary VMM mapping windows, one page at a time. */
typedef struct {
    phys_addr_t *pages;
    uint32_t page_count;
    uint32_t size;
} hbuf_t;

/* Allocates `size` bytes of high memory across page_count = ceil(size/4096)
 * high-pool pages. Returns 1 on success, 0 if high memory is unavailable or
 * exhausted (buf is left zeroed either way). */
int hbuf_alloc(hbuf_t *buf, uint32_t size);
void hbuf_free(hbuf_t *buf);

/* Copies `len` bytes at logical offset `offset` in/out of the buffer,
 * transparently crossing page boundaries. Returns 1 on success, 0 if the
 * range falls outside [0, buf->size). */
int hbuf_read(hbuf_t *buf, uint32_t offset, void *out, uint32_t len);
int hbuf_write(hbuf_t *buf, uint32_t offset, const void *in, uint32_t len);

/* 1=pass, -1=fail, 0=skip (no high memory pool on this boot -- expected on
 * any QEMU target without RAM above 4GB). */
int hbuf_selftest(void);

#endif
