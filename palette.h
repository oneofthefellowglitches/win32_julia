#ifndef PALETTE_H
#define PALETTE_H

#include "system.h"

/* -------------------------------------------------------------------------
   1. DATA STRUCTURES
   ------------------------------------------------------------------------- */
/**
 * @struct rgb_color_t
 * @brief Represents a single 24-bit color point in a palette.
 * * Note: While this struct is 3 bytes, compilers usually pad it to 4 bytes
 * in arrays to maintain alignment, which aids scalar performance.
 */
typedef struct { 
    uint8_t red; 
    uint8_t green; 
    uint8_t blue; 
} rgb_color_t;

/**
 * @enum palette_id_e
 * @brief Unique identifiers for the available fractal color schemes.
 * * These IDs are used as the primary index for the g_fractal_palettes array.
 */
typedef enum {
    PAL_ULTRAMARINE_DEEP = 0,   /* Deep ocean blues to white */
    PAL_SOLAR_FLARE,            /* High-energy solar yellows/oranges */
    PAL_MAGENTA_NEBULA,         /* Cosmic purples and electric pinks */
    PAL_EMERALD_FIRE,           /* Toxic greens and neon highlights */
    PAL_ICE_CRYSTAL,            /* Arctic whites and crystalline cyans */
    PAL_COPPER_CANYON,          /* Earthy metallics and burnt umber */
    PAL_SPECTRUM_CYBERPUNK,     /* High-contrast neon/acid colors */
    PAL_FOREST_LAGOON,          /* Organic greens and murky aquatic tones */
    PAL_PASTEL_DREAM,           /* Soft, desaturated ethereal tones */
    PAL_TOTAL_COUNT             /* Sentinel value for array bounds/loops */
} palette_id_e;

/* -------------------------------------------------------------------------
   2. GLOBAL CONSTANTS
   ------------------------------------------------------------------------- */
/* Number of fixed 'anchor' colors per palette. */
#define COLORS_PER_PALETTE 9

/* Number of gaps between anchors where color interpolation occurs. */
#define INTERPOLATION_SEGMENTS (COLORS_PER_PALETTE - 1)

/**
 * @brief Global read-only database of color schemes.
 * * Declared 'extern' to ensure that even when this header is included in 
 * multiple translation units, only one physical instance of the data 
 * exists in the final binary (defined in the .c implementation).
 */
extern const rgb_color_t g_fractal_palettes[PAL_TOTAL_COUNT][COLORS_PER_PALETTE];

/* -------------------------------------------------------------------------
   3. RENDERING PIPELINE HELPERS
   ------------------------------------------------------------------------- */
/**
 * @brief Samples a color from a fractal palette using scalar linear interpolation.
 *
 * This function takes a normalized fixed-point value and maps it to a smooth
 * color transition between palette "anchors." It serves as the primary fallback 
 * for non-AVX2 systems and for edge-case pixel processing.
 *
 * @param norm_iter  Normalized escape value in Q16.16 (0.0 to 1.0 range).
 * @param palette_id Enumerated ID of the color scheme to be used.
 * @return uint32_t  A packed 24-bit RGB color in 0x00RRGGBB format.
 */
static SYSTEM_FORCEINLINE uint32_t 
palette_sample_lerp(q1616_t norm_iter, palette_id_e palette_id) {
    /* --- 1. VARIABLE DECLARATIONS --- */
    q1616_t segment_pos_fx;
    q1616_t blend_factor;
    int left_anchor_idx;
    const rgb_color_t* active_scheme;

    /* --- 2. COORDINATE TRANSFORMATION --- */
    /* Map normalized iteration [0.0, 1.0] to segment index space [0.0, 8.0] */
    segment_pos_fx = norm_iter * INTERPOLATION_SEGMENTS;
    
    /* Integer part: Identifies which two colors we are between.
       Fractional part: Identifies the distance between those two colors. */
    left_anchor_idx = (int)(segment_pos_fx >> 16);
    blend_factor    = segment_pos_fx & 0xFFFF; 
    
    active_scheme = g_fractal_palettes[palette_id];

    /* --- 3. BOUNDARY CLAMPING --- */
    /* Safety check to prevent array overflow at the extremes of the fractal set */
    if (left_anchor_idx >= INTERPOLATION_SEGMENTS) return 0xFFFFFFFF; /* Solid White */
    if (left_anchor_idx < 0)                       return 0x00000000; /* Solid Black */

    /* --- 4. LINEAR INTERPOLATION (LERP) --- */
    {
        /* Local block declarations for C89 compliance */
        const rgb_color_t color_start = active_scheme[left_anchor_idx];
        const rgb_color_t color_end   = active_scheme[left_anchor_idx + 1];
        uint32_t red, green, blue;

        /* Formula: Result = Start + (Distance * BlendFactor) 
           The '>> 16' performs the fixed-point division by 65536. */
        red   = (uint32_t)(color_start.red   + ((blend_factor * (color_end.red   - color_start.red))   >> 16));
        green = (uint32_t)(color_start.green + ((blend_factor * (color_end.green - color_start.green)) >> 16));
        blue  = (uint32_t)(color_start.blue  + ((blend_factor * (color_end.blue  - color_start.blue))  >> 16));

        /* --- 5. DATA PACKING --- */
        /* Reassemble individual 8-bit channels into a single 32-bit integer.
           Format: [00000000][RRRRRRRR][GGGGGGGG][BBBBBBBB] */
        return (red << 16) | (green << 8) | blue;
    }
}

