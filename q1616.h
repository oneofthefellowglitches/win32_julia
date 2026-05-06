#ifndef Q1616_H
#define Q1616_H

#include "system.h"

/* -------------------------------------------------------------------------
   1. TYPE DEFINITIONS & CONSTANTS
   ------------------------------------------------------------------------- */
/**
 * @brief 32-bit fixed-point type with 16 bits of integer and 16 bits of fraction.
 */
#define Q1616_ONE_VAL        (1L << 16)
#define Q1616_HALF_VAL       (1L << 15)
#define Q1616_LN2_CONST      ((q1616_t)45426) /* 0.693147 * 65536 */

/* -------------------------------------------------------------------------
   2. CONVERSION MACROS
   ------------------------------------------------------------------------- */
#define Q1616_FROM_INT(x)    ((q1616_t)((x) << 16))
#define Q1616_TO_INT(x)      ((x) >> 16)

#define Q1616_FROM_FLOAT(x)  ((q1616_t)((x) * (float)Q1616_ONE_VAL + 0.5f))
#define Q1616_TO_FLOAT(x)    ((float)(x) / (float)Q1616_ONE_VAL)

#define Q1616_FROM_DOUBLE(x) ((q1616_t)((x) * 65536.0))
#define Q1616_TO_DOUBLE(x)   ((double)(x) / 65536.0)
/* -------------------------------------------------------------------------
   3. BASIC ARITHMETIC
   ------------------------------------------------------------------------- */

SYSTEM_FORCEINLINE q1616_t q1616_add(q1616_t x, q1616_t y) {
    return x + y;
}

SYSTEM_FORCEINLINE q1616_t q1616_sub(q1616_t x, q1616_t y) {
    return x - y;
}

/**
 * @brief Multiplies two q1616_t values. 
 * Uses 64-bit intermediate to prevent overflow.
 */
SYSTEM_FORCEINLINE q1616_t q1616_mul(q1616_t x, q1616_t y) {
    int64_t res = (int64_t)x * (int64_t)y;
    return (q1616_t)(res >> 16);
}

/**
 * @brief Divides two q1616_t values.
 * Uses 64-bit intermediate for precision.
 */
SYSTEM_FORCEINLINE q1616_t q1616_div(q1616_t x, q1616_t y) {
    if (y == 0) return 0;
    return (q1616_t)(((int64_t)x << 16) / (int64_t)y);
}

SYSTEM_FORCEINLINE __m128i q1616_mul4(__m128i a, __m128i b) {
    #if 0
    __m128i a_hi;
    __m128i b_hi;
    __m128i prod_lo, prod_hi;
    __m128i res_lo, res_hi;

    /* even lanes: a0*b0, a2*b2 */
    prod_lo = _mm_mul_epu32(a, b);

    /* shift to get odd lanes into even positions */
    a_hi = _mm_srli_si128(a, 4);
    b_hi = _mm_srli_si128(b, 4);

    /* odd lanes: a1*b1, a3*b3 */
    prod_hi = _mm_mul_epu32(a_hi, b_hi);

    /* >> 16 to convert back to Q16.16 */
    prod_lo = _mm_srli_epi64(prod_lo, 16);
    prod_hi = _mm_srli_epi64(prod_hi, 16);

    /* pack back into 32-bit lanes */
    res_lo = _mm_shuffle_epi32(prod_lo, _MM_SHUFFLE(0,0,2,0));
    res_hi = _mm_shuffle_epi32(prod_hi, _MM_SHUFFLE(0,0,2,0));

    /* interleave even/odd results */
    return _mm_unpacklo_epi32(res_lo, res_hi);
    #endif

    /* even lanes: a0*b0, a2*b2 */
    __m128i prod_even_q32_32 = _mm_mul_epi32(a, b);

    /* odd lanes: shift inputs so 1,3 become 0,2 */
    __m128i a_odd = _mm_srli_epi64(a, 32);
    __m128i b_odd = _mm_srli_epi64(b, 32);
    __m128i prod_odd_q32_32 = _mm_mul_epi32(a_odd, b_odd);

    /* Q32.32 -> Q16.16 */
    prod_even_q32_32 = _mm_srli_epi64(prod_even_q32_32, 16);
    prod_odd_q32_32  = _mm_srli_epi64(prod_odd_q32_32,  16);

    /* extract low 32 bits from each 64-bit lane */
    __m128i even_q16_16 =
        _mm_shuffle_epi32(prod_even_q32_32, _MM_SHUFFLE(0,0,2,0));
    __m128i odd_q16_16  =
        _mm_shuffle_epi32(prod_odd_q32_32,  _MM_SHUFFLE(0,0,2,0));

    /* interleave: [e0 o0 e1 o1] */
    return _mm_unpacklo_epi32(even_q16_16, odd_q16_16);
}

