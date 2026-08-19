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
        if (dims[i] > 0 && t->count > 0xFFFFFFFFU / dims[i]) {
            tensor_set_error("tensor dimension product overflows uint32_t.");
            kfree(t);
            return NULL;
        }
        t->count *= dims[i];
    }
    if (t->count == 0) {
        tensor_set_error("tensor has zero elements.");
        kfree(t);
        return NULL;
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
        /* ggml Q4_0 packing: byte i holds elements i (low nibble) and i+16
         * (high nibble) of the 32-element block -- a split-half layout, not
         * sequential pairs. Verified against ggml's reference
         * quantize_row_q4_0/dequantize_row_q4_0. Getting this wrong doesn't
         * just rescale the result: it dot-products each weight against the
         * wrong input dimension, so instead of the coherent sum a correct
         * projection produces, unrelated terms partially cancel out --
         * exactly the ~100-600x energy deficit seen when comparing MicroK's
         * Q4_0 projections against a llama.cpp reference on the same
         * weights (see ROADMAP.md, TinyLlama coherence investigation). */
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t q0 = qs[i] & 0x0F;
            uint8_t q1 = qs[i] >> 4;

            out[b * 32 + i]      = fixed_mul(delta, int_to_fixed((int)q0 - 8));
            out[b * 32 + i + 16] = fixed_mul(delta, int_to_fixed((int)q1 - 8));
        }
    }
}

/**
 * @brief De-quantizes Q6_K super-blocks into fixed-point values.
 * Q6_K block (210 bytes, 256 elements): 128 bytes low-4-bit quants (ql),
 * 64 bytes high-2-bit quants (qh), 16 bytes int8 per-16-element sub-scales,
 * 2 bytes f16 super-block scale (d). Legacy llama.cpp quantizations (e.g.
 * plain "Q4_0" files) commonly keep output.weight at Q6_K for quality even
 * though every other tensor is Q4_0 -- e.g. TinyLlama-1.1B-Chat's
 * tinyllama-1.1b-chat-v1.0.Q4_0.gguf. Mirrors ggml's reference
 * dequantize_row_q6_K bit-for-bit, just emitting fixed_t instead of float.
 */
