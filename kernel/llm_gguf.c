#include "llm_gguf.h"
#include "video.h"
#include "highmem.h"
#include "kheap.h"
#include <string.h>

#define GGUF_MAGIC 0x46554747 /* GGUF */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t tensor_count;
    uint64_t metadata_count;
} __attribute__((packed)) gguf_header_t;

static int gguf_range_ok(uint32_t start, uint32_t size, uint32_t offset, uint32_t need);
static int gguf_skip_value(uint8_t *base, uint32_t size, uint32_t offset, uint32_t type, uint32_t *skip_out);
static int gguf_get_metadata_array_from_info(uint32_t start, uint32_t size, gguf_info_t *info, const char *key, uint32_t *out_count, uint8_t **out_ptr);
static int gguf_find_metadata_entry(uint32_t start, uint32_t size, uint32_t metadata_offset, uint32_t metadata_count, const char *key, uint32_t *out_type, uint32_t *out_value_offset) {
    uint8_t *base = (uint8_t *)(uintptr_t)start;
    uint32_t ptr = metadata_offset;

    for (uint32_t i = 0; i < metadata_count; i++) {
        uint64_t key_len;
        uint32_t val_type;
        uint32_t value_skip;
        char current_key[64];
        uint32_t to_copy;

        if (!gguf_range_ok(start, size, ptr, 8)) return 0;
        key_len = *(uint64_t *)(base + ptr);
        ptr += 8;
        if (key_len > 255 || !gguf_range_ok(start, size, ptr, (uint32_t)key_len)) return 0;

        to_copy = (key_len > 63) ? 63 : (uint32_t)key_len;
        memcpy(current_key, base + ptr, to_copy);
        current_key[to_copy] = '\0';
        ptr += (uint32_t)key_len;

        if (!gguf_range_ok(start, size, ptr, 4)) return 0;
        val_type = *(uint32_t *)(base + ptr);
        ptr += 4;

        if (strcmp(current_key, key) == 0) {
            if (out_type) *out_type = val_type;
            if (out_value_offset) *out_value_offset = ptr;
            return 1;
        }

        if (!gguf_skip_value(base, size, ptr, val_type, &value_skip)) return 0;
        ptr += value_skip;
    }

    return 0;
}

static int gguf_get_metadata_array_from_info(uint32_t start, uint32_t size, gguf_info_t *info, const char *key, uint32_t *out_count, uint8_t **out_ptr) {
    uint8_t *base = (uint8_t *)(uintptr_t)start;
    uint32_t val_type;
    uint32_t value_offset;

    if (!info) return 0;
    if (!gguf_find_metadata_entry(start, size, info->metadata_offset, info->metadata_count, key, &val_type, &value_offset)) {
        return 0;
    }
    if (val_type != GGUF_METADATA_ARRAY) {
        return 0;
    }
    if (!gguf_range_ok(start, size, value_offset, 12)) {
        return 0;
    }

    if (out_count) *out_count = (uint32_t)(*(uint64_t *)(base + value_offset + 4));
    if (out_ptr) *out_ptr = base + value_offset + 12;
    return 1;
}

static int gguf_range_ok(uint32_t start, uint32_t size, uint32_t offset, uint32_t need) {
    (void)start;
    if (offset > size) {
        return 0;
    }
    return need <= (size - offset);
}