/** 
 * @brief Helper for robust Q16.16 Fixed Point SIMD Multiplication.
 * a way to handle the lack of a 32-bit integer 
 * "multiply-and-keep-middle-bits" instruction in AVX2. 
 * Since _mm256_mul_epi32 interprets data as 64-bit chunks 
 * (only using the lower 32 bits of each), "even/odd" shuffle provided
 * is exactly the standard way to simulate a full 32-bit SIMD multiply
 * Note: C89 uses 'static' or '__inline' (compiler specific). 
 */
SYSTEM_FORCEINLINE __m256i q1616_mul8(__m256i a, __m256i b) {
    #if 0
    __m256i even, a_odd, b_odd, odd;

    /* Even lanes: 0, 2, 4, 6 */
    even = _mm256_mul_epi32(a, b);
    even = _mm256_srli_epi64(even, 16); 

    /* Odd lanes: 1, 3, 5, 7 */
    a_odd = _mm256_srli_epi64(a, 32);
    b_odd = _mm256_srli_epi64(b, 32);
    odd   = _mm256_mul_epi32(a_odd, b_odd);

    /* Align odd results to the high 32 bits of the 64-bit lanes */
    odd = _mm256_srli_epi64(odd, 16);
    odd = _mm256_slli_epi64(odd, 32);

    /* 0xAA = 10101010: selects odd lanes from 'odd', even from 'even' */
    return _mm256_blend_epi32(even, odd, 0xAA);
    #endif

    /* even lanes: 0,2,4,6 */
    __m256i prod_even_q32_32 = _mm256_mul_epi32(a, b);
    prod_even_q32_32 = _mm256_srli_epi64(prod_even_q32_32, 16);

    /* odd lanes: 1,3,5,7 */
    __m256i a_odd = _mm256_srli_epi64(a, 32);
    __m256i b_odd = _mm256_srli_epi64(b, 32);
    __m256i prod_odd_q32_32 = _mm256_mul_epi32(a_odd, b_odd);
    prod_odd_q32_32 = _mm256_srli_epi64(prod_odd_q32_32, 16);
    prod_odd_q32_32 = _mm256_slli_epi64(prod_odd_q32_32, 32);

    /* blend odd into odd lanes */
    return _mm256_blend_epi32(prod_even_q32_32, prod_odd_q32_32, 0xAA);
}

/**
 * @brief SIMD Q16.16 Division for 8 lanes (AVX2).
 * Newton-Raphson Reciprocal
 * To divide in SIMD, we don't actually "divide." 
 * Instead, convert our fixed-point integers to floating point.
 * Calculate the reciprocal (1/b) using the hardware's fast approximation.
 * Multiply a x (1/b). Convert back to fixed-point.
 * Logic: result = (a / b). 
 * For fixed point: (a << 16) / b.
 */
SYSTEM_FORCEINLINE __m256i q1616_div8(__m256i a, __m256i b) {
    #if 0
    __m256 float_a; 
    __m256 float_b; 
    __m256 float_res;
    const float scale = 65536.0f; /* 2^16 */

    /* 1. Convert 8 integers to 8 floats. 
       _mm256_cvtepi32_ps: "Convert Packed Integer 32 to Packed Single-float" */
    float_a = _mm256_cvtepi32_ps(a);
    float_b = _mm256_cvtepi32_ps(b);

    /* 2. Compute reciprocal: 1.0 / float_b.
       _mm256_rcp_ps is an approximation (12-bit precision).
       For Julia sets, this is often 'good enough', but we use a real divide 
       here for 24-bit precision since Mandelbrot/Julia is sensitive. */
    float_res = _mm256_div_ps(float_a, float_b);

    /* 3. Scale the result back into fixed point (multiply by 65536) */
    float_res = _mm256_mul_ps(float_res, _mm256_set1_ps(scale));

    /* 4. Convert back to 32-bit integers with truncation.
       _mm256_cvttps_epi32: "Convert with Truncation Packed Single-float to epi32" */
    return _mm256_cvttps_epi32(float_res);
    #endif

    const __m256 scale = _mm256_set1_ps(65536.0f);

    __m256 fa = _mm256_cvtepi32_ps(a);
    __m256 fb = _mm256_cvtepi32_ps(b);

    __m256 f_res = _mm256_div_ps(fa, fb);
    f_res = _mm256_mul_ps(f_res, scale);

    return _mm256_cvttps_epi32(f_res);
}

