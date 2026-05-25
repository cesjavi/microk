#include "llm_gguf.h"
#include "video.h"
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
    uint8_t *base = (uint8_t *)start;
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
    uint8_t *base = (uint8_t *)start;
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
            if (!gguf_range_ok((uint32_t)base, size, offset, 8)) return 0;
            len = *(uint64_t *)(base + offset);
            if (len > 0xFFFFFFFFULL) return 0;
            if (!gguf_range_ok((uint32_t)base, size, offset + 8, (uint32_t)len)) return 0;
            skip = 8 + (uint32_t)len;
            break;
        }
        case GGUF_METADATA_ARRAY: {
            uint32_t sub_type;
            uint64_t count;
            uint32_t inner_skip;

            if (!gguf_range_ok((uint32_t)base, size, offset, 12)) return 0;
            sub_type = *(uint32_t *)(base + offset);
            count = *(uint64_t *)(base + offset + 4);
            if (count > 262144) return 0;
            skip = 4 + 8;
            for (uint64_t i = 0; i < count; i++) {
                if (!gguf_skip_value(base, size, offset + skip, sub_type, &inner_skip)) return 0;
                if (!gguf_range_ok((uint32_t)base, size, offset + skip, inner_skip)) return 0;
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
    uint8_t *base = (uint8_t *)start;
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

/**
 * @brief Validates a GGUF file and calculates the offsets for metadata and tensor info.
 */
int gguf_probe(uint32_t start, uint32_t size, gguf_info_t *info) {
    uint32_t metadata_offset;
    uint32_t tensor_info_offset;
    uint32_t data_offset;

    if (size < sizeof(gguf_header_t)) {
        klog("GGUF Error: File too small for header.\n");
        return 0;
    }
    
    gguf_header_t *header = (gguf_header_t *)start;
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
        uint8_t *base = (uint8_t *)start;
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

    if (info) {
        info->valid = 1;
        info->version = header->version;
        info->tensor_count = (uint32_t)header->tensor_count;
        info->metadata_count = (uint32_t)header->metadata_count;
        info->metadata_offset = metadata_offset;
        info->tensor_info_offset = tensor_info_offset;
        info->data_offset = data_offset;

        // Populate architecture info without recursing back into gguf_probe().
        memset(&info->arch, 0, sizeof(gguf_arch_t));
        gguf_get_metadata_value_from_info(start, size, info, "general.name", GGUF_METADATA_STRING, info->arch.name);

        {
            uint64_t val64;
            uint32_t vocab_count = 0;
            if (gguf_get_metadata_value_from_info(start, size, info, "llama.block_count", GGUF_METADATA_UINT32, &info->arch.n_layer)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.block_count", GGUF_METADATA_UINT64, &val64)) { info->arch.n_layer = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, info, "llama.embedding_length", GGUF_METADATA_UINT32, &info->arch.n_embd)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.embedding_length", GGUF_METADATA_UINT64, &val64)) { info->arch.n_embd = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, info, "llama.attention.head_count", GGUF_METADATA_UINT32, &info->arch.n_head)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.attention.head_count", GGUF_METADATA_UINT64, &val64)) { info->arch.n_head = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, info, "llama.attention.head_count_kv", GGUF_METADATA_UINT32, &info->arch.n_kv_head)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.attention.head_count_kv", GGUF_METADATA_UINT64, &val64)) { info->arch.n_kv_head = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, info, "llama.context_length", GGUF_METADATA_UINT32, &info->arch.n_ctx)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.context_length", GGUF_METADATA_UINT64, &val64)) { info->arch.n_ctx = (uint32_t)val64; }

            if (gguf_get_metadata_value_from_info(start, size, info, "llama.vocab_size", GGUF_METADATA_UINT32, &info->arch.n_vocab)) {}
            else if (gguf_get_metadata_value_from_info(start, size, info, "llama.vocab_size", GGUF_METADATA_UINT64, &val64)) { info->arch.n_vocab = (uint32_t)val64; }
            else if (gguf_get_metadata_array_from_info(start, size, info, "tokenizer.ggml.tokens", &vocab_count, 0)) { info->arch.n_vocab = vocab_count; }
        }

        gguf_get_metadata_value_from_info(start, size, info, "llama.attention.layer_norm_rms_epsilon", GGUF_METADATA_FLOAT32, &info->arch.rms_norm_eps);
    }
    return 1;
}

/**
 * @brief Finds a tensor by name and retrieves its metadata (shape, type, offset).
 */
int gguf_get_tensor(uint32_t start, uint32_t size, const char *name, gguf_tensor_t *out_tensor) {
    gguf_info_t info;
    uint8_t *base = (uint8_t *)start;
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