void dequantize_q6_k(fixed_t *out, const uint8_t *in, uint32_t count) {
    for (uint32_t b = 0; b < count / 256; b++) {
        const uint8_t *block = in + b * 210;
        const uint8_t *ql = block;
        const uint8_t *qh = block + 128;
        const int8_t *sc = (const int8_t *)(block + 192);
        uint16_t d_f16 = *(const uint16_t *)(block + 208);
        fixed_t d = float_to_fixed(f16_to_float(d_f16));
        fixed_t *y = out + (size_t)b * 256;

        for (int n = 0; n < 256; n += 128) {
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                int8_t q1 = (int8_t)((ql[l +  0] & 0x0F) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int8_t q2 = (int8_t)((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int8_t q3 = (int8_t)((ql[l +  0] >>   4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int8_t q4 = (int8_t)((ql[l + 32] >>   4) | (((qh[l] >> 6) & 3) << 4)) - 32;

                y[l +  0] = fixed_mul(fixed_mul(d, int_to_fixed(sc[is + 0])), int_to_fixed(q1));
                y[l + 32] = fixed_mul(fixed_mul(d, int_to_fixed(sc[is + 2])), int_to_fixed(q2));
                y[l + 64] = fixed_mul(fixed_mul(d, int_to_fixed(sc[is + 4])), int_to_fixed(q3));
                y[l + 96] = fixed_mul(fixed_mul(d, int_to_fixed(sc[is + 6])), int_to_fixed(q4));
            }
            y  += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
}

/* Reused row-scratch buffer, grown lazily and kept across calls instead of
 * malloc/free per row -- tensor_read_gguf_row_into() is the fallback path
 * for the argmax vocab scan in kernel/llm.c when the output tensor isn't
 * Q4_0/Q6_K, so it can run once per candidate token (tens of thousands of
 * times per generated token); a fresh kmalloc/kfree pair on every call was
 * a real, measured slowdown once this started going through
 * gguf_read_data_bytes() instead of a bare pointer. */
static uint8_t *tensor_row_scratch = NULL;
static uint32_t tensor_row_scratch_cap = 0;

static uint8_t *tensor_row_scratch_ensure(uint32_t needed) {
    if (needed <= tensor_row_scratch_cap) return tensor_row_scratch;
    uint8_t *grown = kmalloc(needed);
    if (!grown) return NULL;
    if (tensor_row_scratch) kfree(tensor_row_scratch);
    tensor_row_scratch = grown;
    tensor_row_scratch_cap = needed;
    return tensor_row_scratch;
}

int tensor_read_gguf_row_into(uint32_t file_start, uint32_t file_size, const char *name, uint32_t row_index, fixed_t *out, uint32_t out_count) {
    gguf_tensor_t ginfo;
    gguf_info_t info;
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

    /* Row bytes are read through gguf_read_data_bytes() (a scratch copy)
     * instead of a direct pointer, so this works whether the model's tensor
     * DATA section is still low-memory-resident or was migrated to high
     * memory -- see kernel/llm_gguf.c. */
    uint32_t row_byte_offset;
    uint32_t row_byte_len;
    if (ginfo.type == GGML_TYPE_F32) {
        row_byte_len = row_width * 4;
        row_byte_offset = ginfo.offset + row_index * row_byte_len;
    } else if (ginfo.type == GGML_TYPE_F16) {
        row_byte_len = row_width * 2;
        row_byte_offset = ginfo.offset + row_index * row_byte_len;
    } else if (ginfo.type == GGML_TYPE_Q4_0) {
        if ((row_width % 32) != 0) {
            tensor_set_error("Q4_0 row width must be multiple of 32.");
            return 0;
        }
        row_byte_len = (row_width / 32) * 18;
        row_byte_offset = ginfo.offset + row_index * row_byte_len;
    } else if (ginfo.type == GGML_TYPE_Q6_K) {
        if ((row_width % 256) != 0) {
            tensor_set_error("Q6_K row width must be multiple of 256.");
            return 0;
        }
        row_byte_len = (row_width / 256) * 210;
        row_byte_offset = ginfo.offset + row_index * row_byte_len;
    } else {
        tensor_set_error("GGUF tensor type not supported.");
        return 0;
    }

    uint8_t *row_buf = tensor_row_scratch_ensure(row_byte_len);
    if (!row_buf) {
        tensor_set_error("kmalloc failed for GGUF row scratch copy.");
        return 0;
    }
    if (!gguf_read_data_bytes(file_start, file_size, info.data_offset, row_byte_offset, row_buf, row_byte_len)) {
        tensor_set_error("GGUF row data read failed.");
        return 0;
    }

    if (ginfo.type == GGML_TYPE_F32) {
        float *fsrc = (float *)row_buf;
        for (uint32_t i = 0; i < row_width; i++) {
            out[i] = float_to_fixed(fsrc[i]);
        }
    } else if (ginfo.type == GGML_TYPE_F16) {
        uint16_t *hsrc = (uint16_t *)row_buf;
        for (uint32_t i = 0; i < row_width; i++) {
            out[i] = float_to_fixed(f16_to_float(hsrc[i]));
        }
    } else if (ginfo.type == GGML_TYPE_Q4_0) {
        dequantize_q4_0(out, row_buf, row_width);
    } else if (ginfo.type == GGML_TYPE_Q6_K) {
        dequantize_q6_k(out, row_buf, row_width);
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
    t->gguf_type = ginfo.type;

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
    t->gguf_type = ginfo.type;

    gguf_info_t info;
    gguf_probe(file_start, file_size, &info);

    /* tensor_load_gguf() is only ever used for small tensors (norm weights:
     * a few KB each), so a one-shot scratch copy is cheap. This indirection
     * (instead of a direct pointer into the file) is what lets the model's
     * tensor DATA section live in high memory -- see
     * gguf_read_data_bytes()/gguf_migrate_data_to_high_mem() in
     * kernel/llm_gguf.c. */
    uint32_t byte_len;
    if (ginfo.type == GGML_TYPE_F32) {
        byte_len = t->count * 4;
    } else if (ginfo.type == GGML_TYPE_F16) {
        byte_len = t->count * 2;
    } else if (ginfo.type == GGML_TYPE_Q4_0) {
        byte_len = (t->count / 32) * 18;
    } else if (ginfo.type == GGML_TYPE_Q6_K) {
        byte_len = (t->count / 256) * 210;
    } else {
        tensor_set_error("GGUF tensor type not supported.");
        tensor_free(t);
        return NULL;
    }

    uint8_t *src = kmalloc(byte_len);
    if (!src) {
        tensor_set_error("kmalloc failed for GGUF tensor scratch copy.");
        tensor_free(t);
        return NULL;
    }
    if (!gguf_read_data_bytes(file_start, file_size, info.data_offset, ginfo.offset, src, byte_len)) {
        tensor_set_error("GGUF tensor data read failed.");
        kfree(src);
        tensor_free(t);
        return NULL;
    }

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
    } else if (ginfo.type == GGML_TYPE_Q6_K) {
        dequantize_q6_k(t->data, src, t->count);
    }

    kfree(src);
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

int tensor_rope(tensor_t *t, uint32_t pos, uint32_t n_head, uint32_t head_dim, float freq_base) {
    if (!t) return 0;

    fixed_t base_fixed = float_to_fixed(freq_base);

    for (uint32_t h = 0; h < n_head; h++) {
        for (uint32_t i = 0; i < head_dim / 2; i++) {
            /* GGUF/GGML weights for llama2.c-style checkpoints (e.g. stories15M)
             * are laid out for the "original" RoPE convention, which rotates
             * CONSECUTIVE dimension pairs (2i, 2i+1) within each head. The
             * split-half convention (i, i+head_dim/2) is HuggingFace's
             * rotate_half style, which only matches when the Q/K weights were
             * permuted during HF conversion -- these weren't, since they were
             * loaded straight from the GGUF tensors. Using the wrong pairing
             * scrambles attention more and more as position grows, producing
             * a few sane words followed by garbled subword salad. */
            fixed_t theta = fixed_div(FIXED_ONE, fixed_pow(base_fixed, float_to_fixed((float)(2 * i) / (float)head_dim)));
            fixed_t f_theta = fixed_mul(theta, int_to_fixed(pos));
            fixed_t cos_val = fixed_cos(f_theta);
            fixed_t sin_val = fixed_sin(f_theta);

            uint32_t idx1 = h * head_dim + 2 * i;
            uint32_t idx2 = h * head_dim + 2 * i + 1;

            fixed_t x1 = t->data[idx1];
            fixed_t x2 = t->data[idx2];

            t->data[idx1] = fixed_mul(x1, cos_val) - fixed_mul(x2, sin_val);
            t->data[idx2] = fixed_mul(x1, sin_val) + fixed_mul(x2, cos_val);
        }
    }
    return 1;
}