static int gguf_skip_value(uint8_t *base, uint32_t size, uint32_t offset, uint32_t type, uint32_t *skip_out) {
    uint32_t skip = 0;

    if (!skip_out) {
        return 0;
    }

    switch (type) {
        case GGUF_METADATA_UINT8: case GGUF_METADATA_INT8: case GGUF_METADATA_BOOL: skip = 1; break;
        case GGUF_METADATA_UINT16: case GGUF_METADATA_INT16: skip = 2; break;
        case GGUF_METADATA_UINT32: case GGUF_METADATA_INT32: case GGUF_METADATA_FLOAT32: skip = 4; break;
        case GGUF_METADATA_UINT64: case GGUF_METADATA_INT64: case GGUF_METADATA_FLOAT64: skip = 8; break;
        case GGUF_METADATA_STRING: {
            uint64_t len;
            if (!gguf_range_ok((uint32_t)(uintptr_t)base, size, offset, 8)) return 0;
            len = *(uint64_t *)(base + offset);
            if (len > 0xFFFFFFFFULL) return 0;
            if (!gguf_range_ok((uint32_t)(uintptr_t)base, size, offset + 8, (uint32_t)len)) return 0;
            skip = 8 + (uint32_t)len;
            break;
        }
        case GGUF_METADATA_ARRAY: {
            uint32_t sub_type;
            uint64_t count;
            uint32_t inner_skip;

            if (!gguf_range_ok((uint32_t)(uintptr_t)base, size, offset, 12)) return 0;
            sub_type = *(uint32_t *)(base + offset);
            count = *(uint64_t *)(base + offset + 4);
            if (count > 262144) return 0;
            skip = 4 + 8;
            for (uint64_t i = 0; i < count; i++) {
                if (!gguf_skip_value(base, size, offset + skip, sub_type, &inner_skip)) return 0;
                if (!gguf_range_ok((uint32_t)(uintptr_t)base, size, offset + skip, inner_skip)) return 0;
                skip += inner_skip;
            }
            break;
        }
        default:
            return 0;
    }

    *skip_out = skip;
    return 1;
}

static int gguf_get_metadata_value_from_info(uint32_t start, uint32_t size, gguf_info_t *info, const char *key, uint32_t expected_type, void *out_val) {
    uint8_t *base = (uint8_t *)(uintptr_t)start;
    uint32_t val_type;
    uint32_t value_offset;

    if (!info) {
        return 0;
    }
    if (!gguf_find_metadata_entry(start, size, info->metadata_offset, info->metadata_count, key, &val_type, &value_offset)) {
        return 0;
    }
    if (val_type != expected_type) {
        return 0;
    }

    if (val_type == GGUF_METADATA_UINT32 || val_type == GGUF_METADATA_INT32 || val_type == GGUF_METADATA_FLOAT32) {
        if (!gguf_range_ok(start, size, value_offset, 4)) return 0;
        memcpy(out_val, base + value_offset, 4);
        return 1;
    }
    if (val_type == GGUF_METADATA_UINT64 || val_type == GGUF_METADATA_INT64) {
        if (!gguf_range_ok(start, size, value_offset, 8)) return 0;
        memcpy(out_val, base + value_offset, 8);
        return 1;
    }
    if (val_type == GGUF_METADATA_STRING) {
        uint64_t len;
        uint32_t copy_len;
        if (!gguf_range_ok(start, size, value_offset, 8)) return 0;
        len = *(uint64_t *)(base + value_offset);
        if (len > 255 || !gguf_range_ok(start, size, value_offset + 8, (uint32_t)len)) return 0;
        copy_len = (len > 63) ? 63 : (uint32_t)len;
        memcpy(out_val, base + value_offset + 8, copy_len);
        ((char *)out_val)[copy_len] = '\0';
        return 1;
    }

    return 0;
}

uint32_t ggml_type_size(uint32_t type) {
    switch (type) {
        case GGML_TYPE_F32:  return 4;
        case GGML_TYPE_F16:  return 2;
        case GGML_TYPE_Q4_0: return 18; // 32 blocks: 2 (f16 delta) + 16 (data)
        case GGML_TYPE_I8:   return 1;
        case GGML_TYPE_I16:  return 2;
        case GGML_TYPE_I32:  return 4;
        default: return 0;
    }
}

/* gguf_probe rescans the whole metadata section (including potentially
 * tens of thousands of vocabulary entries) on every call. gguf_get_tensor,
 * gguf_get_metadata_value/array and tensor.c's GGUF loaders all call it
 * once per lookup, so a single token generation step could trigger dozens
 * of full O(n_vocab) metadata scans. Cache the last successful probe by
 * (start, size) so repeated lookups against the same loaded model are O(1). */
