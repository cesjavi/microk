#ifndef LLM_GGUF_H
#define LLM_GGUF_H

#include <stdint.h>

typedef enum {
    GGUF_METADATA_UINT8 = 0,
    GGUF_METADATA_INT8 = 1,
    GGUF_METADATA_UINT16 = 2,
    GGUF_METADATA_INT16 = 3,
    GGUF_METADATA_UINT32 = 4,
    GGUF_METADATA_INT32 = 5,
    GGUF_METADATA_FLOAT32 = 6,
    GGUF_METADATA_BOOL = 7,
    GGUF_METADATA_STRING = 8,
    GGUF_METADATA_ARRAY = 9,
    GGUF_METADATA_UINT64 = 10,
    GGUF_METADATA_INT64 = 11,
    GGUF_METADATA_FLOAT64 = 12,
} gguf_metadata_type_t;

typedef enum {
    GGML_TYPE_F32  = 0,
    GGML_TYPE_F16  = 1,
    GGML_TYPE_Q4_0 = 2,
    GGML_TYPE_Q4_1 = 3,
    // ...
    GGML_TYPE_Q6_K = 14, /* legacy llama.cpp quantizations (e.g. plain "Q4_0")
                          * commonly keep output.weight/token_embd.weight at
                          * Q6_K for quality even though the rest of the
                          * model is Q4_0 -- e.g. TinyLlama-1.1B-Chat Q4_0.gguf. */
    GGML_TYPE_I8   = 16,
    GGML_TYPE_I16  = 17,
    GGML_TYPE_I32  = 18,
} ggml_type_t;

typedef struct {
    char name[64];
    uint32_t n_vocab;
    uint32_t n_embd;
    uint32_t n_layer;
    uint32_t n_head;
    uint32_t n_kv_head;
    uint32_t n_ctx;
    float rms_norm_eps;
} gguf_arch_t;

typedef struct {
    int valid;
    uint32_t version;
    uint32_t tensor_count;
    uint32_t metadata_count;
    uint32_t metadata_offset;
    uint32_t tensor_info_offset;
    uint32_t data_offset;
    gguf_arch_t arch;
} gguf_info_t;

typedef struct {
    char name[64];
    uint32_t type;
    uint32_t dims[4];
    uint32_t ndims;
    uint32_t offset;
    uint32_t size;
} gguf_tensor_t;

int gguf_probe(uint32_t start, uint32_t size, gguf_info_t *info);
void gguf_probe_invalidate(void);
int gguf_get_tensor(uint32_t start, uint32_t size, const char *name, gguf_tensor_t *tensor);
int gguf_get_metadata_array(uint32_t start, uint32_t size, const char *key, uint32_t *out_count, uint8_t **out_ptr);
int gguf_get_metadata_value(uint32_t start, uint32_t size, const char *key, uint32_t expected_type, void *out_val);

/* Optional high-memory (>4GB PAE) backing for a GGUF file's tensor DATA
 * section (everything from data_offset onward -- the header/metadata/vocab
 * section before it is never migrated and stays directly low-memory
 * readable). Single-slot, single-model: matches gguf_probe's own
 * (start,size)-keyed single-slot cache above, since MicroK only ever has one
 * model loaded at a time. */

/* Attempts to move [file_start+data_offset, file_start+file_size) into a
 * high-memory hbuf_t and reclaim the low-memory pages that held it. No-op
 * (returns 0, changes nothing) if there's no usable high-memory pool or the
 * data section is too small to bother with -- always safe to call. */
int gguf_migrate_data_to_high_mem(uint32_t file_start, uint32_t file_size, uint32_t data_offset);

/* Frees the high-memory backing for file_start, if it currently owns one.
 * Call before loading a new/different model so repeated loads don't leak
 * high-pool pages. Safe to call even if nothing was ever migrated. */
void gguf_release_high_mem_backing(uint32_t file_start);

/* Reads `len` bytes at tensor_offset (relative to data_offset, i.e. the same
 * value gguf_tensor_t.offset uses) into `out`. Transparently goes through
 * the high-memory backing if file_start was migrated, otherwise falls back
 * to the direct low-memory read every call site used before this existed --
 * callers never need to know which. Returns 1 on success. */
int gguf_read_data_bytes(uint32_t file_start, uint32_t file_size, uint32_t data_offset, uint32_t tensor_offset, void *out, uint32_t len);

#endif
