#include "archive/AdocaoDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {
namespace {

// ---- 位模式比较：double 的"值相等"一律按位（和列编码器的字典一致，
//      -0.0 / NaN 也算不同的值）----
inline uint64_t bitsOf(double d) {
    uint64_t u = 0;
    std::memcpy(&u, &d, 8);
    return u;
}

inline uint64_t distinctOf(std::vector<uint64_t>& v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return (uint64_t)(std::unique(v.begin(), v.end()) - v.begin());
}

// ---- 整数域的环绕/varint（§8.2 的残差后端就是这两个）----
inline uint64_t zigzagOf(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
inline uint64_t varintLen(uint64_t zig) {
    uint64_t z = zig;
    uint64_t n = 1;
    while (z >= 0x80) { z >>= 7; ++n; }
    return n;
}
// 环绕到 (-half, half]：和 §8.2② 一样在**整数域**做（浮点的 fmod 会带 1 ULP 尾巴）
inline int64_t wrapIntHalf(int64_t d, int64_t period, int64_t half) {
    if (period <= 0) return d;
    int64_t r = d % period;
    if (r > half) r -= period;
    else if (r <= -half) r += period;
    return r;
}

// 环绕到 (-period/2, period/2]。**诊断里允许用浮点**（这只是"看有没有结构"，
// 真正的编码必须走 §8.2 的整数域 —— 浮点环绕出来的值会有 1 ULP 的尾巴，
// 于是"塌陷到 1 个值"实际会报到 2 个：这正是 §8.2② 要说的那件事）。
inline double wrapHalf(double d, double period) {
    const double half = period * 0.5;
    double x = std::fmod(d + half, period);
    if (x < 0.0) x += period;
    return x - half;
}

struct Sample {
    std::vector<double> v;        // 采样后的（已掩码）值
    std::vector<uint64_t> raw;    // 未掩码的位模式（用于 d1Raw/wrapRateRaw 的对照）
    uint64_t nTotal = 0;
    uint64_t sentinels = 0;
    uint64_t sentinelRuns = 0;
};

// 前缀采样 + 哨兵掩码。**不跨步抽样**：相邻对（环绕率、差分基数）全靠相邻。
Sample takeSample(const std::vector<double>& v, const ColumnSpec& spec, const DiagOptions& opt) {
    Sample s;
    s.nTotal = (uint64_t)v.size();
    const uint64_t take = std::min<uint64_t>(s.nTotal, opt.maxSample);
    s.raw.reserve((size_t)take);
    s.v.reserve((size_t)take);
    bool inRun = false;
    for (uint64_t i = 0; i < take; ++i) {
        s.raw.push_back(bitsOf(v[(size_t)i]));
        const bool sent = spec.angular && v[(size_t)i] == spec.sentinel;
        if (sent) {
            ++s.sentinels;
            if (!inRun) { ++s.sentinelRuns; inRun = true; }
            continue;
        }
        inRun = false;
        s.v.push_back(v[(size_t)i]);
    }
    return s;
}

// ---- 定点网格：v·2^e ∈ ℤ（2 的幂缩放是精确的，所以这是**真判定**）----
// ⚠️ 上限必须是"**粗**网格"：**任何** double 都在某个 2^-N 上（[256,512) 区间的 ULP 就是
// 2^-44，所以 N 能到 50+）。不设上限的话每列都会"落在网格上"，于是：
//   ① `FixedPoint` 变成"把尾数重新打一遍"（≈ 6.75 B/值 vs Raw 8 B）——**1.2× 的小便宜
//      会把真正的结构（WrapDelta 那种 20×）挤出裁决**；
//   ② §8.1 里"100×+"那个粗网格信号被淹没。
// 真踩过：`diag_irregular`（均匀随机）被判成 FixedPoint，只因它的值是 k/2^29（脚本用
// 32 位整数造的）—— 判据没错，但它已经不是"结构"了。
inline constexpr int kCoarseGridExp2 = 20;
int gridPowerOfTwo(const std::vector<double>& v) {
    if (v.empty()) return -1;
    const size_t probe = std::min<size_t>(v.size(), 4096);
    for (int e = 0; e <= kCoarseGridExp2; ++e) {
        const double s = std::ldexp(1.0, e);
        bool ok = true;
        for (size_t i = 0; i < probe; ++i) {
            const double x = v[i] * s;
            if (!std::isfinite(x) || std::floor(x) != x) { ok = false; break; }
        }
        if (ok) return e;
    }
    return -1;
}

// 十进制网格：**精确判定**（不是容差）—— 值必须**真的**等于 k/10^e（在 double 里逐位相等）。
// 为什么不能用容差：0.1 在二进制里是无限小数（= 3602879701896397/2^55），拿 1e-6 去容，
// 任何"小数位够多"的谱都会被判成"落在 10^-9 网格上"（真踩过：均匀随机的谱被报成
// `FixedPoint`，于是"没结构"的结论被一句假判据顶掉）。而 `k/10^e == v` 是**可验的**：
// 0.3 的 double 就是 3/10 的 double，所以它成立；随机的 double 几乎不可能成立。
int gridPowerOfTen(const std::vector<double>& v) {
    if (v.empty()) return -1;
    const size_t probe = std::min<size_t>(v.size(), 4096);
    double s = 1.0;
    for (int e = 0; e <= 9; ++e, s *= 10.0) {
        bool ok = true;
        for (size_t i = 0; i < probe; ++i) {
            const double x = v[i] * s;
            if (!std::isfinite(x)) { ok = false; break; }
            const double k = std::round(x);
            if (k / s != v[i]) { ok = false; break; }
        }
        if (ok) return e;
    }
    return -1;
}

// 定点候选的估算字节/值：把值缩放到整数、看整数**值域**要几个 bit（+ 一个首值平摊）。
// 这才是 FixedPoint 的真实开销 —— 固定给个 2.0 会让它在"网格判歪"时白赢 Raw。
double fixedPointBytesPerValue(const std::vector<double>& v, int e2, int e10) {
    if (v.empty() || (e2 < 0 && e10 < 0)) return 8.0;
    const double s = e2 >= 0 ? std::ldexp(1.0, e2) : std::pow(10.0, (double)e10);
    const size_t probe = std::min<size_t>(v.size(), 4096);
    int64_t lo = 0, hi = 0;
    bool first = true;
    for (size_t i = 0; i < probe; ++i) {
        const int64_t k = (int64_t)std::llround(v[i] * s);
        if (first) { lo = hi = k; first = false; }
        else { lo = std::min(lo, k); hi = std::max(hi, k); }
    }
    unsigned bits = 1;
    while (bits < 62 && (hi - lo) >> bits != 0) ++bits;
    return (double)bits / 8.0 + 8.0 / (double)v.size();
}

// ---- 自相关（归一化，处理边缘）：找"主峰" ----
// 只在采样上跑；lag 上限 maxLag。平滑数据在 lag=1 上自然接近 1，所以主峰只在 lag ≥ 2 里取。
void autocorrelation(const std::vector<double>& v, int maxLag, int& bestLag, double& bestPeak) {
    bestLag = 0;
    bestPeak = 0.0;
    const int n = (int)v.size();
    if (n < 8) return;
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= (double)n;
    std::vector<double> c((size_t)n);
    for (int i = 0; i < n; ++i) c[(size_t)i] = v[(size_t)i] - mean;

    const int lagMax = std::min(maxLag, n / 4);
    for (int lag = 2; lag <= lagMax; ++lag) {
        double num = 0.0, a = 0.0, b = 0.0;
        const int m = n - lag;
        for (int i = 0; i < m; ++i) {
            const double x = c[(size_t)i], y = c[(size_t)i + lag];
            num += x * y;
            a += x * x;
            b += y * y;
        }
        if (a <= 0.0 || b <= 0.0) continue;
        const double r = num / std::sqrt(a * b);
        if (r > bestPeak) { bestPeak = r; bestLag = lag; }
    }
}

// ---- 窗口唯一数：p 个连续的**位模式**作为窗口，滚动哈希后数不同窗口 ----
// 逐位重复才会塌陷 —— 全精度的正弦每层差 1 ULP，这里就永远不塌（这是诚实的结果，
// 也是 `Motif` 的真实前提，见 docs/adocao-format.md §8.1 的 ⚠️）。
uint64_t windowUnique(const std::vector<uint64_t>& bits, int p, uint64_t* windowCount) {
    const size_t n = bits.size();
    if (p <= 0 || (size_t)p >= n) { *windowCount = 0; return 0; }
    const uint64_t B = 0x100000001B3ull;   // FNV prime
    uint64_t powB = 1;
    for (int i = 0; i < p - 1; ++i) powB *= B;

    std::vector<uint64_t> hashes;
    hashes.reserve(n - (size_t)p + 1);
    uint64_t h = 0;
    for (int i = 0; i < p; ++i) h = h * B + bits[(size_t)i];
    hashes.push_back(h);
    for (size_t j = 0; j + (size_t)p < n; ++j) {
        h = (h - bits[j] * powB) * B + bits[j + (size_t)p];
        hashes.push_back(h);
    }
    *windowCount = (uint64_t)hashes.size();
    return distinctOf(hashes);
}

// ---- 判据的裁决：把每条候选折算成"估算字节/值"，取最小 ----
// 这是**估算**（字典 + 下标两段都算进去了），目的只是让人一眼看到"哪条路最可能赢"，
// 真正的裁决永远是编码器里那次实编（§8.0-2）。
struct Choice {
    const char* name = "Raw";
    double bytesPerValue = 8.0;
    std::string why;
};

double dictBytes(uint64_t n, uint64_t cardinality, double valueBytes) {
    if (n == 0) return 0.0;
    // 字典本体 + 位打包下标（+ 一个粗略的 varint 尾差，这里忽略，够用）
    double bits = 1.0;
    while ((1ull << (int)bits) < cardinality && bits < 64.0) bits += 1.0;
    return ((double)cardinality * valueBytes + (double)n * bits / 8.0) / (double)n;
}

uint64_t bitWidthFor(uint64_t cardinality) {
    uint64_t bits = 1;
    while ((1ull << (int)bits) < cardinality && bits < 64) bits += 1;
    return bits;
}

}  // namespace

uint64_t maskSentinels(const std::vector<double>& v, const ColumnSpec& spec,
                       std::vector<double>& masked, std::vector<uint8_t>& bitmap) {
    masked.clear();
    bitmap.assign((v.size() + 7) / 8, 0);
    uint64_t count = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (spec.angular && v[i] == spec.sentinel) {
            bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
            ++count;
            continue;
        }
        masked.push_back(v[i]);   // 原样拷贝：不碰数值
    }
    return count;
}

