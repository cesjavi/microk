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
int gguf_get_tensor(uint32_t start, uint32_t size, const char *name, gguf_tensor_t *tensor);
int gguf_get_metadata_array(uint32_t start, uint32_t size, const char *key, uint32_t *out_count, uint8_t **out_ptr);
int gguf_get_metadata_value(uint32_t start, uint32_t size, const char *key, uint32_t expected_type, void *out_val);

#endif