SYSTEM_FORCEINLINE __m512i q1616_mul16(__m512i a, __m512i b) {
    #if 0
    __m512i prod_even, prod_odd;
    __m512i a_odd, b_odd;
    __m512i even32, odd32;
    __m512i result;

    /* even lanes: a0*b0, a2*b2, ... */
    prod_even = _mm512_mul_epi32(a, b);

    /* odd lanes */
    a_odd = _mm512_srli_epi64(a, 32);
    b_odd = _mm512_srli_epi64(b, 32);
    prod_odd = _mm512_mul_epi32(a_odd, b_odd);

    /* shift Q32.32 -> Q16.16 */
    prod_even = _mm512_srli_epi64(prod_even, 16);
    prod_odd  = _mm512_srli_epi64(prod_odd, 16);

    /* narrow to int32 (returns __m256i!) */
    even32 = _mm512_castsi256_si512(
                 _mm512_cvtepi64_epi32(prod_even));
    odd32  = _mm512_castsi256_si512(
                 _mm512_cvtepi64_epi32(prod_odd));

    /* interleave even / odd lanes */
    result = _mm512_mask_blend_epi32(0xAAAA,even32,odd32);

    return result;
    #endif

    /* 1) Multiply even lanes: a0*b0, a2*b2, ... */
    __m512i prod_even_q32_32 = _mm512_mul_epi32(a, b);

    /* 2) Multiply odd lanes */
    __m512i a_odd = _mm512_srli_epi64(a, 32);
    __m512i b_odd = _mm512_srli_epi64(b, 32);
    __m512i prod_odd_q32_32 = _mm512_mul_epi32(a_odd, b_odd);

    /* 3) Q32.32 → Q16.16 */
    prod_even_q32_32 = _mm512_srli_epi64(prod_even_q32_32, 16);
    prod_odd_q32_32  = _mm512_srli_epi64(prod_odd_q32_32,  16);

    /* 4) Narrow 64 → 32 (returns __m256i!) */
    __m256i even32 = _mm512_cvtepi64_epi32(prod_even_q32_32);
    __m256i odd32  = _mm512_cvtepi64_epi32(prod_odd_q32_32);

    /* 5) Expand to 512-bit registers */
    __m512i even512 = _mm512_castsi256_si512(even32);
    __m512i odd512  = _mm512_castsi256_si512(odd32);

    /* 6) Interleave even / odd lanes */
    return _mm512_mask_blend_epi32(0xAAAA, even512, odd512);
}


/* -------------------------------------------------------------------------
   4. LOGARITHMIC IMPLEMENTATIONS (Taylor Series)
   ------------------------------------------------------------------------- */

/**
 * @brief Natural logarithm ln(x) using a 4-term Taylor series via Horner's Method.
 * Balanced for speed in cases where high precision is less critical than throughput.
 */
SYSTEM_FORCEINLINE q1616_t q1616_ln_taylor_4term(q1616_t x) {
    int exp;
    q1616_t y, poly, ln_m, ln_e;
    
    /* Pre-calculated Taylor coefficients */
    const q1616_t c_half   = Q1616_FROM_FLOAT(0.5f);
    const q1616_t c_third  = Q1616_FROM_FLOAT(0.333333f);
    const q1616_t c_fourth = Q1616_FROM_FLOAT(0.25f);

    /* Handle non-positive input: ln(x <= 0) is undefined */
    if (x <= 0) {
        return Q1616_FROM_FLOAT(-30.0f);
    }

    /* --- 1. Range Reduction --- */
    /* Normalize x to the range [1, 2) and extract the binary exponent */
    exp = 0;
    while (x < Q1616_ONE_VAL) { 
        x <<= 1; 
        --exp; 
    }
    while (x >= (Q1616_ONE_VAL << 1)) { 
        x >>= 1; 
        ++exp; 
    }

    /* Normalized y is in range [0, 1] representing the mantissa fraction */
    y = x - Q1616_ONE_VAL;

    /* --- 2. Horner's Evaluation --- */
    /* Mathematical form: y * (1 - y * (1/2 - y * (1/3 - y/4))) 
       This nesting significantly reduces rounding errors in fixed-point. */
    /* Start with innermost term: y/4 */
    poly = c_fourth;
    
    /* Step 1: (1/3 - y * 1/4) */    
    poly = q1616_sub(c_third, q1616_mul(y, poly));

    /* Step 2: (1/2 - y * Step1) */
    poly = q1616_sub(c_half, q1616_mul(y, poly));

    /* Step 3: ln_mantissa = y * (1 - y * Step2) */
    ln_m = q1616_mul(y, q1616_sub(Q1616_ONE_VAL, q1616_mul(y, poly)));

    /* --- 3. Reconstruction --- */
    /* Final result: ln(x) = ln(mantissa) + exponent * ln(2) */
    ln_e = q1616_mul(Q1616_FROM_INT(exp), Q1616_LN2_CONST);

    return q1616_add(ln_m, ln_e);
}