void diagnoseDoubles(const std::vector<double>& v, const ColumnSpec& spec,
                     const DiagOptions& opt, ColumnDiagnostics& out) {
    out = ColumnDiagnostics{};
    out.nTotal = (uint64_t)v.size();
    const Sample s = takeSample(v, spec, opt);
    out.n = (uint64_t)s.v.size();
    out.sentinels = s.sentinels;
    out.sentinelRuns = s.sentinelRuns;
    if (out.n == 0) { out.verdict = "Const"; out.note = "整列都是哨兵/空列"; return; }

    // 值集基数 / 游程
    {
        std::vector<uint64_t> b;
        b.reserve(s.v.size());
        for (double x : s.v) b.push_back(bitsOf(x));
        out.runs = 1;
        for (size_t i = 1; i < b.size(); ++i)
            if (b[i] != b[i - 1]) ++out.runs;
        out.distinct = distinctOf(b);
    }

    // 差分：raw 与 wrapped 各一对（一阶 + 二阶）
    const double period = spec.period > 0.0 ? spec.period : 360.0;
    const double half = period * 0.5;
    {
        std::vector<uint64_t> d1, d2, w1, w2;
        const size_t n = s.v.size();
        d1.reserve(n); d2.reserve(n); w1.reserve(n); w2.reserve(n);
        double prevDeltaRaw = 0.0, prevDeltaWrap = 0.0;
        uint64_t wrapHits = 0, pairs = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                const double d = s.v[i] - s.v[i - 1];
                d1.push_back(bitsOf(d));
                if (i > 1) d2.push_back(bitsOf(d - prevDeltaRaw));
                prevDeltaRaw = d;
                if (std::fabs(d) > half) ++wrapHits;
                ++pairs;
                // wrapped（只在 angular 列上有意义；非角度列按"整列无环绕"处理）
                if (spec.angular) {
                    const double w = wrapHalf(d, period);
                    w1.push_back(bitsOf(w));
                    if (i > 1) w2.push_back(bitsOf(w - prevDeltaWrap));
                    prevDeltaWrap = w;
                }
            }
        }
        out.wrapRateMasked = pairs ? (double)wrapHits / (double)pairs : 0.0;
        out.d1Raw = distinctOf(d1);
        out.d2Raw = distinctOf(d2);
        out.d1Wrapped = spec.angular ? distinctOf(w1) : out.d1Raw;
        out.d2Wrapped = spec.angular ? distinctOf(w2) : out.d2Raw;
    }

    // 未掩码的对照：**这一对数字就是"哨兵掩码到底有没有用"的证据**
    // （`diag_midspin` 那种谱上 raw 会被哨兵抬到 ~25%，masked 应该回落到滑音的真实水平）。
    {
        const size_t n = s.raw.size();
        uint64_t wrapHits = 0, pairs = 0;
        double prev = 0.0;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            double x;
            std::memcpy(&x, &s.raw[i], 8);
            if (!first) {
                if (std::fabs(x - prev) > half) ++wrapHits;
                ++pairs;
            }
            prev = x;
            first = false;
        }
        out.wrapRateRaw = pairs ? (double)wrapHits / (double)pairs : 0.0;
    }

    // 定点网格 / 整数
    out.allIntegers = true;
    for (double x : s.v)
        if (std::floor(x) != x) { out.allIntegers = false; break; }
    out.gridExp2 = gridPowerOfTwo(s.v);
    out.gridExp10 = gridPowerOfTen(s.v);

    // ---- 整数域的二阶差分（§8.2①：残差只在整数域里算）----
    // 为什么非要这一步：浮点域里 4 位小数的滑音，真曲率（~1e-7）会被 ±1e-4 的舍入噪声
    // 完全淹没，于是"wrapped Δ² 基数"只塌 2~3 倍、看起来 WrapDelta 不值钱；换成整数域，
    // 同一份数据的 raw Δ² 是 ±3600000 的尖峰、wrapped Δ² 只有 ±40 —— 收益一眼可见。
    {
        double scale = 0.0;
        if (out.gridExp10 >= 0) scale = std::pow(10.0, (double)out.gridExp10);
        else if (out.gridExp2 >= 0) scale = std::ldexp(1.0, out.gridExp2);
        if (scale > 0.0) {
            std::vector<int64_t> k(s.v.size());
            bool ok = true;
            for (size_t i = 0; i < s.v.size() && ok; ++i) {
                const double x = s.v[i] * scale;
                if (!std::isfinite(x) || std::fabs(x) > 9.0e18) { ok = false; break; }
                const double r = std::round(x);
                if (r / scale != s.v[i]) { ok = false; break; }   // 必须是**精确**整数域
                k[i] = (int64_t)r;
            }
            if (ok && k.size() >= 3) {
                out.gridScale = scale;
                // 注意别把名字取成 period/half：那会遮蔽外层同名的 double，而
                // `const int64_t period = llround(period * scale)` 里的 period 指的是
                // **正在声明的自己**（初始化器里已进入作用域）→ 拿未初始化的值算周期。
                const int64_t periodI = (int64_t)std::llround(period * scale);
                const int64_t halfI = periodI / 2;
                uint64_t rawBytes = 0, wrapBytes = 0, cnt = 0;
                int64_t d1RawPrev = 0, d1WrapPrev = 0;
                bool havePrev = false;
                for (size_t i = 1; i < k.size(); ++i) {
                    const int64_t d = k[i] - k[i - 1];            // 整数域一阶差分
                    const int64_t w = wrapIntHalf(d, periodI, halfI);  // 环绕到 (-180,180]
                    if (havePrev) {
                        const int64_t r2 = d - d1RawPrev, w2 = w - d1WrapPrev;
                        out.d2RawMax = std::max(out.d2RawMax, std::abs(r2));
                        out.d2WrapMax = std::max(out.d2WrapMax, std::abs(w2));
                        rawBytes += varintLen(zigzagOf(r2));
                        wrapBytes += varintLen(zigzagOf(w2));
                        ++cnt;
                    }
                    d1RawPrev = d;
                    d1WrapPrev = w;
                    havePrev = true;
                }
                if (cnt) {
                    out.d2RawBytes = (double)rawBytes / (double)cnt;
                    out.d2WrapBytes = (double)wrapBytes / (double)cnt;
                }
            }
        }
    }

    // 周期（自相关主峰 + 窗口唯一数）
    {
        std::vector<double> vals = s.v;   // 非角度列也能看周期（如 angleOffset 的循环）
        int bestLag = 0;
        double bestPeak = 0.0;
        autocorrelation(vals, opt.maxLag, bestLag, bestPeak);
        out.period = bestLag;
        out.periodPeak = bestPeak;

        std::vector<uint64_t> bits;
        const size_t wcap = std::min<size_t>(s.v.size(), 65536);
        bits.reserve(wcap);
        for (size_t i = 0; i < wcap; ++i) bits.push_back(bitsOf(s.v[i]));

        static const int kLags[] = {2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96,
                                    128, 192, 256, 384, 512, 768, 1024};
        // 选周期：**自相关主峰优先**。为什么不直接取"唯一数最少"的 lag —— 周期 96 的
        // 数据在 p=2 上同样只有 96 个不同的二元窗口（唯一数相同、比值相同），
        // 于是"最少"会退化成"最先试到的那个"，报出 p=2 这种没用的答案。
        // 自相关主峰问的才是"数据本身的周期是多少"，窗口唯一数接着证明它确实逐位重复。
        auto measure = [&](int p) {
            uint64_t wc = 0;
            const uint64_t uniq = windowUnique(bits, p, &wc);
            return std::pair<uint64_t, uint64_t>(uniq, wc);
        };
        auto better = [](uint64_t u1, uint64_t c1, uint64_t u2, uint64_t c2) {
            // 唯一数占比更小者胜（用交叉相乘，避免浮点）
            return c1 != 0 && (c2 == 0 || u1 * c2 < u2 * c1);
        };
        if (bestLag >= 2 && bestLag <= opt.maxLag) {
            const auto r = measure(bestLag);
            if (r.second != 0 && r.first * 4 <= r.second) {
                out.windowUnique = r.first;
                out.windowCount = r.second;
                out.motifPeriod = bestLag;
            }
        }
        for (int p : kLags) {
            if (p < 2 || p > opt.maxLag || (size_t)p * 8 > bits.size()) continue;
            if (out.motifPeriod == p) continue;
            const auto r = measure(p);
            if (r.second == 0 || r.first * 4 > r.second) continue;   // 塌陷不到 4x 不算
            // 只有当"唯一数占比"明显更低（差 20% 以上）时才顶掉自相关主峰 ——
            // 周期 96 的数据在 p=2 上唯一数**一样**是 96（只是窗口总数多 94 个），
            // 严格比比值会让 p=2 这种没用的答案赢。
            const bool clearlyBetter = out.motifPeriod != 0 &&
                                       r.first * out.windowCount * 5 <
                                           out.windowUnique * r.second * 4;
            if (out.motifPeriod == 0 || clearlyBetter) {
                out.windowUnique = r.first;
                out.windowCount = r.second;
                out.motifPeriod = p;
            }
        }
        if (out.motifPeriod != 0 && out.windowUnique * 4 > out.windowCount) {
            out.motifPeriod = 0;   // 兜底：塌陷不到 4x 就不算 Motif
        }
    }

    // ---- 裁决：估算字节/值，取最小 ----
    const double rawBytes = 8.0;
    Choice best{"Raw", rawBytes, "没有可利用的结构"};
    auto consider = [&](const Choice& c) {
        if (c.bytesPerValue < best.bytesPerValue) best = c;
    };
    if (out.distinct == 1) {
        best = Choice{"Const", 0.0, "整列一个值"};
    } else {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "值集 %llu（%llu bit 下标）",
                      (unsigned long long)out.distinct,
                      (unsigned long long)bitWidthFor(out.distinct));
        consider(Choice{"Dict", dictBytes(out.n, out.distinct, 8.0), buf});
        if (out.runs * 4 <= out.n && out.distinct * 16 <= out.n) {
            // v2 的 Rle = 字典 + (下标, 游程长度) 对，不是"把游程也塞进字典"。
            // 实测校准：diag_dict 的 angleData 是 7462 B（runs=3677、distinct=10）
            // → 10*8 + 3677*(1+1) = 7434 B，对得上。
            std::snprintf(buf, sizeof(buf), "游程 %llu / %llu 值（字典 %llu 项 + 游程对）",
                          (unsigned long long)out.runs, (unsigned long long)out.n,
                          (unsigned long long)out.distinct);
            const double bytes = (double)out.distinct * 8.0 +
                                 (double)out.runs * (double)(bitWidthFor(out.distinct) + 7) / 8.0 * 2.0;
            consider(Choice{"Rle", bytes / (double)out.n, buf});
        }
        if (out.d1Raw) {
            std::snprintf(buf, sizeof(buf), "一阶差分基数 %llu", (unsigned long long)out.d1Raw);
            consider(Choice{"Delta\u2192Dict", dictBytes(out.n, out.d1Raw, 8.0), buf});
        }
        if (spec.angular && out.d1Wrapped && out.d1Wrapped * 4 <= out.d1Raw) {
            std::snprintf(buf, sizeof(buf), "wrapped 一阶基数 %llu vs raw %llu",
                          (unsigned long long)out.d1Wrapped, (unsigned long long)out.d1Raw);
            consider(Choice{"WrapDelta(mod360)\u2192Dict", dictBytes(out.n, out.d1Wrapped, 8.0), buf});
        }
        if (out.gridScale > 0.0 && out.d2WrapBytes > 0.0 &&
            out.d2WrapBytes * 5.0 <= out.d2RawBytes * 4.0) {
            std::snprintf(buf, sizeof(buf),
                          "整数域 wrapped Δ² 的 |max| %lld / varint %.2f B，raw 是 %lld / %.2f B",
                          (long long)out.d2WrapMax, out.d2WrapBytes,
                          (long long)out.d2RawMax, out.d2RawBytes);
            consider(Choice{"WrapDelta(mod360)\u2192Delta2\u2192Varint", out.d2WrapBytes, buf});
        }
        if (spec.angular && out.d2Wrapped && out.d2Wrapped * 4 <= out.d2Raw) {
            std::snprintf(buf, sizeof(buf), "wrapped 二阶基数 %llu vs raw %llu（尖峰被抹平）",
                          (unsigned long long)out.d2Wrapped, (unsigned long long)out.d2Raw);
            consider(Choice{"WrapDelta(mod360)\u2192Delta2\u2192Dict",
                            dictBytes(out.n, out.d2Wrapped, 8.0), buf});
        }
        if (out.motifPeriod && out.windowUnique) {
            // 窗口字典本体是 windowUnique × p 个值，必须算进去（否则 Motif 会被高估）
            const double body = (double)out.windowUnique * (double)out.motifPeriod * 8.0;
            const double idx = (double)out.windowCount * (double)bitWidthFor(out.windowUnique) / 8.0;
            const double bpv = (body + idx) / (double)out.n;
            std::snprintf(buf, sizeof(buf), "窗口唯一 %llu / %llu（p=%d，字典本体占 %.2f B/值）",
                          (unsigned long long)out.windowUnique, (unsigned long long)out.windowCount,
                          out.motifPeriod, body / (double)out.n);
            consider(Choice{"Motif", bpv, buf});
        }
        if (out.gridExp2 >= 0 || out.gridExp10 >= 0) {
            char g[64];
            if (out.gridExp2 >= 0) std::snprintf(g, sizeof(g), "2^-%d", out.gridExp2);
            else std::snprintf(g, sizeof(g), "10^-%d", out.gridExp10);
            std::snprintf(buf, sizeof(buf), "落在 %s 网格上（整数值域要 %u bit）", g,
                          (unsigned)(fixedPointBytesPerValue(s.v, out.gridExp2, out.gridExp10) * 8.0));
            consider(Choice{"FixedPoint",
                            fixedPointBytesPerValue(s.v, out.gridExp2, out.gridExp10), buf});
        }
    }
    out.verdict = best.name;
    out.note = best.why;
    if (out.gridExp2 >= 0) out.note += "；二进制网格 2^-" + std::to_string(out.gridExp2);
    else if (out.gridExp10 >= 0) out.note += "；十进制网格 10^-" + std::to_string(out.gridExp10);
}

