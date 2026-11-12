#pragma once

// A hand-vectorised dot product. The effect spends most of its time in two
// FIR filters; auto-vectorisers disagree about how to handle them (what is
// fast under one compiler is slow under another), so this is spelled out for
// SSE2 (x86-64) and NEON (ARM64), with a portable fallback.

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <emmintrin.h>
 #define TONEGENIE_SIMD_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
 #include <arm_neon.h>
 #define TONEGENIE_SIMD_NEON 1
#endif

namespace tg
{

/** sum over i of a[i] * b[i], for any n >= 0. */
inline float dotProduct (const float* a, const float* b, int n) noexcept
{
    int i = 0;
    float sum = 0.0f;

   #if TONEGENIE_SIMD_SSE2
    __m128 s0 = _mm_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;

    for (; i + 16 <= n; i += 16)
    {
        s0 = _mm_add_ps (s0, _mm_mul_ps (_mm_loadu_ps (a + i), _mm_loadu_ps (b + i)));
        s1 = _mm_add_ps (s1, _mm_mul_ps (_mm_loadu_ps (a + i + 4), _mm_loadu_ps (b + i + 4)));
        s2 = _mm_add_ps (s2, _mm_mul_ps (_mm_loadu_ps (a + i + 8), _mm_loadu_ps (b + i + 8)));
        s3 = _mm_add_ps (s3, _mm_mul_ps (_mm_loadu_ps (a + i + 12), _mm_loadu_ps (b + i + 12)));
    }

    alignas (16) float lanes[4];
    _mm_store_ps (lanes, _mm_add_ps (_mm_add_ps (s0, s1), _mm_add_ps (s2, s3)));
    sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
   #elif TONEGENIE_SIMD_NEON
    float32x4_t s0 = vdupq_n_f32 (0.0f), s1 = s0, s2 = s0, s3 = s0;

    for (; i + 16 <= n; i += 16)
    {
        s0 = vaddq_f32 (s0, vmulq_f32 (vld1q_f32 (a + i), vld1q_f32 (b + i)));
        s1 = vaddq_f32 (s1, vmulq_f32 (vld1q_f32 (a + i + 4), vld1q_f32 (b + i + 4)));
        s2 = vaddq_f32 (s2, vmulq_f32 (vld1q_f32 (a + i + 8), vld1q_f32 (b + i + 8)));
        s3 = vaddq_f32 (s3, vmulq_f32 (vld1q_f32 (a + i + 12), vld1q_f32 (b + i + 12)));
    }

    float lanes[4];
    vst1q_f32 (lanes, vaddq_f32 (vaddq_f32 (s0, s1), vaddq_f32 (s2, s3)));
    sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
   #endif

    for (; i < n; ++i)
        sum += a[i] * b[i];

    return sum;
}

} // namespace tg