/**
 * @brief Performs 8-way parallel color interpolation using AVX2 SIMD.
 *
 * This function maps eight normalized iteration values (representing fractal 
 * escape speeds) into eight 32-bit ARGB pixels. It implements a "Vectorized 
 * Linear Interpolation" (LERP) across multi-segment color schemes.
 *
 * Mathematical Logic:
 * 1. Scaling: Converts Q16.16 normalized values [0,1] to segment indices [0,8].
 * 2. Decomposition: Splits the position into an integer 'anchor' index and 
 * a fractional 'blend factor'.
 * 3. SoA Conversion: Unpacks non-contiguous RGB data from the palette array 
 * into Structure-of-Arrays (SoA) buffers for parallel math.
 * 4. Parallel LERP: Calculates Red, Green, and Blue channels for all 8 pixels
 * simultaneously using the formula: C = C1 + (Factor * (C2 - C1)) >> 16.
 * 5. Packing: Recombines channels into 0x00RRGGBB format for the framebuffer.
 *
 * @param v_norm_iters A 256-bit vector containing 8 packed Q16.16 fixed-point 
 * values (typically result of smooth iteration calculation).
 * @param palette_id   The ID of the color scheme to sample from.
 * @param out_pixels   Pointer to the destination framebuffer. Must be able to 
 * hold at least 8 uint32_t values.
 *
 * @note Requires AVX2 support (Intel Haswell / AMD Zen or newer).
 */
static SYSTEM_FORCEINLINE void 
palette_sample_lerp_avx2(__m256i v_norm_iters, palette_id_e palette_id, uint32_t* out_pixels) {
    /* --- 1. STACK DECLARATIONS --- */
    const rgb_color_t* active_scheme;
    __m256i v_segment_pos, v_left_indices, v_blend_factors;
    __m256i v_red, v_grn, v_blu, v_pixels;
    int i;

    /* Structure of Arrays (SoA) buffers for SIMD processing */
    SYSTEM_ALIGN32_BEG int index_lanes[8] SYSTEM_ALIGN32_END;
    SYSTEM_ALIGN32_BEG int r_start[8], g_start[8], b_start[8] SYSTEM_ALIGN32_END;
    SYSTEM_ALIGN32_BEG int r_delta[8], g_delta[8], b_delta[8] SYSTEM_ALIGN32_END;

    /* --- 2. INITIALIZATION & FIXED-POINT SCALING --- */
    active_scheme = g_fractal_palettes[palette_id];

    /* Project normalized [0..1] Q16.16 values to palette segment index space [0..8] 
       Shift left by 3 is effectively multiplying by INTERPOLATION_SEGMENTS (8) */
    v_segment_pos = _mm256_slli_epi32(v_norm_iters, 3); 

    /* v_left_indices: Integer part of the position (The left-hand color anchor) 
       v_blend_factors: Fractional part (The 16-bit mix ratio for LERP) */
    v_left_indices  = _mm256_srli_epi32(v_segment_pos, 16);
    v_blend_factors = _mm256_and_si256(v_segment_pos, _mm256_set1_epi32(0xFFFF));

    /* Move indices to stack so we can look up non-contiguous RGB colors */
    _mm256_store_si256((__m256i*)index_lanes, v_left_indices);

    /* --- 3. SCALAR LOOKUP & COLOR UNPACKING --- */
    for (i = 0; i < 8; ++i) {
        int idx = index_lanes[i];
        rgb_color_t c1, c2;

        /* Clamp index to [0, INTERPOLATION_SEGMENTS - 1] to prevent array overflow */
        if (idx < 0) idx = 0;
        if (idx >= INTERPOLATION_SEGMENTS) idx = INTERPOLATION_SEGMENTS - 1;

        c1 = active_scheme[idx];
        c2 = active_scheme[idx + 1];

        /* Store base color and the 'distance' to the next color for vectorized LERP */
        r_start[i] = c1.red;   r_delta[i] = (int)c2.red   - c1.red;
        g_start[i] = c1.green; g_delta[i] = (int)c2.green - c1.green;
        b_start[i] = c1.blue;  b_delta[i] = (int)c2.blue  - c1.blue;
    }

    /* --- 4. VECTORIZED LINEAR INTERPOLATION (LERP) --- */
    /* Formula: Color = StartColor + (BlendFactor * (EndColor - StartColor)) >> 16 */
    
    v_red = _mm256_add_epi32(_mm256_load_si256((__m256i*)r_start), 
            _mm256_srli_epi32(_mm256_mullo_epi32(v_blend_factors, _mm256_load_si256((__m256i*)r_delta)), 16));
                    
    v_grn = _mm256_add_epi32(_mm256_load_si256((__m256i*)g_start), 
            _mm256_srli_epi32(_mm256_mullo_epi32(v_blend_factors, _mm256_load_si256((__m256i*)g_delta)), 16));
                    
    v_blu = _mm256_add_epi32(_mm256_load_si256((__m256i*)b_start), 
            _mm256_srli_epi32(_mm256_mullo_epi32(v_blend_factors, _mm256_load_si256((__m256i*)b_delta)), 16));

    /* --- 5. COMPONENT PACKING & STORAGE --- */
    /* Combine R, G, B channels into 32-bit packed format: 0x00RRGGBB */
    v_pixels = _mm256_or_si256(
                   _mm256_slli_epi32(v_red, 16),
                   _mm256_or_si256(_mm256_slli_epi32(v_grn, 8), v_blu)
               );

    /* Use Unaligned Store because the framebuffer line might not be 32-byte aligned */
    _mm256_storeu_si256((__m256i*)out_pixels, v_pixels);
}

