#include "tensor.h"
#include "kheap.h"
#include "llm_gguf.h"
#include <string.h>

static char tensor_error_buf[96];

static void tensor_set_error(const char *msg) {
    memset(tensor_error_buf, 0, sizeof(tensor_error_buf));
    if (msg) {
        strncpy(tensor_error_buf, msg, sizeof(tensor_error_buf) - 1);
    }
}

const char *tensor_last_error(void) {
    return tensor_error_buf[0] ? tensor_error_buf : "unknown tensor error";
}

static float f16_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;
    uint32_t bits;

    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x0400) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000 | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }

    union {
        uint32_t u;
        float f;
    } conv;
    conv.u = bits;
    return conv.f;
}

/**
 * @brief Creates a new tensor with the specified dimensions.
 * Allocates memory for both the structure and the data.
 */
tensor_t *tensor_create(uint32_t ndims, uint32_t *dims) {
    tensor_t *t = kmalloc(sizeof(tensor_t));
    if (!t) {
        tensor_set_error("kmalloc failed for tensor struct.");
        return NULL;
    }
    
    t->ndims = ndims;
    t->count = 1;
    for (uint32_t i = 0; i < ndims; i++) {
        t->dims[i] = dims[i];
        t->count *= dims[i];
    }
    
    t->data = kmalloc(t->count * sizeof(fixed_t));
    if (!t->data) {
        tensor_set_error("kmalloc failed for tensor data.");
        kfree(t);
        return NULL;
    }
    t->is_owner = 1;
    memset(t->data, 0, t->count * sizeof(fixed_t));
    return t;
}

/**
 * @brief Frees all memory associated with a tensor.
 */
void tensor_free(tensor_t *t) {
    if (!t) return;
    if (t->is_owner && t->data) kfree(t->data);
    kfree(t);
}

/**
 * @brief De-quantizes Q4_0 blocks into fixed-point values.
 * Q4_0 block: 2 bytes (float16 delta) + 16 bytes (32 4-bit weights).
 */
void dequantize_q4_0(fixed_t *out, const uint8_t *in, uint32_t count) {
    for (uint32_t b = 0; b < count / 32; b++) {
        const uint8_t *block = in + b * 18;
        uint16_t delta_f16 = *(uint16_t *)block;
        
        // Proper f16 to fixed conversion
        fixed_t delta = float_to_fixed(f16_to_float(delta_f16)); 
        
        const uint8_t *qs = block + 2;
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t q0 = qs[i] & 0x0F;
            uint8_t q1 = qs[i] >> 4;
            
            out[b * 32 + i * 2 + 0] = fixed_mul(delta, int_to_fixed((int)q0 - 8));
            out[b * 32 + i * 2 + 1] = fixed_mul(delta, int_to_fixed((int)q1 - 8));
        }
    }
}

int tensor_read_gguf_row_into(uint32_t file_start, uint32_t file_size, const char *name, uint32_t row_index, fixed_t *out, uint32_t out_count) {
    gguf_tensor_t ginfo;
    gguf_info_t info;
    uint8_t *src;
    uint32_t row_width;
    uint32_t row_count;

    if (!out || !name) {
        tensor_set_error("invalid GGUF row read arguments.");
        return 0;
    }
    if (!gguf_get_tensor(file_start, file_size, name, &ginfo)) {
        tensor_set_error("tensor name not found in GGUF.");
        return 0;
    }
    if (!gguf_probe(file_start, file_size, &info)) {
        tensor_set_error("GGUF probe failed for row read.");
        return 0;
    }
    if (ginfo.ndims != 2) {
        tensor_set_error("GGUF row read requires 2D tensor.");
        return 0;
    }

    row_width = ginfo.dims[0];
    row_count = ginfo.dims[1];
    if (row_index >= row_count) {
        tensor_set_error("GGUF row index out of range.");
        return 0;
    }
    if (out_count < row_width) {
        tensor_set_error("GGUF row output buffer too small.");
        return 0;
    }

    src = (uint8_t *)(file_start + info.data_offset + ginfo.offset);
    if (ginfo.type == GGML_TYPE_F32) {
        float *fsrc = (float *)src + (row_index * row_width);
        for (uint32_t i = 0; i < row_width; i++) {
            out[i] = float_to_fixed(fsrc[i]);
        }
    } else if (ginfo.type == GGML_TYPE_F16) {
        uint16_t *hsrc = (uint16_t *)src + (row_index * row_width);
        for (uint32_t i = 0; i < row_width; i++) {
            out[i] = float_to_fixed(f16_to_float(hsrc[i]));
        }
    } else if (ginfo.type == GGML_TYPE_Q4_0) {
        uint32_t blocks_per_row;
        uint8_t *row_src;

        if ((row_width % 32) != 0) {
            tensor_set_error("Q4_0 row width must be multiple of 32.");
            return 0;
        }
        blocks_per_row = row_width / 32;
        row_src = src + (row_index * blocks_per_row * 18);
        dequantize_q4_0(out, row_src, row_width);
    } else {
        tensor_set_error("GGUF tensor type not supported.");
        return 0;
    }

    tensor_set_error("");
    return 1;
}

