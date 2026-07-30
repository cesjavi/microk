#include "highmem.h"
#include "kheap.h"
#include "vmm.h"
#include "string.h"

/* Dedicated temp-mapping slot for hbuf access, distinct from slot 0 (used by
 * the standalone PAE verification test in syscall.c case 40) so the two
 * don't clobber each other if a hbuf copy ever happens mid-test. */
#define HBUF_WINDOW_SLOT 1

int hbuf_alloc(hbuf_t *buf, uint32_t size) {
    if (!buf || size == 0) return 0;

    uint32_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t *pages = (phys_addr_t *)kmalloc(page_count * sizeof(phys_addr_t));
    if (!pages) return 0;

    for (uint32_t i = 0; i < page_count; i++) {
        phys_addr_t phys = pmm_alloc_high_block();
        if (phys == 0) {
            for (uint32_t j = 0; j < i; j++) {
                pmm_free_high_block(pages[j]);
            }
            kfree(pages);
            return 0;
        }
        pages[i] = phys;
    }

    buf->pages = pages;
    buf->page_count = page_count;
    buf->size = size;
    return 1;
}

void hbuf_free(hbuf_t *buf) {
    if (!buf || !buf->pages) return;

    for (uint32_t i = 0; i < buf->page_count; i++) {
        pmm_free_high_block(buf->pages[i]);
    }
    kfree(buf->pages);
    buf->pages = 0;
    buf->page_count = 0;
    buf->size = 0;
}

static int hbuf_copy(hbuf_t *buf, uint32_t offset, void *out, const void *in, uint32_t len) {
    if (!buf || !buf->pages || (!out && !in) || (uint64_t)offset + len > buf->size) {
        return 0;
    }

    uint32_t remaining = len;
    uint32_t pos = offset;
    uint8_t *out_cursor = (uint8_t *)out;
    const uint8_t *in_cursor = (const uint8_t *)in;

    while (remaining > 0) {
        uint32_t page_index = pos / PAGE_SIZE;
        uint32_t page_offset = pos % PAGE_SIZE;
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > remaining) chunk = remaining;

        uint8_t *window = (uint8_t *)vmm_temp_map_high_slot(HBUF_WINDOW_SLOT, buf->pages[page_index]);
        if (out_cursor) {
            memcpy(out_cursor, window + page_offset, chunk);
            out_cursor += chunk;
        } else {
            memcpy(window + page_offset, in_cursor, chunk);
            in_cursor += chunk;
        }
        vmm_temp_unmap_high_slot(HBUF_WINDOW_SLOT);

        pos += chunk;
        remaining -= chunk;
    }

    return 1;
}

int hbuf_read(hbuf_t *buf, uint32_t offset, void *out, uint32_t len) {
    if (!out) return 0;
    return hbuf_copy(buf, offset, out, 0, len);
}

int hbuf_write(hbuf_t *buf, uint32_t offset, const void *in, uint32_t len) {
    if (!in) return 0;
    return hbuf_copy(buf, offset, 0, in, len);
}

/* Returns 0 (skip) if the high pool has no blocks at all -- true on any QEMU
 * target without RAM above 4GB (i.e. everything except `qemu-highmem`), and
 * not a real failure of the hbuf code. Returns -1 on an actual failure and 1
 * on success, mirroring the PASS/FAIL/SKIP convention used by
 * llm_gguf_selftest(). */
int hbuf_selftest(void) {
    pmm_stats_t stats;
    pmm_get_stats(&stats);
    if (stats.high_pool_total_blocks == 0 || !stats.high_pool_allocatable) {
        return 0;
    }

    hbuf_t buf;
    /* 3 pages: exercises full pages plus a partial cross-page read/write. */
    uint32_t size = PAGE_SIZE * 3;

    if (!hbuf_alloc(&buf, size)) return -1;

    uint8_t pattern[512];
    for (uint32_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 37 + 11);
    }

    /* Fill the whole buffer with a position-dependent byte so page-boundary
     * corruption (wrong window, wrong page index) would show up on readback. */
    for (uint32_t pos = 0; pos < size; pos += sizeof(pattern)) {
        uint32_t chunk = sizeof(pattern);
        if (pos + chunk > size) chunk = size - pos;
        if (!hbuf_write(&buf, pos, pattern, chunk)) {
            hbuf_free(&buf);
            return -1;
        }
    }

    uint8_t readback[512];
    for (uint32_t pos = 0; pos < size; pos += sizeof(readback)) {
        uint32_t chunk = sizeof(readback);
        if (pos + chunk > size) chunk = size - pos;
        if (!hbuf_read(&buf, pos, readback, chunk)) {
            hbuf_free(&buf);
            return -1;
        }
        if (memcmp(readback, pattern, chunk) != 0) {
            hbuf_free(&buf);
            return -1;
        }
    }

    /* Cross-page unaligned write/read spanning the page-0/page-1 boundary. */
    const char *straddle = "PAE-hbuf-cross-page-boundary-marker";
    uint32_t straddle_len = (uint32_t)strlen(straddle);
    uint32_t straddle_off = PAGE_SIZE - 8;
    if (!hbuf_write(&buf, straddle_off, straddle, straddle_len)) {
        hbuf_free(&buf);
        return -1;
    }
    char straddle_readback[64];
    memset(straddle_readback, 0, sizeof(straddle_readback));
    if (!hbuf_read(&buf, straddle_off, straddle_readback, straddle_len)) {
        hbuf_free(&buf);
        return -1;
    }
    if (memcmp(straddle_readback, straddle, straddle_len) != 0) {
        hbuf_free(&buf);
        return -1;
    }

    /* Out-of-range access must fail cleanly, not silently truncate. */
    uint8_t scratch;
    if (hbuf_read(&buf, size - 1, &scratch, 2)) {
        hbuf_free(&buf);
        return -1;
    }

    hbuf_free(&buf);
    return 1;
}
