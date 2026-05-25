#ifndef TENSOR_H
#define TENSOR_H

#include <stdint.h>
#include "../lib/math.h"

typedef struct {
    uint32_t dims[4];
    uint32_t ndims;
    uint32_t count;
    fixed_t *data;
    int is_owner; // If 1, we should kfree data
    uint32_t gguf_offset; // Offset in GGUF file (if applicable)
} tensor_t;

tensor_t *tensor_create(uint32_t ndims, uint32_t *dims);
void tensor_free(tensor_t *t);
uint32_t tensor_get_count(tensor_t *t);
const char *tensor_last_error(void);

// GGUF Integration
tensor_t *tensor_load_gguf(uint32_t file_start, uint32_t file_size, const char *name);
tensor_t *tensor_load_gguf_view(uint32_t file_start, uint32_t file_size, const char *name);
int tensor_read_gguf_row_into(uint32_t file_start, uint32_t file_size, const char *name, uint32_t row_index, fixed_t *out, uint32_t out_count);
void dequantize_q4_0(fixed_t *out, const uint8_t *in, uint32_t count);

typedef enum {
    ACT_RELU,
    ACT_SIGMOID,
    ACT_TANH,
    ACT_SILU
} activation_t;

// Tensor Operations
int tensor_matmul(tensor_t *out, tensor_t *a, tensor_t *b);
int tensor_add(tensor_t *out, tensor_t *a, tensor_t *b);
void tensor_apply_activation(tensor_t *t, activation_t act);
int tensor_rmsnorm(tensor_t *out, tensor_t *in, tensor_t *weight, fixed_t epsilon);
int tensor_softmax(tensor_t *t);
int tensor_rope(tensor_t *t, uint32_t pos, uint32_t n_head, uint32_t head_dim);

#endif
