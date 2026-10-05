#pragma once

#include <cstddef>
#ifdef __AVX2__
#include <immintrin.h>
#endif

// SIMD-accelerated AABB-frustum culling, abstracted behind a clean interface
// so the backend (AVX2, std::simd, scalar) is pluggable.
//
// When C++26 std::simd lands, add a new backend here without touching callers.


namespace adofai {

namespace CullSIMD {

// Number of AABBs processed in one SIMD batch
constexpr size_t WIDTH = 4;

// Test 4 AABBs against the four frustum edges.
// Returns a 4-bit mask: bit i is set if instance i is inside the frustum.
inline int test4(const double* minX, const double* maxX,
                 const double* minY, const double* maxY,
                 double vl, double vr, double vb, double vt) {
    int mask = 0;
#ifdef __AVX2__
    __m256d v_vl = _mm256_set1_pd(vl), v_vr = _mm256_set1_pd(vr);
    __m256d v_vb = _mm256_set1_pd(vb), v_vt = _mm256_set1_pd(vt);
    __m256d v_minX = _mm256_loadu_pd(minX), v_maxX = _mm256_loadu_pd(maxX);
    __m256d v_minY = _mm256_loadu_pd(minY), v_maxY = _mm256_loadu_pd(maxY);
    __m256d c0 = _mm256_cmp_pd(v_maxX, v_vl, _CMP_NLT_UQ); // maxX >= vl
    __m256d c1 = _mm256_cmp_pd(v_minX, v_vr, _CMP_LE_OQ);  // minX <= vr
    __m256d c2 = _mm256_cmp_pd(v_maxY, v_vb, _CMP_NLT_UQ); // maxY >= vb
    __m256d c3 = _mm256_cmp_pd(v_minY, v_vt, _CMP_LE_OQ);  // minY <= vt
    mask = _mm256_movemask_pd(_mm256_and_pd(_mm256_and_pd(c0, c1), _mm256_and_pd(c2, c3)));
#else
    for (size_t i = 0; i < WIDTH; i++) {
        if (maxX[i] >= vl && minX[i] <= vr && maxY[i] >= vb && minY[i] <= vt)
            mask |= (1 << (int)i);
    }
#endif
    return mask;
}

} // namespace CullSIMD

}  // namespace adofai