tensor_t *tensor_load_gguf_view(uint32_t file_start, uint32_t file_size, const char *name) {
    gguf_tensor_t ginfo;
    if (!gguf_get_tensor(file_start, file_size, name, &ginfo)) {
        tensor_set_error("tensor name not found in GGUF.");
        return NULL;
    }
    
    tensor_t *t = kmalloc(sizeof(tensor_t));
    if (!t) {
        tensor_set_error("kmalloc failed for view struct.");
        return NULL;
    }
    
    t->ndims = ginfo.ndims;
    t->count = 1;
    for (uint32_t i = 0; i < ginfo.ndims; i++) {
        t->dims[i] = ginfo.dims[i];
        t->count *= ginfo.dims[i];
    }
    
    t->data = NULL;
    t->is_owner = 0;
    t->gguf_offset = ginfo.offset;
    
    tensor_set_error("");
    return t;
}

/**
 * @brief Loads a specific tensor from a GGUF file and converts it to fixed-point.
 * Automatically handles data alignment and type conversion (FLOAT32 or Q4_0 to fixed_t).
 */
tensor_t *tensor_load_gguf(uint32_t file_start, uint32_t file_size, const char *name) {
    gguf_tensor_t ginfo;
    if (!gguf_get_tensor(file_start, file_size, name, &ginfo)) {
        tensor_set_error("tensor name not found in GGUF.");
        return NULL;
    }
    
    tensor_t *t = tensor_create(ginfo.ndims, ginfo.dims);
    if (!t) return NULL;
    t->gguf_offset = ginfo.offset;
    
    gguf_info_t info;
    gguf_probe(file_start, file_size, &info);
    
    uint8_t *src = (uint8_t *)(file_start + info.data_offset + ginfo.offset);
    
    if (ginfo.type == GGML_TYPE_F32) {
        float *fsrc = (float *)src;
        for (uint32_t i = 0; i < t->count; i++) {
            t->data[i] = float_to_fixed(fsrc[i]);
        }
    } else if (ginfo.type == GGML_TYPE_F16) {
        uint16_t *hsrc = (uint16_t *)src;
        for (uint32_t i = 0; i < t->count; i++) {
            t->data[i] = float_to_fixed(f16_to_float(hsrc[i]));
        }
    } else if (ginfo.type == GGML_TYPE_Q4_0) {
        dequantize_q4_0(t->data, src, t->count);
    } else {
        tensor_set_error("GGUF tensor type not supported.");
        tensor_free(t);
        return NULL;
    }

    tensor_set_error("");
    return t;
}

/**
 * @brief Performs matrix multiplication: out = a * b.
 * Matrix A: M x K, Matrix B: K x N, Output: M x N.
 */
int tensor_matmul(tensor_t *out, tensor_t *a, tensor_t *b) {
    if (!out || !a || !b) return 0;
    // a (M x K), b (K x N) -> out (M x N)
    uint32_t M = a->dims[0];
    uint32_t K = a->dims[1];
    uint32_t N = b->dims[1];

    if (a->dims[1] != b->dims[0] || out->dims[0] != M || out->dims[1] != N) {
        return 0;
    }

    for (uint32_t i = 0; i < M; i++) {
        for (uint32_t j = 0; j < N; j++) {
            fixed_t sum = 0;
            for (uint32_t k = 0; k < K; k++) {
                sum += fixed_mul(a->data[i * K + k], b->data[k * N + j]);
            }
            out->data[i * N + j] = sum;
        }
    }
    return 1;
}

