// `.adocao` v3 判据统计（§8.4）自测：**哨兵掩码 + 差分基数**是这一步的全部价值。
//
// 为什么断言长这样：这一步的产物是"人看的判据"，最容易变成"看起来很有道理"的废话。
// 所以每条断言都对着一个**会发生的事**：
//   * 滑音：raw 的 Δ² 带 ±360 尖峰 → wrapped Δ² 必须塌陷（这是 §8.1 里 WrapDelta 的正当性）；
//   * 中旋谱：不掩码的环绕率被哨兵抬高 → 掩码后必须回落（**负向对照**，证明掩码不是装饰）；
//   * 不规则谱：值集基数 ≈ n、且判据**不许**说出 Dict/WrapDelta 这类好听话；
//   * 傅里叶（量化到 1e-6、公共周期 96）：窗口唯一数必须真的塌到 96 —— 也就是说
//     **逐位重复**才是 Motif 的前提，全精度的正弦永远不塌（那条也断言）。

#include "archive/AdocaoDiagnostics.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace adofai;
using namespace adofai::adocao;

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

ColumnSpec angularSpec() {
    ColumnSpec s;
    s.angular = true;
    s.period = 360.0;
    s.sentinel = 999.0;
    return s;
}

double mod360(double x) {
    double r = std::fmod(x, 360.0);
    if (r < 0.0) r += 360.0;
    return r;
}

// 与 tests/gen_adocao_charts.py 同一套公式（脚本改了这里也要改：两处必须一致）
std::vector<double> glide(int n, double base = 0.0, double amp = 3.0, double period = 5000.0) {
    std::vector<double> out;
    out.reserve((size_t)n);
    double ang = base;
    for (int i = 0; i < n; ++i) {
        // 137.508°（黄金角）= 每 2~3 层跨一次 0/360，且 360/137.508 不是有理数 → 值几乎不重复
        const double d = 137.508 + amp * std::sin(2.0 * 3.14159265358979323846 * (double)i / period);
        ang = mod360(ang + d);
        out.push_back(std::round(ang * 10000.0) / 10000.0);
    }
    return out;
}

// 纯律大七和弦 C-E-G-B：谐波号 2/4/8/10/12/15，公共周期 120（= 120/h），与生成脚本一致
std::vector<double> fourier(int n, bool quantize) {
    const double periods[6] = {60.0, 30.0, 15.0, 12.0, 10.0, 8.0};
    const double amps[6] = {30.0, 18.0, 24.0, 20.0, 14.0, 10.0};
    std::vector<double> out;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        double a = 180.0;
        for (int k = 0; k < 6; ++k)
            a += amps[k] * std::sin(2.0 * 3.14159265358979323846 * (double)i / periods[k]);
        a = mod360(a);
        out.push_back(quantize ? std::round(a * 1e6) / 1e6 : a);
    }
    return out;
}

// 确定性 LCG（与生成脚本同一个），避免测试依赖 libc 的 rand()
uint64_t g_state = 0x3303;
double nextUniform(double lo, double hi) {
    g_state = g_state * 6364136223846793005ull + 1442695040888963407ull;
    const uint32_t x = (uint32_t)(g_state >> 32);   // 高 32 位（低 31 位只覆盖半量程）
    return lo + (hi - lo) * ((double)x / 4294967296.0);
}

}  // namespace

