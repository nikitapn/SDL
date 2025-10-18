/* SIMD optimizations for Android software renderer pixel format conversion */

#ifndef SDL_ANDROIDSOFTNOEGL_SIMD_H
#define SDL_ANDROIDSOFTNOEGL_SIMD_H

#include "SDL3/SDL_stdinc.h"

/* 
 * Pixel format clarification:
 * 
 * SDL_PIXELFORMAT_ARGB8888 on little-endian (Android ARM/x86):
 *   - As 32-bit int: 0xAARRGGBB (ARGB order, MSB to LSB)
 *   - In memory bytes: [BB, GG, RR, AA] = BGRA byte order
 *   - This is because SDL's "packed order" naming is big-endian style
 * 
 * Android WINDOW_FORMAT_RGBA_8888:
 *   - As 32-bit int: 0xAABBGGRR (weird, but correct!)
 *   - In memory bytes: [RR, GG, BB, AA] = RGBA byte order
 * 
 * Conversion needed: Swap R and B channels (A and G stay in place)
 */

/* Convert SDL ARGB8888 (BGRA in memory) to Android RGBA8888 scanline */
static inline void ConvertARGBtoRGBA_Scanline(const Uint32 *src, Uint32 *dst, int width)
{
#if defined(__ARM_NEON) || defined(__aarch64__)
    /* NEON-optimized path for ARM - process 4 pixels at once */
    #include <arm_neon.h>

    int x = 0;

    /* Process 4 pixels at a time with NEON */
    for (; x + 3 < width; x += 4) {
        uint32x4_t pixels = vld1q_u32(&src[x]);

        /* SDL BGRA (0xAARRGGBB int) to Android RGBA (0xAABBGGRR int) */
        /* Strategy: Keep A&G in place, swap R&B positions */
        uint32x4_t ag = vandq_u32(pixels, vdupq_n_u32(0xFF00FF00));  /* Alpha and Green: 0xAA__GG__ */
        uint32x4_t r = vandq_u32(pixels, vdupq_n_u32(0x00FF0000));   /* Red:   0x__RR____ */
        uint32x4_t b = vandq_u32(pixels, vdupq_n_u32(0x000000FF));   /* Blue:  0x______BB */

        /* Rearrange: A and G stay, B→R position, R→B position */
        uint32x4_t result = vorrq_u32(
            ag,                                   /* A and G in place */
            vorrq_u32(vshlq_n_u32(b, 16),         /* B << 16 (to R position) */
                      vshrq_n_u32(r, 16))         /* R >> 16 (to B position) */
        );

        vst1q_u32(&dst[x], result);
    }

    /* Handle remaining pixels with scalar code */
    for (; x < width; ++x) {
        Uint32 pixel = src[x];
        Uint32 a = (pixel >> 24) & 0xFF;
        Uint32 r = (pixel >> 16) & 0xFF;
        Uint32 g = (pixel >> 8) & 0xFF;
        Uint32 b = pixel & 0xFF;
        dst[x] = (a << 24) | (b << 16) | (g << 8) | r;
    }
    
#elif defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    /* SSE2-optimized path for x86/x64 - process 4 pixels at once */
    #include <emmintrin.h>

    int x = 0;

    /* Process 4 pixels at a time with SSE2 */
    for (; x + 3 < width; x += 4) {
        __m128i pixels = _mm_loadu_si128((const __m128i*)&src[x]);

        /* SDL BGRA (0xAARRGGBB int) to Android RGBA (0xAABBGGRR int) */
        /* Strategy: Keep A&G in place, swap R&B positions */
        __m128i ag = _mm_and_si128(pixels, _mm_set1_epi32(0xFF00FF00));  /* Alpha and Green */
        __m128i r = _mm_and_si128(pixels, _mm_set1_epi32(0x00FF0000));  /* Red */
        __m128i b = _mm_and_si128(pixels, _mm_set1_epi32(0x000000FF));  /* Blue */

        /* Rearrange: A and G stay, B→R position, R→B position */
        __m128i result = _mm_or_si128(
            ag,                                  /* A and G in place */
            _mm_or_si128(_mm_slli_epi32(b, 16),  /* B << 16 (to R position) */
                         _mm_srli_epi32(r, 16))  /* R >> 16 (to B position) */
        );

        _mm_storeu_si128((__m128i*)&dst[x], result);
    }

    /* Handle remaining pixels with scalar code */
    for (; x < width; ++x) {
        Uint32 pixel = src[x];
        Uint32 a = (pixel >> 24) & 0xFF;
        Uint32 r = (pixel >> 16) & 0xFF;
        Uint32 g = (pixel >> 8) & 0xFF;
        Uint32 b = pixel & 0xFF;
        dst[x] = (a << 24) | (b << 16) | (g << 8) | r;
    }

#else
    /* Scalar fallback */
    for (int x = 0; x < width; ++x) {
        Uint32 pixel = src[x];

        /* SDL BGRA (0xAARRGGBB int) to Android RGBA (0xAABBGGRR int) */
        /* Extract channels and swap R<->B */
        Uint32 a = (pixel >> 24) & 0xFF;
        Uint32 r = (pixel >> 16) & 0xFF;
        Uint32 g = (pixel >> 8) & 0xFF;
        Uint32 b = pixel & 0xFF;

        dst[x] = (a << 24) | (b << 16) | (g << 8) | r;
    }
#endif
}

#endif /* SDL_ANDROIDSOFTNOEGL_SIMD_H */
