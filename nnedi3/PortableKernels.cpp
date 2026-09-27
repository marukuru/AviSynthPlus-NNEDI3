// SPDX-License-Identifier: GPL-2.0-or-later
#include "PortableKernels.h"
#include <cstdint>
#include <initializer_list>
#include <avs/cpuid.h>

#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>

// Inputs are 8-bit pixels or unsigned pixels with at most 15 significant bits.
// Widen the pairwise products before summing high-bit-depth neighbourhoods.
template<bool wide>
__attribute__((target("sse2")))
static void dot_sse2(const float *input, const float *weightsf, float *out,
                     int count, int length, const float *scale)
{
    const auto *data = reinterpret_cast<const int16_t *>(input);
    const auto *weights = reinterpret_cast<const int16_t *>(weightsf);
    const auto *factors = reinterpret_cast<const float *>(weights + count * length);
    for (int i = 0; i < count; ++i) {
        __m128i sum = _mm_setzero_si128();
        for (int j = 0; j < length; j += 8) {
            const __m128i v = _mm_madd_epi16(
                _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + j)),
                _mm_loadu_si128(reinterpret_cast<const __m128i *>(weights + j)));
            if (wide) {
                const __m128i sign = _mm_srai_epi32(v, 31);
                sum = _mm_add_epi64(sum, _mm_unpacklo_epi32(v, sign));
                sum = _mm_add_epi64(sum, _mm_unpackhi_epi32(v, sign));
            } else {
                sum = _mm_add_epi32(sum, v);
            }
        }
        float value;
        if (wide) {
            alignas(16) int64_t lanes[2];
            _mm_store_si128(reinterpret_cast<__m128i *>(lanes), sum);
            value = static_cast<float>(lanes[0] + lanes[1]);
        } else {
            alignas(16) int32_t lanes[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(lanes), sum);
            value = static_cast<float>(int64_t(lanes[0]) + lanes[1] + lanes[2] + lanes[3]);
        }
        const int off = (i / 4) * 8 + i % 4;
        out[i] = value * factors[off] * *scale + factors[off + 4];
        weights += length;
    }
}

template<bool wide>
__attribute__((target("avx2")))
static void dot_avx2(const float *input, const float *weightsf, float *out,
                     int count, int length, const float *scale)
{
    const auto *data = reinterpret_cast<const int16_t *>(input);
    const auto *weights = reinterpret_cast<const int16_t *>(weightsf);
    const auto *factors = reinterpret_cast<const float *>(weights + count * length);
    for (int i = 0; i < count; ++i) {
        __m256i sum = _mm256_setzero_si256();
        for (int j = 0; j < length; j += 16) {
            const __m256i v = _mm256_madd_epi16(
                _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + j)),
                _mm256_loadu_si256(reinterpret_cast<const __m256i *>(weights + j)));
            if (wide) {
                sum = _mm256_add_epi64(sum, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v)));
                sum = _mm256_add_epi64(sum, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1)));
            } else {
                sum = _mm256_add_epi32(sum, v);
            }
        }
        float value;
        if (wide) {
            alignas(32) int64_t lanes[4];
            _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), sum);
            value = static_cast<float>(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
        } else {
            alignas(32) int32_t lanes[8];
            _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), sum);
            int64_t total = 0;
            for (int32_t lane : lanes) total += lane;
            value = static_cast<float>(total);
        }
        const int off = (i / 4) * 8 + i % 4;
        out[i] = value * factors[off] * *scale + factors[off + 4];
        weights += length;
    }
}
#endif

DotProduct selectIntegerDotProduct(int cpu_flags, bool wide)
{
#if defined(__i386__) || defined(__x86_64__)
    if (cpu_flags & CPUF_AVX2) return wide ? dot_avx2<true> : dot_avx2<false>;
    if (cpu_flags & CPUF_SSE2) return wide ? dot_sse2<true> : dot_sse2<false>;
#endif
    return nullptr;
}

int optimizationRequirements(int opt)
{
    const int sse = CPUF_SSE2 | CPUF_SSE4_1;
    const int avx2 = sse | CPUF_AVX | CPUF_AVX2;
    switch (opt) {
    case 1: return 0;
    case 2: return CPUF_SSE2;
    case 3: return sse;
    case 4: return sse | CPUF_AVX;
    case 5: return avx2;
    case 6: return avx2 | CPUF_FMA3;
    // FMA4 kernels share the AVX2 extraction, integer and exponential helpers.
    case 7: return avx2 | CPUF_FMA4;
    case 8: return avx2 | CPUF_FMA3 | CPUF_AVX512F | CPUF_AVX512DQ | CPUF_AVX512BW | CPUF_AVX512VL;
    default: return -1;
    }
}

int resolveOptimization(int requested, int cpu_flags, bool assembly_available)
{
    if (requested < 0 || requested > 8) return -1;
    if (!assembly_available) return requested <= 1 ? 1 : -1;
    if (requested == 0) {
        for (int mode : {8, 6, 7, 5, 4, 3, 2, 1}) {
            const int required = optimizationRequirements(mode);
            if ((cpu_flags & required) == required) return mode;
        }
    }
    const int required = optimizationRequirements(requested);
    return (cpu_flags & required) == required ? requested : -1;
}

const char *optimizationName(int opt)
{
    static const char *const names[] = {"automatic", "C++", "SSE2", "SSE4.1", "AVX", "AVX2", "FMA3", "FMA4", "AVX512"};
    return opt >= 0 && opt <= 8 ? names[opt] : "invalid";
}
