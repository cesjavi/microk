#ifndef MATH_H
#define MATH_H

#include <stdint.h>

typedef int32_t fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FIXED_HALF (1 << (FIXED_SHIFT - 1))

#define float_to_fixed(f) ((fixed_t)((f) * FIXED_ONE))
#define fixed_to_float(x) ((float)(x) / FIXED_ONE)
#define int_to_fixed(i) ((fixed_t)(i) << FIXED_SHIFT)
#define fixed_to_int(x) ((x) >> FIXED_SHIFT)

static inline fixed_t fixed_mul(fixed_t a, fixed_t b) {
    fixed_t res;
    __asm__ (
        "imull %%edx;"
        "shrdl $16, %%edx, %%eax;"
        : "=a" (res)
        : "a" (a), "d" (b)
        : "cc"
    );
    return res;
}
fixed_t fixed_div(fixed_t a, fixed_t b);

// Common math functions for tensors
fixed_t fixed_sqrt(fixed_t x);
fixed_t fixed_exp(fixed_t x);
fixed_t fixed_tanh(fixed_t x);
fixed_t fixed_sigmoid(fixed_t x);
fixed_t fixed_sin(fixed_t x);
fixed_t fixed_cos(fixed_t x);
fixed_t fixed_pow(fixed_t base, fixed_t exp);

uint64_t div_u64_u32(uint64_t numerator, uint32_t denominator);

#endif
