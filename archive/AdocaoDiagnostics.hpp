#pragma once

// `.adocao` v3 的**判据统计**（`docs/adocao-format.md` §8.4）。
//
// 为什么要它：现在编码器是"每个候选真编一遍取最小"——稳，但**看不见"为什么"**。
// 于是每加一个变换/后端都只能靠合成数据猜它该不该赢。这里把"这一列长什么样"
// 量出来，让判据（和它的失败）可观测。
//
// 两条纪律：
//   1. **纯诊断，不改行为**：本模块不参与编码，`--codec-report` 之外没人调它。
//   2. **只在掩码后的流上算环绕类统计**：`angleData` 里混着 midspin 的 `999`
//      （`LevelData.cpp` 的 `angleData[i] == 999`），`999 mod 360 = 279` 是假角度，
//      999↔真角度的相邻对还会造出一大批假的 |Δ|>180 —— 不掩码的话"环绕率"
//      会指向一个不存在的结构（见 §8.1 的哨兵铁律、§8.10 的负向对照 ⑦）。
//
// **采样的规矩**：`maxSample` 之外只取**前缀**（不用跨步抽样）—— 跨步会破坏"相邻对"
// 语义，而环绕率/差分基数全都建立在相邻对上。报告里必须写出 `sampled from N`。

#include <cstdint>
#include <string>
#include <vector>

namespace adofai {

struct LevelData;

namespace adocao {

// 一列的量纲：角度列要环绕/哨兵，其它列（floor、type、float 位模式）不用。
struct ColumnSpec {
    bool   angular = false;   // true → 按"环上的量"处理
    double period  = 360.0;   // 环的模数
    double sentinel = 999.0;  // midspin 哨兵（只在 angular 时有意义）
};

struct DiagOptions {
    // 统计上限（前缀采样）。默认 1<<17：1e6 量级的谱也能在 ~0.2 s 内报完，
    // 而周期搜索上限 1024 需要 n >> 1024 才有意义（这里给到 128 个周期）。
    uint64_t maxSample = (uint64_t)1 << 17;
    int      maxLag    = 1024;   // 自相关/窗口唯一数搜索的周期上限
};

// 一列的判据。默认字段全是"没结构"的那一档，所以"什么都没填"= 该走 Raw。
struct ColumnDiagnostics {
    std::string name;
    uint64_t n = 0;             // 采样后参与统计的值数
    uint64_t nTotal = 0;        // 原始值数（> n 说明是前缀采样）
    uint64_t sentinels = 0;     // 哨兵个数（angular 列才有）
    uint64_t sentinelRuns = 0;  // 哨兵是否连成片（>0 且远小于个数 → 连续 midspin 段）

    uint64_t distinct = 0;      // 值集基数（double 按**位模式**，与编码器一致）
    uint64_t runs = 0;          // 相邻相等的段数（游程数；runs << n → Rle 有戏）

    uint64_t d1Raw = 0, d1Wrapped = 0;   // 一阶差分后的基数（wrapped 只对 angular 有意义）
    uint64_t d2Raw = 0, d2Wrapped = 0;   // 二阶差分后的基数 ← **决定性**的那对
    // 一阶差分里 |Δ| > period/2 的比例：即"跨 0/360 的跳变"占比。
    // 注意它**单独不足以**下结论：慢滑音（Δ=0.37°/层）的环绕率只有 ~0.1%，
    // 但它的 wrapped Δ² 会从 ~3.6e2 的尖峰塌成 ~1e-6 —— 真正的判据是下面那对基数。
    double wrapRateRaw = 0.0, wrapRateMasked = 0.0;

    // 定点网格：只有**粗**网格才算结构（上限见 .cpp 的 kCoarseGridExp2 说明：
    // 任何 double 都落在某个 2^-N 上，不设限的话每一列都会"落在网格上"）
    int gridExp2 = -1;          // 全体 v·2^e ∈ ℤ 的最小 e（>20 视作"不是粗网格"）
    int gridExp10 = -1;         // 全体 v·10^e ∈ k/10^e（**精确**判定，不是容差）
    double gridScale = 0.0;     // 可用的精确整数域 scale（0 = 没有 → 残差只能在浮点域看）
    bool allIntegers = false;   // 值域上全是整数（floor 那类）

    // **整数域**的二阶差分（§8.2 ①：残差只在整数域里算）—— 这一对才是 WrapDelta 的真实收益。
    // 浮点域的 Δ² 会被量化噪声主导（4 位小数的谱：真曲率 1e-7 被 ±1e-4 的舍入噪声淹没），
    // 于是"基数塌陷"看起来只有 2~3 倍；整数域里同样的数据是 ±3600000 的尖峰 vs ±40 的小值。
    int64_t d2RawMax = 0, d2WrapMax = 0;     // 整数域二阶差分的最大绝对值
    double  d2RawBytes = 0.0, d2WrapBytes = 0.0;  // zigzag varint 的平均字节/值

    int    period = 0;          // 自相关主峰所在的 lag
    double periodPeak = 0.0;    // 归一化自相关峰值 [-1,1]
    uint64_t windowUnique = 0;  // 该周期下**逐位相同**的窗口个数
    uint64_t windowCount = 0;   // 参与比较的窗口总数（n - p + 1）
    int    motifPeriod = 0;     // 窗口唯一数塌陷最狠的周期（0 = 没塌陷）

    double overlap = 0.0;       // 空间自重叠率（只有传了坐标才有意义）

    const char* verdict = "raw";     // 建议（人读，不参与决策）
    std::string note;                // 一句话说明为什么
};

// ---- 纯函数（可单独测；不碰文件、不碰编码器）----

// 哨兵掩码：`masked` 只收非哨兵值，`bitmap` 每 8 个值 1 字节（bit i%8 = 第 i 个值是否是哨兵）。
// 返回哨兵个数。**逐位不变量**：masked 里的值与原值完全相同（原样拷贝，不做任何算术）。
uint64_t maskSentinels(const std::vector<double>& v, const ColumnSpec& spec,
                       std::vector<double>& masked, std::vector<uint8_t>& bitmap);

// 单列判据（double 按位模式比较；angular 列会先掩码再算环绕统计）。
void diagnoseDoubles(const std::vector<double>& v, const ColumnSpec& spec,
                     const DiagOptions& opt, ColumnDiagnostics& out);

// 整数列（floor / type 等）：没有环绕/哨兵，但基数、差分基数、游程、周期同样有意义。
void diagnoseInts(const std::vector<int64_t>& v, const DiagOptions& opt, ColumnDiagnostics& out);

// u32 列的薄封装（值域 u32，内部按 int64 处理）。
void diagnoseU32s(const std::vector<uint32_t>& v, const DiagOptions& opt, ColumnDiagnostics& out);

// 空间自重叠率：把坐标按 `cell` 量化进格子，返回"落进已占用格子"的比例 [0,1]。
// 高 → 叠加型（网格摆放 × 单元），也就是 §5.5 那种"信息量只有几百 KB"的谱。
double spatialOverlap(const std::vector<std::pair<double, double>>& xy, double cell);

// 把一整个 LevelData 的列都过一遍（CLI 的 `--codec-report` 直接用它）。
// 列的切法与写入方一致（angleData / actions.floor / type / strId / flag / val1 / val2），
// 但**只读不写**：不产出任何编码决策。
bool diagnoseLevel(const LevelData& level, const DiagOptions& opt,
                   std::vector<ColumnDiagnostics>& out, std::string& err);

}  // namespace adocao
}  // namespace adofai