static gguf_info_t gguf_probe_cache;
static uint32_t gguf_probe_cache_start = 0;
static uint32_t gguf_probe_cache_size = 0;
static int gguf_probe_cache_valid = 0;

void gguf_probe_invalidate(void) {
    gguf_probe_cache_valid = 0;
}

/**
 * @brief Validates a GGUF file and calculates the offsets for metadata and tensor info.
 */
int gguf_probe(uint32_t start, uint32_t size, gguf_info_t *info) {
    uint32_t metadata_offset;
    uint32_t tensor_info_offset;
    uint32_t data_offset;

    if (gguf_probe_cache_valid && gguf_probe_cache_start == start && gguf_probe_cache_size == size) {
        if (info) *info = gguf_probe_cache;
        return 1;
    }

    if (size < sizeof(gguf_header_t)) {
        klog("GGUF Error: File too small for header.\n");
        return 0;
    }
    
    gguf_header_t *header = (gguf_header_t *)(uintptr_t)start;
    if (header->magic != GGUF_MAGIC) {
        klog("GGUF Error: Invalid Magic Number.\n");
        return 0;
    }
    
    // Safety check for metadata/tensor offsets
    if (header->metadata_count > 1000 || header->tensor_count > 1000) {
        klog("GGUF Error: Suspected invalid header (too many entries).\n");
        return 0;
    }
    if (header->version == 0 || header->version > 3) {
        klog("GGUF Error: Unsupported version.\n");
        return 0;
    }

    metadata_offset = sizeof(gguf_header_t);
    tensor_info_offset = metadata_offset;

    {
        uint8_t *base = (uint8_t *)(uintptr_t)start;
        uint32_t ptr = metadata_offset;
        for (uint32_t i = 0; i < (uint32_t)header->metadata_count; i++) {
            uint64_t key_len;
            uint32_t val_type;
            uint32_t value_skip;

            if (!gguf_range_ok(start, size, ptr, 8)) return 0;
            key_len = *(uint64_t *)(base + ptr);
            ptr += 8;
            if (key_len > 255 || !gguf_range_ok(start, size, ptr, (uint32_t)key_len)) return 0;
            ptr += (uint32_t)key_len;
            if (!gguf_range_ok(start, size, ptr, 4)) return 0;
            val_type = *(uint32_t *)(base + ptr);
            ptr += 4;
            if (!gguf_skip_value(base, size, ptr, val_type, &value_skip)) return 0;
            if (!gguf_range_ok(start, size, ptr, value_skip)) return 0;
            ptr += value_skip;
        }
        tensor_info_offset = ptr;

        for (uint32_t i = 0; i < (uint32_t)header->tensor_count; i++) {
            uint64_t name_len;
            uint32_t ndims;

            if (!gguf_range_ok(start, size, ptr, 8)) return 0;
            name_len = *(uint64_t *)(base + ptr);
            ptr += 8;
            if (name_len > 255 || !gguf_range_ok(start, size, ptr, (uint32_t)name_len)) return 0;
            ptr += (uint32_t)name_len;
            if (!gguf_range_ok(start, size, ptr, 4)) return 0;
            ndims = *(uint32_t *)(base + ptr);
            if (ndims == 0 || ndims > 4) return 0;
            ptr += 4;
            if (!gguf_range_ok(start, size, ptr, ndims * 8)) return 0;
            ptr += ndims * 8;
            if (!gguf_range_ok(start, size, ptr, 12)) return 0;
            ptr += 12;
        }

        data_offset = (ptr + 31) & ~31U;
        if (data_offset > size) return 0;
    }

    {
        gguf_info_t computed;
        memset(&computed, 0, sizeof(computed));
        computed.valid = 1;
        computed.version = header->version;
        computed.tensor_count = (uint32_t)header->tensor_count;
        computed.metadata_count = (uint32_t)header->metadata_count;
        computed.metadata_offset = metadata_offset;
        computed.tensor_info_offset = tensor_info_offset;
        computed.data_offset = data_offset;

        // Populate architecture info without recursing back into gguf_probe().
        gguf_get_metadata_value_from_info(start, size, &computed, "general.name", GGUF_METADATA_STRING, computed.arch.name);

        {
            uint64_t val64;
            uint32_t vocab_count = 0;
            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.block_count", GGUF_METADATA_UINT32, &computed.arch.n_layer)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.block_count", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_layer = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.embedding_length", GGUF_METADATA_UINT32, &computed.arch.n_embd)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.embedding_length", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_embd = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.attention.head_count", GGUF_METADATA_UINT32, &computed.arch.n_head)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.attention.head_count", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_head = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.attention.head_count_kv", GGUF_METADATA_UINT32, &computed.arch.n_kv_head)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.attention.head_count_kv", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_kv_head = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.context_length", GGUF_METADATA_UINT32, &computed.arch.n_ctx)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.context_length", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_ctx = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.vocab_size", GGUF_METADATA_UINT32, &computed.arch.n_vocab)) {}
            else if (gguf_get_metadata_value_from_info(start, size, &computed, "llama.vocab_size", GGUF_METADATA_UINT64, &val64)) { computed.arch.n_vocab = (uint32_t)val64; }
            else if (gguf_get_metadata_array_from_info(start, size, &computed, "tokenizer.ggml.tokens", &vocab_count, 0)) { computed.arch.n_vocab = vocab_count; }
        }

        gguf_get_metadata_value_from_info(start, size, &computed, "llama.attention.layer_norm_rms_epsilon", GGUF_METADATA_FLOAT32, &computed.arch.rms_norm_eps);

        /* Default matches every Llama checkpoint seen so far (stories15M,
         * TinyLlama) so this is a no-op for them; only overridden for models
         * that actually set a different llama.rope.freq_base. */
        computed.arch.rope_freq_base = 10000.0f;
        gguf_get_metadata_value_from_info(start, size, &computed, "llama.rope.freq_base", GGUF_METADATA_FLOAT32, &computed.arch.rope_freq_base);

        gguf_probe_cache = computed;
        gguf_probe_cache_start = start;
        gguf_probe_cache_size = size;
        gguf_probe_cache_valid = 1;

        if (info) *info = computed;
    }
    return 1;
}

