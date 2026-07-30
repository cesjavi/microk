#include "math.h"

uint64_t div_u64_u32(uint64_t numerator, uint32_t denominator) {
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    if (denominator == 0) {
        return 0;
    }

    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= (uint64_t)1 << bit;
        }
    }

    return quotient;
}

/**
 * @brief Divides two fixed-point numbers (Q16.16).
 */
fixed_t fixed_div(fixed_t a, fixed_t b) {
    int negative = 0;
    uint64_t numerator;
    uint32_t denominator;
    uint64_t quotient;

    if (b == 0) return 0;

    if (a < 0) {
        negative = !negative;
        numerator = (uint64_t)(-(int64_t)a);
    } else {
        numerator = (uint64_t)a;
    }

    if (b < 0) {
        negative = !negative;
        denominator = (uint32_t)(-(int64_t)b);
    } else {
        denominator = (uint32_t)b;
    }

    quotient = div_u64_u32(numerator << FIXED_SHIFT, denominator);
    return negative ? -(fixed_t)quotient : (fixed_t)quotient;
}

/**
 * @brief Computes the square root of a fixed-point number.
 */
fixed_t fixed_sqrt(fixed_t x) {
    if (x <= 0) return 0;
    
    fixed_t res = 0;
    fixed_t bit = 1 << 30;
    
    while (bit > x) bit >>= 2;
    
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res << (FIXED_SHIFT / 2);
}

/* Forward declaration: real definition lives further down next to
 * math_fast_log2f. Needed here because fixed_exp (used heavily by softmax
 * and SiLU/sigmoid in the transformer forward pass) must not use the old
 * 4-term Taylor series below -- that series only tracks exp(x) for
 * |x| <~ 2; for any softmax logit gap or SiLU input beyond that (routine
 * once real trained weights are involved, not just toy values), it not
 * only loses precision but can flip sign/magnitude entirely (e.g. the
 * truncated polynomial at x=-5 evaluates to +13.7, while exp(-5)=0.0067),
 * corrupting attention weighting and FFN gating throughout every layer. */
static float math_fast_pow2f(float p);

/**
 * @brief Approximates exp(x) via exp2(x * log2(e)) using a fast pow2
 * approximation (see math_fast_pow2f below), valid and reasonably
 * accurate across the whole practical range instead of only |x| <~ 2.
 *
 * fixed_t is Q16.16, so the largest safely representable magnitude is
 * ~32767. exp(10) ~= 22026 is the largest input that still fits; without
 * clamping, any upstream value that pushes x past ~11 (e.g. an
 * unnormalized softmax logit gap, or a still-undiagnosed bug elsewhere in
 * the forward pass) makes float_to_fixed() overflow the int32 conversion
 * and wrap to garbage (observed as h_sumsq printing as ~INT32_MIN).
 */
fixed_t fixed_exp(fixed_t x) {
    if (x == 0) return FIXED_ONE;
    float xf = fixed_to_float(x);
    if (xf > 10.0f) xf = 10.0f;
    if (xf < -10.0f) xf = -10.0f;
    float result = math_fast_pow2f(xf * 1.4426950409f); /* log2(e) */
    return float_to_fixed(result);
}

/**
 * @brief Approximates the hyperbolic tangent function tanh(x).
 */
fixed_t fixed_tanh(fixed_t x) {
    if (x > int_to_fixed(3)) return FIXED_ONE;
    if (x < -int_to_fixed(3)) return -FIXED_ONE;
    
    // tanh(x) approx using Taylor: x - x^3/3 + 2x^5/15
    fixed_t x2 = fixed_mul(x, x);
    fixed_t x3 = fixed_mul(x2, x);
    
    return x - x3 / 3;
}

/**
 * @brief Computes the sigmoid activation function: 1 / (1 + exp(-x)).
 */
fixed_t fixed_sigmoid(fixed_t x) {
    // 1 / (1 + exp(-x))
    fixed_t e = fixed_exp(-x);
    return fixed_div(FIXED_ONE, FIXED_ONE + e);
}

#define MATH_PI_F 3.14159265f
#define MATH_TWO_PI_F 6.28318531f

static float math_floorf(float x) {
    float t = (float)(int)x;
    if (t > x) t -= 1.0f;
    return t;
}

/* Reduce x to (-pi, pi] before applying the Bhaskara approximation below;
 * RoPE angles grow unboundedly with sequence position, so this range
 * reduction is required -- a plain low-order Taylor series (the previous
 * implementation) diverges badly past a few radians, which silently
 * corrupted attention for every token beyond the first few positions. */
static float math_wrap_pi(float x) {
    float k = math_floorf((x + MATH_PI_F) / MATH_TWO_PI_F);
    return x - k * MATH_TWO_PI_F;
}

/* Bhaskara I sine approximation, valid for x in [0, pi], max relative
 * error ~0.2%. Good enough for positional encoding purposes. */
static float math_bhaskara_sin(float x) {
    float num = 16.0f * x * (MATH_PI_F - x);
    float den = 5.0f * MATH_PI_F * MATH_PI_F - 4.0f * x * (MATH_PI_F - x);
    return num / den;
}

static float math_sinf(float x) {
    x = math_wrap_pi(x);
    if (x >= 0.0f) return math_bhaskara_sin(x);
    return -math_bhaskara_sin(-x);
}

static float math_cosf(float x) {
    return math_sinf(x + MATH_PI_F / 2.0f);
}

fixed_t fixed_sin(fixed_t x) {
    return float_to_fixed(math_sinf(fixed_to_float(x)));
}

fixed_t fixed_cos(fixed_t x) {
    return float_to_fixed(math_cosf(fixed_to_float(x)));
}

/* Fast log2/pow2 approximations (Ankerl-style), ~1-3% relative error.
 * Avoids needing libm in this freestanding kernel while still giving
 * fixed_pow real per-dimension RoPE frequencies instead of the previous
 * stub, which always returned 1.0 regardless of base/exponent -- collapsing
 * every RoPE dimension pair onto the same rotation frequency. */
static float math_fast_log2f(float x) {
    union { float f; uint32_t i; } vx = { .f = x };
    union { uint32_t i; float f; } mx = { .i = (vx.i & 0x007FFFFFu) | 0x3f000000u };
    float y = (float)vx.i * 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f - 1.72587999f / (0.3520887068f + mx.f);
}

static float math_fast_pow2f(float p) {
    float clipp = (p < -126.0f) ? -126.0f : p;
    int w = (int)clipp;
    float offset = (clipp < 0.0f) ? 1.0f : 0.0f;
    float z = clipp - (float)w + offset;
    union { uint32_t i; float f; } v = {
        .i = (uint32_t)((1 << 23) * (clipp + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z))
    };
    return v.f;
}

fixed_t fixed_pow(fixed_t base, fixed_t exp) {
    float base_f = fixed_to_float(base);
    float exp_f = fixed_to_float(exp);
    if (base_f <= 0.0f) return 0;
    float result = math_fast_pow2f(exp_f * math_fast_log2f(base_f));
    return float_to_fixed(result);
}
