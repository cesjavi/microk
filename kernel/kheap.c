#include "kheap.h"
#include "pmm.h"
#include <string.h>

#define HEAP_START 0xC00000  // 12MB
/* 4MB was tight even for a tiny model (stories15M): per-token-string
 * vocab allocation (~900KB-1MB for a 32K-token tokenizer), per-layer KV
 * cache (which scales with n_layer * n_kv_head, e.g. ~2.8MB for
 * TinyLlama-1.1B's 22 layers), and the norm-weight copies together can
 * exceed 4MB well before a 1B-class model's FPU scratch buffers get
 * allocated -- which surfaced as "LLM: Failed to allocate FPU buffer."
 * with no other warning. kheap_init() runs before boot_modules_load(),
 * so PMM reserves whatever size this is before the model buffer gets
 * placed; bumping it doesn't reduce how much RAM is left for the model
 * itself by more than the few extra MB reserved here. */
#define HEAP_SIZE  0x2000000 // 32MB
#define KHEAP_MAGIC 0x4B484541 // KHEA

typedef struct header {
    struct header *next;
    size_t size;
    uint32_t magic;
    int is_free;
} header_t;

static header_t *heap_start = NULL;

static size_t align4(size_t size) {
    return (size + 3) & ~((size_t)3);
}

static int header_in_heap(header_t *header) {
    uint32_t addr = (uint32_t)header;
    return addr >= HEAP_START && addr + sizeof(header_t) <= HEAP_START + HEAP_SIZE;
}

static header_t *ptr_to_header(void *ptr) {
    if (!ptr) {
        return NULL;
    }

    uint32_t addr = (uint32_t)ptr;
    if (addr < HEAP_START + sizeof(header_t) || addr >= HEAP_START + HEAP_SIZE) {
        return NULL;
    }

    header_t *header = (header_t *)((char *)ptr - sizeof(header_t));
    if (!header_in_heap(header) || header->magic != KHEAP_MAGIC) {
        return NULL;
    }

    return header;
}

static void coalesce_forward(header_t *block) {
    while (block && block->next && block->is_free && block->next->is_free) {
        header_t *next = block->next;
        if (!header_in_heap(next) || next->magic != KHEAP_MAGIC) {
            return;
        }

        block->size += sizeof(header_t) + next->size;
        block->next = next->next;
    }
}

void kheap_init() {
    pmm_reserve_region(HEAP_START, HEAP_SIZE);
    heap_start = (header_t *)HEAP_START;
    heap_start->size = HEAP_SIZE - sizeof(header_t);
    heap_start->next = NULL;
    heap_start->magic = KHEAP_MAGIC;
    heap_start->is_free = 1;
}

void *kmalloc(size_t size) {
    size = align4(size);
    if (size == 0) {
        return NULL;
    }

    header_t *curr = heap_start;
    while (curr) {
        if (!header_in_heap(curr) || curr->magic != KHEAP_MAGIC) {
            return NULL;
        }

        if (curr->is_free && curr->size >= size) {
            // Split block if possible
            if (curr->size >= size + sizeof(header_t) + 4) {
                header_t *next = (header_t *)((char *)curr + sizeof(header_t) + size);
                next->size = curr->size - size - sizeof(header_t);
                next->magic = KHEAP_MAGIC;
                next->is_free = 1;
                next->next = curr->next;
                
                curr->size = size;
                curr->next = next;
            }
            curr->is_free = 0;
            return (void *)((char *)curr + sizeof(header_t));
        }
        curr = curr->next;
    }
    return NULL; // Out of heap memory
}

void kfree(void *ptr) {
    header_t *header = ptr_to_header(ptr);
    if (!header || header->is_free) {
        return;
    }

    header->is_free = 1;
    coalesce_forward(header);

    header_t *curr = heap_start;
    while (curr && curr->next) {
        if (!header_in_heap(curr) || curr->magic != KHEAP_MAGIC) {
            return;
        }
        if (curr->next == header) {
            coalesce_forward(curr);
            return;
        }
        curr = curr->next;
    }
}

void kheap_get_stats(kheap_stats_t *out) {
    header_t *curr = heap_start;

    if (!out) {
        return;
    }

    memset(out, 0, sizeof(kheap_stats_t));
    out->start = HEAP_START;
    out->size = HEAP_SIZE;

    while (curr) {
        if (!header_in_heap(curr) || curr->magic != KHEAP_MAGIC) {
            return;
        }
        out->blocks++;
        if (curr->is_free) {
            out->free_blocks++;
            out->free_bytes += curr->size;
        } else {
            out->used_bytes += curr->size;
        }
        curr = curr->next;
    }
}

int kheap_selftest(void) {
    kheap_stats_t before;
    kheap_stats_t after;
    void *a;
    void *b;
    void *c;
    void *d;

    kheap_get_stats(&before);

    a = kmalloc(32);
    b = kmalloc(128);
    c = kmalloc(256);
    if (!a || !b || !c) {
        if (a) kfree(a);
        if (b) kfree(b);
        if (c) kfree(c);
        return 0;
    }

    memset(a, 0xA1, 32);
    memset(b, 0xB2, 128);
    memset(c, 0xC3, 256);

    kfree(b);
    d = kmalloc(64);
    if (!d) {
        kfree(a);
        kfree(c);
        return 0;
    }
    memset(d, 0xD4, 64);

    kfree(a);
    kfree(c);
    kfree(d);

    // Invalid frees should be ignored by pointer validation.
    kfree((void *)0x1234);
    kfree(NULL);

    kheap_get_stats(&after);

    return after.free_bytes >= before.free_bytes &&
           after.free_blocks <= before.free_blocks + 1;
}
