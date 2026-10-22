#pragma once
// JSON 结构扫描：只在**字节分类**这一层做 SIMD。
//
// 为什么可以向量化：这里只回答"下一个结构字节在哪"（`"` `\` `[` `]` `{` `}`），
// 既不读也不写任何数值。向量分支与标量分支对同一段输入给出**完全相同**的返回指针，
// 所以上层解析（parseNumber / parseInt / strtod）的位级结果不可能改变 ——
// 本文件没有一次浮点运算，也没有重排有副作用的操作（括号深度仍按字节顺序逐个 +1/−1）。
//
// 为什么不是"逐命中"扫描：JSON action 对象里引号很密（平均每 4~5 字节一个结构字节），
// 每次命中都 ctz + 分支 + 重新加载向量的写法在本机实测**比逐字节还慢**（约 0.4~0.6x）。
// 这里改成"整块掩码"：
//   1. 一次加载 16 字节，算出引号掩码 q 和括号掩码 brackets（两次 movemask）；
//   2. 用前缀异或求"块内每个字节是否在字符串里"（in-string 掩码），把串内括号一次抹掉；
//   3. 只剩括号需要顺序处理 —— 每个对象通常 2~4 个，逐字节的引号开销全部消失。
// 块里出现反斜杠就整块退回标量状态机（转义规则不猜），所以转义/未闭合字符串/CR
// 这些边角与原来的逐字节实现逐字符一致。
//
// 后端：NEON（arm64，本机实测路径）/ AVX2（32 B 块）/ SSE2（16 B 块）/ 标量兜底。
// 向量只在 `e - p >= kVecBytes` 时加载，绝不读到 e 之后 —— mmap 出来的文件末尾并不
// 终止于 e（见 LevelData.cpp 里 parseNumber 的那段注释），尾部一律交给逐字节循环。

#include <cstddef>
#include <cstdint>
#include <cstring>

#if (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)) && \
    (defined(__aarch64__) || defined(_M_ARM64))
#  define ADOCAO_JSONSCAN_NEON 1
#  include <arm_neon.h>
#elif defined(__AVX2__)
#  define ADOCAO_JSONSCAN_AVX2 1
#  include <immintrin.h>
#elif defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64))
#  define ADOCAO_JSONSCAN_SSE2 1
#  include <emmintrin.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#endif