/**
 * @brief Performs element-wise addition: out = a + b.
 */
int tensor_add(tensor_t *out, tensor_t *a, tensor_t *b) {
    if (!out || !a || !b || a->count != b->count || out->count != a->count) return 0;
    for (uint32_t i = 0; i < a->count; i++) {
        out->data[i] = a->data[i] + b->data[i];
    }
    return 1;
}

/**
 * @brief Applies an activation function (ReLU, Sigmoid, Tanh) to all elements in a tensor.
 */
void tensor_apply_activation(tensor_t *t, activation_t act) {
    if (!t) return;
    for (uint32_t i = 0; i < t->count; i++) {
        if (act == ACT_RELU) {
            if (t->data[i] < 0) t->data[i] = 0;
        } else if (act == ACT_SIGMOID) {
            t->data[i] = fixed_sigmoid(t->data[i]);
        } else if (act == ACT_TANH) {
            t->data[i] = fixed_tanh(t->data[i]);
        } else if (act == ACT_SILU) {
            t->data[i] = fixed_mul(t->data[i], fixed_sigmoid(t->data[i]));
        }
    }
}

int tensor_rmsnorm(tensor_t *out, tensor_t *in, tensor_t *weight, fixed_t epsilon) {
    if (!out || !in || !weight || in->count != out->count) return 0;
    
    // RMS = sqrt(mean(x^2) + eps)
    int64_t sum_sq = 0;
    for (uint32_t i = 0; i < in->count; i++) {
        fixed_t val = in->data[i];
        sum_sq += (int64_t)fixed_mul(val, val);
    }
    
    fixed_t mean_sq = (fixed_t)div_u64_u32(sum_sq, in->count);
    fixed_t rms = fixed_sqrt(mean_sq + epsilon);
    if (rms == 0) rms = 1; // Safety fallback
    fixed_t inv_rms = fixed_div(FIXED_ONE, rms);
    
    for (uint32_t i = 0; i < in->count; i++) {
        out->data[i] = fixed_mul(fixed_mul(in->data[i], inv_rms), weight->data[i]);
    }
    return 1;
}

int tensor_softmax(tensor_t *t) {
    if (!t || t->count == 0) return 0;
    
    // Find max for numerical stability
    fixed_t max_val = t->data[0];
    for (uint32_t i = 1; i < t->count; i++) {
        if (t->data[i] > max_val) max_val = t->data[i];
    }
    
    fixed_t sum = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        t->data[i] = fixed_exp(t->data[i] - max_val);
        sum += t->data[i];
    }
    
    if (sum == 0) return 0;
    fixed_t inv_sum = fixed_div(FIXED_ONE, sum);
    for (uint32_t i = 0; i < t->count; i++) {
        t->data[i] = fixed_mul(t->data[i], inv_sum);
    }
    return 1;
}

int tensor_rope(tensor_t *t, uint32_t pos, uint32_t n_head, uint32_t head_dim) {
    if (!t) return 0;
    
    for (uint32_t h = 0; h < n_head; h++) {
        for (uint32_t i = 0; i < head_dim / 2; i++) {
            // Basic RoPE: rotate pairs
            fixed_t theta = fixed_div(FIXED_ONE, fixed_pow(int_to_fixed(10000), float_to_fixed((float)(2 * i) / (float)head_dim)));
            fixed_t f_theta = fixed_mul(theta, int_to_fixed(pos));
            fixed_t cos_val = fixed_cos(f_theta);
            fixed_t sin_val = fixed_sin(f_theta);
            
            uint32_t idx1 = h * head_dim + i;
            uint32_t idx2 = h * head_dim + i + head_dim / 2;
            
            fixed_t x1 = t->data[idx1];
            fixed_t x2 = t->data[idx2];
            
            t->data[idx1] = fixed_mul(x1, cos_val) - fixed_mul(x2, sin_val);
            t->data[idx2] = fixed_mul(x1, sin_val) + fixed_mul(x2, cos_val);
        }
    }
    return 1;
}