/**
 * @brief ln(x) using Horner's Method with 8 terms.
 * High precision, recommended for final color mapping.
 */
SYSTEM_FORCEINLINE q1616_t q1616_ln_slow(q1616_t x) {
    int exp;
    q1616_t y, poly, ln_m, ln_e;
    const q1616_t c_0500 = Q1616_FROM_FLOAT(0.5f);
    const q1616_t c_0333 = Q1616_FROM_FLOAT(0.333333f);
    const q1616_t c_0250 = Q1616_FROM_FLOAT(0.25f);
    const q1616_t c_0200 = Q1616_FROM_FLOAT(0.2f);
    const q1616_t c_0166 = Q1616_FROM_FLOAT(0.166667f);
    const q1616_t c_0142 = Q1616_FROM_FLOAT(0.142857f);

    if (x <= 0) return Q1616_FROM_FLOAT(-30.0f);

    /* Manual Range Reduction (C89 Portable) */
    exp = 0;
    while (x < Q1616_ONE_VAL) { x <<= 1; --exp; }
    while (x >= (Q1616_ONE_VAL << 1)) { x >>= 1; ++exp; }

    y = x - Q1616_ONE_VAL;

    /* Horner's evaluation for 8 terms */
    poly = (y >> 3); /* 1/8 */
    poly = q1616_sub(c_0142, q1616_mul(y, poly));
    poly = q1616_sub(c_0166, q1616_mul(y, poly));
    poly = q1616_sub(c_0200, q1616_mul(y, poly));
    poly = q1616_sub(c_0250, q1616_mul(y, poly));
    poly = q1616_sub(c_0333, q1616_mul(y, poly));
    poly = q1616_sub(c_0500, q1616_mul(y, poly));

    ln_m = q1616_mul(y, q1616_sub(Q1616_ONE_VAL, q1616_mul(y, poly)));
    ln_e = q1616_mul(Q1616_FROM_INT(exp), Q1616_LN2_CONST);

    return ln_m + ln_e;
}

/**
 * @brief ln(x) using Hardware Bit-Scan and 5-term Horner.
 * Optimized for fractal inner loops.
 */
SYSTEM_FORCEINLINE q1616_t q1616_ln_fast(q1616_t x) {
    int exp;
    q1616_t y, poly, ln_m, ln_e;
    const q1616_t c_0333 = Q1616_FROM_FLOAT(0.333333f);
    const q1616_t c_0200 = Q1616_FROM_FLOAT(0.2f);

    if (x <= 0) return Q1616_FROM_FLOAT(-30.0f);

    /* 1. Hardware-accelerated normalization */
#if defined(_MSC_VER)
    {
        unsigned long msb;
        _BitScanReverse(&msb, (unsigned long)x);
        exp = (int)msb - 16;
    }
#elif defined(__GNUC__) || defined(__clang__)
    exp = (31 - __builtin_clz((unsigned int)x)) - 16;
#else
    /* Fallback for other compilers */
    exp = 0;
    {
        q1616_t temp = x;
        while (temp < Q1616_ONE_VAL) { temp <<= 1; --exp; }
        while (temp >= (Q1616_ONE_VAL << 1)) { temp >>= 1; ++exp; }
    }
#endif

    if (exp > 0) x >>= exp; else if (exp < 0) x <<= (-exp);
    y = x - Q1616_ONE_VAL;

    /* 2. Horner's evaluation for 5 terms */
    poly = q1616_mul(y, c_0200);
    poly = q1616_sub(Q1616_FROM_FLOAT(0.25f), poly);
    poly = q1616_sub(c_0333, q1616_mul(y, poly));
    poly = q1616_sub(Q1616_FROM_FLOAT(0.5f), q1616_mul(y, poly));
    
    ln_m = q1616_mul(y, q1616_sub(Q1616_ONE_VAL, q1616_mul(y, poly)));
    ln_e = q1616_mul(Q1616_FROM_INT(exp), Q1616_LN2_CONST);

    return ln_m + ln_e;
}

#endif /* Q1616_H */