/**
 * @brief Computes the exact on-disk byte size of a tensor for the types this
 * kernel actually dereferences (F32/F16/Q4_0/Q6_K row math in tensor.c and
 * llm.c). Returns 0 (size unknown) for any other type -- callers must then
 * fall back to validating only that the tensor's start offset is in range.
 */
static int gguf_tensor_byte_size(uint32_t type, const uint32_t *dims, uint32_t ndims, uint64_t *out_size) {
    /* rows = product of every dim except dims[0], so count = rows*dims[0]
     * without ever needing a 64-bit/64-bit division (no __udivdi3 on i386
     * without libgcc linked in) -- only the 32-bit dims[0] is divided below. */
    uint64_t rows = 1;
    for (uint32_t d = 1; d < ndims; d++) {
        rows *= dims[d];
    }

    switch (type) {
        case GGML_TYPE_F32:
            *out_size = rows * dims[0] * 4;
            return 1;
        case GGML_TYPE_F16:
            *out_size = rows * dims[0] * 2;
            return 1;
        case GGML_TYPE_Q4_0:
            if (ndims == 0 || dims[0] == 0 || (dims[0] % 32) != 0) return 0;
            *out_size = rows * (dims[0] / 32) * 18;
            return 1;
        case GGML_TYPE_Q6_K:
            if (ndims == 0 || dims[0] == 0 || (dims[0] % 256) != 0) return 0;
            *out_size = rows * (dims[0] / 256) * 210;
            return 1;
        default:
            return 0;
    }
}