int main() {
    const ColumnSpec ang = angularSpec();
    DiagOptions opt;   // 默认 maxSample = 1<<17，够这些谱用

    // ① 掩码本身：逐位不动、位图对得上
    {
        const std::vector<double> v = {10.0, 999.0, 20.0, 999.0, 999.0, 30.0, -0.0};
        std::vector<double> masked;
        std::vector<uint8_t> bitmap;
        const uint64_t sent = maskSentinels(v, ang, masked, bitmap);
        check(sent == 3, "哨兵计数 = 3");
        check(masked.size() == 4, "掩码后剩 4 个真值");
        check(masked[0] == 10.0 && masked[1] == 20.0 && masked[2] == 30.0, "掩码后顺序不变");
        // -0.0 必须原样带过来（不是 0.0）—— 字典是按位模式建的
        check(std::memcmp(&masked[3], &v[6], 8) == 0, "-0.0 原样通过（按位不动）");
        check((bitmap[0] & 0x02) && (bitmap[0] & 0x08) && (bitmap[0] & 0x10), "位图标中第 1/3/4 个");
        // 非角度列：999 就是普普通通一个值
        ColumnSpec plain;
        ColumnDiagnostics pd;
        diagnoseDoubles(v, plain, opt, pd);
        check(pd.sentinels == 0, "非角度列不把 999 当哨兵");
    }

    // ② 滑音：wrapped 的 Δ² 必须塌陷（WrapDelta 的正当性）
    {
        const int n = 16384;
        const std::vector<double> v = glide(n);
        ColumnDiagnostics d;
        diagnoseDoubles(v, ang, opt, d);
        std::printf("     滑音：值集 %llu，Δ基数 %llu/%llu，Δ²基数 %llu/%llu，环绕率 %.4f\n",
                    (unsigned long long)d.distinct, (unsigned long long)d.d1Raw,
                    (unsigned long long)d.d1Wrapped, (unsigned long long)d.d2Raw,
                    (unsigned long long)d.d2Wrapped, d.wrapRateMasked);
        check(d.sentinels == 0, "滑音谱里没有哨兵");
        check(d.distinct > (uint64_t)n / 2, "滑音的值集基数很大（Dict 该输）");
        check(d.wrapRateMasked > 0.20, "黄金角滑音的环绕率很高（每 2~3 层跨一次 0/360）");
        // 浮点域看着"只塌 2~3 倍"：4 位小数的谱里真曲率（~1e-7）被 ±1e-4 的舍入噪声淹没。
        // 这不是数据没结构，而是**在错误的域里看** —— 所以这条断言是"浮点域确实会骗人"，
        // 下面那对整数域的数字才是 WrapDelta 的收益。
        check(d.d2Wrapped * 4 > d.d2Raw, "浮点域的 Δ² 基数几乎不塌（量化噪声主导）");
        // 整数域（scale=1e4）：raw Δ² 是 ±3.6e6 的跨环尖峰，wrapped 只剩调制的那点曲率
        check(d.gridScale == 10000.0, "4 位小数的谱有精确整数域（scale=1e4）");
        check(d.d2WrapBytes * 2 <= d.d2RawBytes, "整数域里 wrapped Δ² 的 varint 至少省一半");
        check(d.d2WrapMax * 100 <= d.d2RawMax, "整数域 |Δ²|max 塌 100 倍以上（跨环尖峰没了）");
        check(std::strncmp(d.verdict, "WrapDelta", 9) == 0, "判据指向 WrapDelta");
    }

    // ③ 中旋哨兵：**负向对照** —— 不掩码的环绕率被抬高，掩码后必须回落
    {
        const int n = 16384;
        std::vector<double> v = glide(n);
        for (int i = 3; i < n; i += 8) v[(size_t)i] = 999.0;
        ColumnDiagnostics d;
        diagnoseDoubles(v, ang, opt, d);
        std::printf("     中旋：哨兵 %llu，环绕率 raw %.4f → masked %.4f，Δ²基数 %llu/%llu\n",
                    (unsigned long long)d.sentinels, d.wrapRateRaw, d.wrapRateMasked,
                    (unsigned long long)d.d2Raw, (unsigned long long)d.d2Wrapped);
        check(d.sentinels == 2048, "哨兵个数 = n/8");
        // 哨兵每 8 层一个：raw 的相邻对里 2/8 与 999 有关，环绕率被抬到 ~25%
        check(d.wrapRateRaw > 0.20, "不掩码时环绕率被哨兵抬到 20% 以上（假象）");
        // 掩码后剩下的就是黄金角滑音自己的环绕率（~0.38），所以这里的判据是
        // "哨兵那一份被去掉了"而不是"环绕率变低"：raw - masked ≈ 2/8。
        // 每个哨兵牵动 2 个相邻对（2/8 = 25%），其中一部分本来也是环绕对 → 差 ≈ 0.15~0.20
        check(d.wrapRateRaw - d.wrapRateMasked > 0.12, "raw 比 masked 高出的正是哨兵那一份");
        check(d.wrapRateMasked > 0.30 && d.wrapRateMasked < 0.45,
              "掩码后回落到滑音的真实环绕率（~0.38）");
    }

    // ④ 不规则：判据必须**说不出好听话**（这正是"地板由生成器决定"）
    {
        const int n = 8192;
        std::vector<double> v;
        v.reserve((size_t)n);
        for (int i = 0; i < n; ++i) v.push_back(nextUniform(0.0, 360.0));
        ColumnDiagnostics d;
        diagnoseDoubles(v, ang, opt, d);
        std::printf("     不规则：值集 %llu / %llu，Δ基数 %llu，判据 %s\n",
                    (unsigned long long)d.distinct, (unsigned long long)d.n,
                    (unsigned long long)d.d1Raw, d.verdict);
        check(d.distinct > (uint64_t)n * 99 / 100, "值集基数 ≈ n（全精度、几乎不重复）");
        check(std::strcmp(d.verdict, "Raw") == 0, "判据落在 Raw（没结构就说没结构）");
        check(d.d1Raw > (uint64_t)n / 4, "一阶差分的基数也很大（差分救不了它）");
        // **负向对照**：这些"随机"值是 k/2^29（脚本用 32 位整数造的），所以它们**确实**
        // 落在 2^-29 网格上 —— 但那是"任何 double 都在某个 2^-N 上"的退化情形（真正在
        // 起作用的只是尾数），判据必须把它挡在"粗网格"之外，否则 FixedPoint 会用
        // 1.2× 的小便宜把真正的结构挤出裁决。
        check(d.gridExp2 < 0, "不把 2^-29 这种退化网格当成结构（粗网格上限 2^-20）");
        check(d.gridExp10 < 0, "十进制网格判定也不许误报");
    }

    // ⑤ 傅里叶（大七和弦）：量化到 1e-6、公共周期 120 → 窗口唯一数真的塌到 120
    {
        const int n = 16384;
        const std::vector<double> q = fourier(n, true);
        ColumnDiagnostics d;
        diagnoseDoubles(q, ang, opt, d);
        std::printf("     傅里叶/大七和弦（量化）：值集 %llu，窗口唯一 %llu/%llu@p%d，周期 %d(%.3f)\n",
                    (unsigned long long)d.distinct, (unsigned long long)d.windowUnique,
                    (unsigned long long)d.windowCount, d.motifPeriod, d.period, d.periodPeak);
        check(d.motifPeriod == 120, "窗口唯一数塌在 p=120（大七和弦的公共周期）");
        check(d.windowUnique <= 160, "窗口唯一数 ≤ 160（真的在逐位重复）");

        // 同一份波形**不量化**：每层差 1 ULP → 窗口永不重复。这是"Motif 的前提"的负向对照。
        const std::vector<double> raw = fourier(n, false);
        ColumnDiagnostics dr;
        diagnoseDoubles(raw, ang, opt, dr);
        std::printf("     傅里叶（全精度）：值集 %llu，窗口唯一 %llu/%llu@p%d\n",
                    (unsigned long long)dr.distinct, (unsigned long long)dr.windowUnique,
                    (unsigned long long)dr.windowCount, dr.motifPeriod);
        check(dr.distinct > 4000, "全精度下值集远大于量化版（~105）");
        check(d.gridExp10 == 6, "量化到 1e-6 → 判成 10^-6 网格");
        check(dr.motifPeriod == 0, "全精度下没有可用的 Motif 周期（逐位重复才是前提）");
    }

    // ⑥ 小字典列：Dict 该赢，且游程/差分都不能压过它
    {
        std::vector<double> v;
        const double alphabet[10] = {0.0, 22.5, 45.0, 67.5, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0};
        // 顺序必须是**看不出周期**的：i*7%10 那种循环会让 Δ 只剩 7 个取值，
        // 于是"Delta→Dict"真的比"Dict"更省 —— 那时判据报 Delta→Dict 是对的。
        for (int i = 0; i < 16384; ++i)
            v.push_back(alphabet[(int)(nextUniform(0.0, 9.999) )]);
        ColumnDiagnostics d;
        diagnoseDoubles(v, ang, opt, d);
        std::printf("     字典列：值集 %llu，判据 %s（%s）\n", (unsigned long long)d.distinct,
                    d.verdict, d.note.c_str());
        check(d.distinct == 10, "值集 = 10");
        check(std::strcmp(d.verdict, "Dict") == 0, "判据指向 Dict");
    }

    // ⑦ 定长/游程/网格这些"平凡"判据也别写错
    {
        // floor 那种列：差分恒 +1
        std::vector<int64_t> floors;
        for (int i = 0; i < 5000; ++i) floors.push_back(10 + i * 3);
        ColumnDiagnostics d;
        diagnoseInts(floors, opt, d);
        std::printf("     floor 列：值集 %llu，一阶差分基数 %llu，判据 %s\n",
                    (unsigned long long)d.distinct, (unsigned long long)d.d1Raw, d.verdict);
        check(d.distinct == 5000 && d.d1Raw == 1, "差分恒定 → 一阶差分基数 1");
        check(std::strcmp(d.verdict, "DeltaRleVarint") == 0, "差分恒定的列判给 DeltaRleVarint");

        // 二进制网格：22.5 → 2^-3；十进制网格：0.1（二进制里永远不是有限小数，启发式兜）
        std::vector<double> g2 = {22.5, 45.0, 67.5, 90.0, 112.5, 135.0, 157.5, 180.0};
        ColumnDiagnostics d2;
        diagnoseDoubles(g2, ang, opt, d2);
        check(d2.gridExp2 == 1, "22.5° 落在 2^-1 网格（22.5x2 = 45 已经是整数）");
        std::vector<double> g3 = {11.25, 22.5, 33.75, 45.0};
        ColumnDiagnostics d3;
        diagnoseDoubles(g3, ang, opt, d3);
        check(d3.gridExp2 == 2, "11.25° 落在 2^-2 网格");
        std::vector<double> g10 = {0.1, 0.2, 0.3, 0.4, 1.5, 2.7};
        ColumnDiagnostics d10;
        diagnoseDoubles(g10, ang, opt, d10);
        check(d10.gridExp2 < 0, "0.1 不是粗二进制网格（不许误报）");
        check(d10.gridExp10 == 1, "0.1 判成 10^-1 网格（精确判定：k/10^e == v）");

        // 空间自重叠率：4 个砖位反复重访 → 接近 1；铺开 → 接近 0
        std::vector<std::pair<double, double>> loop;
        for (int i = 0; i < 1000; ++i) {
            loop.emplace_back((double)(i % 4), (double)(i % 4) * 0.0);
        }
        const double ov = spatialOverlap(loop, 1.0);
        check(ov > 0.99, "反复回到同一批格子 → 重叠率 ≈ 1");
        std::vector<std::pair<double, double>> spread;
        for (int i = 0; i < 1000; ++i) spread.emplace_back((double)i, 0.0);
        check(spatialOverlap(spread, 1.0) < 0.01, "一路铺开 → 重叠率 ≈ 0");
        check(spatialOverlap({}, 1.0) == 0.0, "空输入不炸");
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