namespace adofai {
namespace jsonscan {

#if defined(ADOCAO_JSONSCAN_NEON) || defined(ADOCAO_JSONSCAN_AVX2) || defined(ADOCAO_JSONSCAN_SSE2)
#  define ADOCAO_JSONSCAN_HAVE_VEC 1
#endif

inline unsigned ctz32(uint32_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i = 0;
    _BitScanForward(&i, x);
    return (unsigned)i;
#else
    return (unsigned)__builtin_ctz(x);
#endif
}

// ---------------------------------------------------------------------------
// 后端：16/32 字节块的 load / or / eq / movemask
// ---------------------------------------------------------------------------
#if defined(ADOCAO_JSONSCAN_NEON)

using Vec = uint8x16_t;
inline constexpr size_t kVecBytes = 16;
inline constexpr uint32_t kAllMask = 0xFFFFu;

// 每个字节 0xFF/0x00 → 位掩码（bit i = 第 i 字节非零）。
// 低/高 8 字节各一次水平加（ADDV）比两次窄化移位更短。
inline uint32_t vmask(Vec m) {
    static constexpr uint8_t kLsb[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                                         1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t t = vandq_u8(m, vld1q_u8(kLsb));
    return (uint32_t)vaddv_u8(vget_low_u8(t)) | ((uint32_t)vaddv_u8(vget_high_u8(t)) << 8);
}
inline Vec vload(const char* p) { return vld1q_u8((const uint8_t*)p); }
inline Vec vor(Vec a, Vec b) { return vorrq_u8(a, b); }
inline Vec veq(Vec a, char c) { return vceqq_u8(a, vdupq_n_u8((uint8_t)c)); }

#elif defined(ADOCAO_JSONSCAN_AVX2)

using Vec = __m256i;
inline constexpr size_t kVecBytes = 32;
inline constexpr uint32_t kAllMask = 0xFFFFFFFFu;

inline uint32_t vmask(Vec m) { return (uint32_t)_mm256_movemask_epi8(m); }
inline Vec vload(const char* p) { return _mm256_loadu_si256((const __m256i*)p); }
inline Vec vor(Vec a, Vec b) { return _mm256_or_si256(a, b); }
inline Vec veq(Vec a, char c) { return _mm256_cmpeq_epi8(a, _mm256_set1_epi8(c)); }

#elif defined(ADOCAO_JSONSCAN_SSE2)

using Vec = __m128i;
inline constexpr size_t kVecBytes = 16;
inline constexpr uint32_t kAllMask = 0xFFFFu;

inline uint32_t vmask(Vec m) { return (uint32_t)_mm_movemask_epi8(m); }
inline Vec vload(const char* p) { return _mm_loadu_si128((const __m128i*)p); }
inline Vec vor(Vec a, Vec b) { return _mm_or_si128(a, b); }
inline Vec veq(Vec a, char c) { return _mm_cmpeq_epi8(a, _mm_set1_epi8(c)); }

#endif

// ---------------------------------------------------------------------------
// 容器：p 指向 '[' 或 '{'；返回配对闭括号之后的位置，失败返回 nullptr。
// 语义 = 逐字节：`"` 走字符串（`\` 转义）、`[`/`{` ++depth、`]`/`}` --depth（到 0 即返回）。
// ---------------------------------------------------------------------------
inline const char* skipContainer(const char* p, const char* e) {
    int depth = 0;
    bool inStr = false;
    while (p < e) {
#if defined(ADOCAO_JSONSCAN_HAVE_VEC)
        if ((size_t)(e - p) >= kVecBytes) {
            const Vec v = vload(p);
            // 有反斜杠就不在这里猜转义规则：整块交给下面的逐字节状态机。
            if (vmask(veq(v, '\\')) == 0) {
                const uint32_t brackets =
                    vmask(vor(vor(veq(v, '['), veq(v, '{')), vor(veq(v, ']'), veq(v, '}'))));
                const uint32_t q = vmask(veq(v, '"'));
                // 前缀异或：px 的 bit i = 引号掩码在 [0..i] 上的奇偶性
                uint32_t px = q;
                px ^= px << 1; px ^= px << 2; px ^= px << 4; px ^= px << 8;
                if constexpr (kVecBytes == 32) px ^= px << 16;
                px &= kAllMask;
                // 第 i 字节"在本字节之前"是否已在串内 = 块首状态 ^ 前 i 个引号的奇偶
                uint32_t ins = (px << 1) & kAllMask;
                if (inStr) ins = ~ins & kAllMask;
                uint32_t b = brackets & ~ins & kAllMask;
                inStr = inStr != (((px >> (kVecBytes - 1)) & 1u) != 0);   // 离开本块时的串状态
                while (b) {
                    const unsigned i = ctz32(b);
                    b &= b - 1;
                    const char c = p[i];
                    if (c == '[' || c == '{') {
                        ++depth;
                    } else if (--depth == 0) {
                        return p + i + 1;
                    }
                }
                p += kVecBytes;
                continue;
            }
        }
#endif
        const char c = *p;
        if (inStr) {
            if (c == '\\') { if (p + 1 >= e) return nullptr; p += 2; continue; }
            if (c == '"') inStr = false;
            ++p;
            continue;
        }
        if (c == '"') { inStr = true; ++p; continue; }
        if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') { if (--depth == 0) return p + 1; }
        ++p;
    }
    return nullptr;
}

}  // namespace jsonscan
}  // namespace adofai