/**
 * @brief Finds a tensor by name and retrieves its metadata (shape, type, offset).
 */
int gguf_get_tensor(uint32_t start, uint32_t size, const char *name, gguf_tensor_t *out_tensor) {
    gguf_info_t info;
    uint8_t *base = (uint8_t *)(uintptr_t)start;
    if (!gguf_probe(start, size, &info)) return 0;

    uint32_t ptr = info.tensor_info_offset;
    for (uint32_t i = 0; i < info.tensor_count; i++) {
        extern int sys_abort_requested();
        if (sys_abort_requested()) return 0;
        uint64_t name_len;
        uint32_t ndims;
        uint32_t dims[4] = {0};
        uint32_t type;
        uint64_t offset;

        if (!gguf_range_ok(start, size, ptr, 8)) return 0;
        name_len = *(uint64_t *)(base + ptr);
        ptr += 8;
        if (name_len > 255 || !gguf_range_ok(start, size, ptr, (uint32_t)name_len)) return 0;
        
        char tensor_name[64];
        uint32_t to_copy = (name_len > 63) ? 63 : (uint32_t)name_len;
        memcpy(tensor_name, base + ptr, to_copy);
        tensor_name[to_copy] = '\0';
        ptr += name_len;

        if (!gguf_range_ok(start, size, ptr, 4)) return 0;
        ndims = *(uint32_t *)(base + ptr);
        if (ndims == 0 || ndims > 4) return 0;
        ptr += 4;
        
        for (uint32_t d = 0; d < ndims; d++) {
            if (!gguf_range_ok(start, size, ptr, 8)) return 0;
            dims[d] = (uint32_t)(*(uint64_t *)(base + ptr));
            ptr += 8;
        }

        if (!gguf_range_ok(start, size, ptr, 12)) return 0;
        type = *(uint32_t *)(base + ptr);
        ptr += 4;
        offset = *(uint64_t *)(base + ptr);
        ptr += 8;

        if (strcmp(tensor_name, name) == 0) {
            /* The tensor's data offset/dims come straight from the file and
             * are used for raw pointer arithmetic by every consumer (llm.c,
             * tensor.c). Reject anything that doesn't actually fit inside
             * the loaded file before handing it back -- otherwise a crafted
             * offset near the top of the 32-bit range (or dims implying a
             * huge row) turns into an out-of-bounds read/OOB pointer later. */
            uint64_t data_start = (uint64_t)info.data_offset + offset;
            if (data_start > (uint64_t)size) return 0;

            uint64_t byte_size;
            if (gguf_tensor_byte_size(type, dims, ndims, &byte_size)) {
                if (byte_size > (uint64_t)size - data_start) return 0;
            }

            if (out_tensor) {
                strcpy(out_tensor->name, tensor_name);
                out_tensor->type = type;
                out_tensor->ndims = ndims;
                memcpy(out_tensor->dims, dims, sizeof(dims));
                out_tensor->offset = (uint32_t)offset;
            }
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Retrieves a metadata array (like vocabulary tokens) by its key.
 */
int gguf_get_metadata_value(uint32_t start, uint32_t size, const char *key, uint32_t expected_type, void *out_val) {
    gguf_info_t info;

    if (!gguf_probe(start, size, &info)) return 0;
    return gguf_get_metadata_value_from_info(start, size, &info, key, expected_type, out_val);
}

/**
 * @brief Retrieves a metadata array (like vocabulary tokens) by its key.
 */
int gguf_get_metadata_array(uint32_t start, uint32_t size, const char *key, uint32_t *out_count, uint8_t **out_ptr) {
    gguf_info_t info;

    if (!gguf_probe(start, size, &info)) return 0;
    return gguf_get_metadata_array_from_info(start, size, &info, key, out_count, out_ptr);
}

/* Single-slot high-memory backing for the current model's tensor DATA
 * section, mirroring gguf_probe_cache's single-slot (start,size) design
 * above -- MicroK only ever has one model loaded at a time. */
static uint32_t gguf_hbuf_start = 0;
static hbuf_t gguf_hbuf_data;

/* Below this, migration isn't worth the added indirection -- covers the
 * small test fixtures (tiny.gguf, tin1.gguf) that should keep their current
 * zero-copy direct-pointer behavior unconditionally. */
#define GGUF_HIGH_MEM_MIN_BYTES (8u * 1024u * 1024u)
#define GGUF_HIGH_MEM_COPY_CHUNK 65536u

int gguf_migrate_data_to_high_mem(uint32_t file_start, uint32_t file_size, uint32_t data_offset) {
    if (data_offset >= file_size) return 0;
    uint32_t data_size = file_size - data_offset;
    if (data_size < GGUF_HIGH_MEM_MIN_BYTES) {
        klog("GGUF: model data section too small to migrate to high memory, staying low.\n");
        return 0;
    }

    pmm_stats_t stats;
    pmm_get_stats(&stats);
    if (stats.high_pool_total_blocks == 0 || !stats.high_pool_allocatable) {
        klog("GGUF: no high memory pool available, model data stays low.\n");
        return 0;
    }

    hbuf_t hb;
    if (!hbuf_alloc(&hb, data_size)) {
        klog("GGUF: high memory allocation for model data failed, staying low.\n");
        return 0;
    }

    const uint8_t *src = (const uint8_t *)(file_start + data_offset);

    for (uint32_t off = 0; off < data_size; off += GGUF_HIGH_MEM_COPY_CHUNK) {
        uint32_t n = data_size - off;
        if (n > GGUF_HIGH_MEM_COPY_CHUNK) n = GGUF_HIGH_MEM_COPY_CHUNK;
        if (!hbuf_write(&hb, off, src + off, n)) {
            klog("GGUF: copy to high memory failed midway, staying low.\n");
            hbuf_free(&hb);
            return 0;
        }
    }

    /* Deliberately NOT reclaiming the low-memory pages that held the data
     * section here (a straightforward pmm_free_region() call would do it).
     * Investigation found that freeing this range and letting it be reused
     * by later allocations correlates with a rare, reproducible single-page
     * data corruption elsewhere in the migrated copy, whose root cause we
     * could not pin down: it isn't the copy loop, the chunk size, timer
     * interrupts, or a duplicate physical-page allocation (all ruled out by
     * direct instrumentation and by disabling interrupts around the copy),
     * and the only code path in this kernel that can reach physical memory
     * above 4GB (vmm_temp_map_high_slot, exclusively via this file) shows no
     * unaccounted-for access to the affected page. See
     * docs/PAE_MEMORY_PLAN.md for the full investigation. Reads still go
     * through the high-memory hbuf (verified correct across the whole
     * buffer in every test run); we just leave the original low-memory
     * bytes reserved and unused instead of returning them to the pool, so
     * this feature is safe -- it costs the low-memory savings this
     * milestone was meant to provide, not correctness. */

    gguf_hbuf_data = hb;
    gguf_hbuf_start = file_start;
    klog("GGUF: migrated model data section to high memory (low-memory copy kept reserved, not freed).\n");
    return 1;
}

void gguf_release_high_mem_backing(uint32_t file_start) {
    if (gguf_hbuf_start != 0 && gguf_hbuf_start == file_start) {
        hbuf_free(&gguf_hbuf_data);
        gguf_hbuf_start = 0;
    }
}

int gguf_read_data_bytes(uint32_t file_start, uint32_t file_size, uint32_t data_offset, uint32_t tensor_offset, void *out, uint32_t len) {
    (void)file_size;
    if (gguf_hbuf_start != 0 && gguf_hbuf_start == file_start) {
        return hbuf_read(&gguf_hbuf_data, tensor_offset, out, len);
    }
    memcpy(out, (const void *)(file_start + data_offset + tensor_offset), len);
    return 1;
}