#endif /* PALETTE_H */

/* -------------------------------------------------------------------------
   4. IMPLEMENTATION (Compiled ONLY in the owner file)
   ------------------------------------------------------------------------- */
#ifdef PALETTE_IMPLEMENTATION
#ifndef PALETTE_IMPL_GUARD
#define PALETTE_IMPL_GUARD

/* This data is created EXACTLY once in the entire project */
const rgb_color_t g_fractal_palettes[PAL_TOTAL_COUNT][COLORS_PER_PALETTE] = {
    /* PAL_ULTRAMARINE_DEEP: classic deep blue */
    { {66,30,15},    {25,7,26},     {9,1,47},
      {4,4,73},      {0,7,100},     {12,44,138},
      {24,82,177},   {57,125,209},  {255,255,255} },

    /* PAL_SOLAR_FLARE: intense heat/fire */
    { {30,8,0},      {66,14,5},     {120,35,0},
      {200,70,0},    {255,140,0},   {255,180,40},
      {255,220,100}, {255,250,200}, {255,255,255} },

    /* PAL_MAGENTA_NEBULA: violet and electric pink */
    { {20,0,20},     {60,0,60},     {120,0,120},
      {180,0,180},   {255,0,255},   {255,60,180},
      {255,120,120}, {255,180,180}, {255,255,255} },

    /* PAL_EMERALD_FIRE: high contrast complementary (green/orange) */
    { {0,32,63},     {0,78,89},     {0,119,90},
      {0,163,89},    {255,120,0},   {255,170,0},
      {255,210,60},  {255,245,160}, {255,255,255} },

    /* PAL_ICE_CRYSTAL: high-altitude frost and cold water */
    { {0,0,0},       {0,20,40},     {0,60,80},
      {0,100,120},   {80,160,200},  {140,200,240},
      {200,230,255}, {230,245,255}, {255,255,255} },

    /* PAL_COPPER_CANYON: metallic browns and bronze */
    { {40,20,0},     {80,30,0},     {120,50,0},
      {160,70,0},    {200,100,0},   {220,140,40},
      {240,180,80},  {255,220,160}, {255,255,255} },

    /* PAL_SPECTRUM_CYBERPUNK: vivid neon/acid trip */
    { {0,0,0},       {0,255,0},     {0,200,255},
      {0,100,255},   {100,0,255},   {255,0,255},
      {255,0,100},   {255,255,0},   {255,255,255} },

    /* PAL_FOREST_LAGOON: natural foliage and aquatic transitions */
    { {20,20,0},     {60,40,0},     {100,80,0},
      {160,140,0},   {120,200,120}, {80,160,200},
      {40,120,240},  {180,220,255}, {255,255,255} },

    /* PAL_PASTEL_DREAM: soft, washed-out ethereal */
    { {255,179,186}, {255,223,186}, {255,255,186},
      {186,255,201}, {186,225,255}, {200,200,255},
      {255,200,255}, {240,240,240}, {255,255,255} }
};

#endif /* PALETTE_IMPL_GUARD */
#endif /* PALETTE_IMPLEMENTATION */