void diagnoseInts(const std::vector<int64_t>& v, const DiagOptions& opt, ColumnDiagnostics& out) {
    out = ColumnDiagnostics{};
    out.nTotal = (uint64_t)v.size();
    const uint64_t take = std::min<uint64_t>(out.nTotal, opt.maxSample);
    out.n = take;
    if (take == 0) { out.verdict = "Const"; return; }

    std::vector<uint64_t> b;
    b.reserve((size_t)take);
    for (uint64_t i = 0; i < take; ++i) b.push_back((uint64_t)v[(size_t)i]);
    out.runs = 1;
    for (size_t i = 1; i < b.size(); ++i)
        if (b[i] != b[i - 1]) ++out.runs;
    out.distinct = distinctOf(b);

    std::vector<uint64_t> d1, d2;
    d1.reserve(b.size()); d2.reserve(b.size());
    int64_t prevD = 0;
    for (uint64_t i = 0; i < take; ++i) {
        if (i > 0) {
            const int64_t d = v[(size_t)i] - v[(size_t)i - 1];
            d1.push_back((uint64_t)d);
            if (i > 1) d2.push_back((uint64_t)(d - prevD));
            prevD = d;
        }
    }
    out.d1Raw = distinctOf(d1);
    out.d2Raw = distinctOf(d2);
    out.d1Wrapped = out.d1Raw;
    out.d2Wrapped = out.d2Raw;

    out.allIntegers = true;
    {
        std::vector<double> asDouble;
        const size_t probe = std::min<size_t>((size_t)take, 4096);
        asDouble.reserve(probe);
        for (size_t i = 0; i < probe; ++i) asDouble.push_back((double)v[i]);
        out.gridExp2 = gridPowerOfTwo(asDouble);
        out.gridExp10 = gridPowerOfTen(asDouble);
    }

    {
        std::vector<double> vals;
        vals.reserve((size_t)take);
        for (uint64_t i = 0; i < take; ++i) vals.push_back((double)v[(size_t)i]);
        int bestLag = 0;
        double bestPeak = 0.0;
        autocorrelation(vals, opt.maxLag, bestLag, bestPeak);
        out.period = bestLag;
        out.periodPeak = bestPeak;
    }

    if (out.distinct == 1) {
        out.verdict = "Const";
        out.note = "整列一个值";
        return;
    }
    Choice best{"Raw", 8.0, "没有可利用的结构"};
    auto consider = [&](const Choice& c) {
        if (c.bytesPerValue < best.bytesPerValue) best = c;
    };
    char buf[160];
    std::snprintf(buf, sizeof(buf), "值集 %llu", (unsigned long long)out.distinct);
    consider(Choice{"Dict", dictBytes(out.n, out.distinct, 8.0), buf});
    if (out.d1Raw) {
        std::snprintf(buf, sizeof(buf), "一阶差分基数 %llu", (unsigned long long)out.d1Raw);
        consider(Choice{"DeltaVarint", dictBytes(out.n, out.d1Raw, 8.0), buf});
    }
    if (out.d1Raw == 1) {
        best = Choice{"DeltaRleVarint", 0.05, "差分恒定（floor 那类）"};
    }
    out.verdict = best.name;
    out.note = best.why;
}

void diagnoseU32s(const std::vector<uint32_t>& v, const DiagOptions& opt, ColumnDiagnostics& out) {
    std::vector<int64_t> tmp;
    tmp.reserve(v.size());
    for (uint32_t x : v) tmp.push_back((int64_t)(uint64_t)x);
    diagnoseInts(tmp, opt, out);
    // u32 的朴素表示是 4 B/值，不是 8 —— 估算里改回来（裁决在 diagnoseInts 里已经算过，
    // 这里只把"Raw 兜底"的数字改对，免得报告自相矛盾）
    if (std::strcmp(out.verdict, "Raw") == 0) out.verdict = "Raw(u32)";
}

double spatialOverlap(const std::vector<std::pair<double, double>>& xy, double cell) {
    if (xy.empty()) return 0.0;
    if (cell <= 0.0) cell = 1.0;
    std::vector<uint64_t> keys;
    keys.reserve(xy.size());
    for (const auto& p : xy) {
        const int64_t cx = (int64_t)std::floor(p.first / cell);
        const int64_t cy = (int64_t)std::floor(p.second / cell);
        // 两个 32 位坐标拼成一个 key（±2^31 格 = ±2e9 砖，够用到 21 亿层目标）
        keys.push_back(((uint64_t)(uint32_t)cx << 32) | (uint64_t)(uint32_t)cy);
    }
    const uint64_t total = keys.size();
    const uint64_t uniq = distinctOf(keys);
    return total ? 1.0 - (double)uniq / (double)total : 0.0;
}

bool diagnoseLevel(const LevelData& level, const DiagOptions& opt,
                   std::vector<ColumnDiagnostics>& out, std::string& err) {
    out.clear();
    err.clear();

    ColumnSpec angular;
    angular.angular = true;
    angular.period = 360.0;
    // midspin 的哨兵值：与 core 侧一致（angleData == 999 = 上一层 + 180）
    angular.sentinel = 999.0;

    {
        ColumnDiagnostics d;
        diagnoseDoubles(level.angleData, angular, opt, d);
        d.name = "angleData";
        out.push_back(std::move(d));
    }

    if (!level.actions.empty()) {
        const size_t n = level.actions.size();
        std::vector<int64_t> floors;
        std::vector<uint32_t> types, strIds, flags, v1, v2;
        floors.reserve(n); types.reserve(n); strIds.reserve(n); flags.reserve(n);
        v1.reserve(n); v2.reserve(n);
        for (const LevelData::FastAction& a : level.actions) {
            floors.push_back((int64_t)a.floor);
            types.push_back((uint32_t)a.type);
            strIds.push_back((uint32_t)a.strId);
            flags.push_back(a.flag ? 1u : 0u);
            uint32_t x = 0;
            std::memcpy(&x, &a.val1, 4); v1.push_back(x);
            std::memcpy(&x, &a.val2, 4); v2.push_back(x);
        }
        auto pushI = [&](const char* name, const std::vector<int64_t>& col) {
            ColumnDiagnostics d;
            diagnoseInts(col, opt, d);
            d.name = name;
            out.push_back(std::move(d));
        };
        auto pushU = [&](const char* name, const std::vector<uint32_t>& col) {
            ColumnDiagnostics d;
            diagnoseU32s(col, opt, d);
            d.name = name;
            out.push_back(std::move(d));
        };
        pushI("actions.floor", floors);
        pushU("actions.type", types);
        pushU("actions.strId", strIds);
        pushU("actions.flag", flags);
        pushU("actions.val1", v1);
        pushU("actions.val2", v2);
    }

    // 空间自重叠率（叠加型）：砖位落进 1 砖格子的重复度
    if (!level.tiles.empty()) {
        std::vector<std::pair<double, double>> xy;
        const uint64_t take = std::min<uint64_t>(level.tiles.size(), opt.maxSample);
        xy.reserve((size_t)take);
        for (uint64_t i = 0; i < take; ++i)
            xy.emplace_back(level.tiles[(size_t)i].position[0], level.tiles[(size_t)i].position[1]);
        ColumnDiagnostics d;
        d.name = "tiles.position";
        d.n = take;
        d.nTotal = level.tiles.size();
        d.overlap = spatialOverlap(xy, 1.0);
        // ⚠️ 重叠率高只说明"空间被反复访问"，**不等于**"单元 × 摆放"那种叠加型：
        // 绕圈的滑音（黄金角螺旋）也会把同一批格子踩上千遍 —— 实测 dia_glide 的重叠率
        // 就是 0.9996。要区分得看**位置序列的周期**（窗口唯一数），所以这里只报事实。
        d.verdict = d.overlap > 0.5 ? "revisit-heavy" : "spread";
        d.note = d.overlap > 0.5
                     ? "空间被反复访问（叠加型 **或** 绕圈的滑音，两者要靠位置序列的周期区分）"
                     : "砖位铺得开";
        out.push_back(std::move(d));
    }
    return true;
}

}  // namespace adocao
}  // namespace adofai
