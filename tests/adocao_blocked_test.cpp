// `.adocao` v3 分块列容器自测（`archive/AdocaoBlocked.*`，`docs/adocao-format.md` §8.3 ⑥）。
//
// 这条线的价值有两个，缺一不可：
//   ① **逐位无损 + 坏数据干净失败** —— 与列编码器同一条硬规矩（§8.0）；
//   ② **分块真的赚到**（分段型收益）**且不会让任何谱变大**（最坏零收益，§8.0-2）。
//      所以除了正向收益，还有两个负向对照：同质列上分块**不许**赢，白噪声列上也不许赢。
//
// 关键口径：单块 vs 分块的分界在**段级**（见 AdocaoWriter）—— v2 把整个段压成一帧 zstd，
// 比"每列各压一帧"更省。所以分块的比较基线取"单块 + 自压"（`ColumnLayoutStats::singleBytes`），
// 与生产路径一致。

#include "archive/AdocaoBlocked.hpp"
#include "archive/AdocaoColumns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace adofai;
using namespace adofai::adocao;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) std::printf("ok   %s\n", what.c_str());
    else { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---- 合成列 ------------------------------------------------------------------

// "平滑"块：闭合方块（0/90/180/270 循环）—— 块内编码器只要 4 个字典项就能吃掉整块
//（≈0.25 B/值）。选它是因为 §8.3 量的是"局部同质、全局异质"：块内自选的赢面必须来自
//"块内结构简单"，而不是靠块间重复。
void fillSmooth(std::vector<double>& v, size_t from, size_t to) {
    static const double kCycle[4] = {0.0, 90.0, 180.0, 270.0};
    for (size_t i = from; i < to; ++i) v[i] = kCycle[i % 4];
}

// 另一族"平滑"：黄金角滑音（4 位小数）。它每块有 1440 个不同取值 → **每块都要重付字典费**，
// 正是 §8.3 警告的"字典主导列在小块下变大"。拿它当负向对照（分块不许赢）。
void fillGlide(std::vector<double>& v, size_t from, size_t to, double& angle) {
    for (size_t i = from; i < to; ++i) {
        angle += 0.25;
        if (angle >= 360.0) angle -= 360.0;
        v[i] = std::round(angle * 1e4) / 1e4;
    }
}

// 分段型：`blocks` 个 4096 块，其中约 `fraction` 的块是"不规则"（全精度均匀随机），
// 且**均匀铺开**（不是挤在开头）。这正是 §8.3 量过的那一族：全局只能选一个编码，怎么选都亏一半。
std::vector<double> segmentedColumn(int blocks, double fraction, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 360.0);
    std::vector<double> v((size_t)blocks * kBlockElems, 0.0);
    const int irregular = (int)std::lround(blocks * fraction);
    double angle = 137.508;
    for (int b = 0; b < blocks; ++b) {
        // 均匀铺开：把 irregular 个块摊到 blocks 个位置上（第 b 块该放 iff 比例跨过了整数）
        const bool take = (irregular > 0) &&
                          ((int64_t)b * irregular / blocks != (int64_t)(b + 1) * irregular / blocks);
        const size_t from = (size_t)b * kBlockElems;
        if (take) {
            for (uint32_t i = 0; i < kBlockElems; ++i)
                v[from + i] = std::round(uni(rng) * 1e6) / 1e6;
        } else {
            fillSmooth(v, from, from + kBlockElems);
        }
    }
    return v;
}

std::vector<double> smoothColumn(size_t n, uint32_t seed, double step) {
    std::vector<double> v(n);
    double angle = 137.508 + (double)(seed % 7);
    for (size_t i = 0; i < n; ++i) {
        angle += step;
        if (angle >= 360.0) angle -= 360.0;
        v[i] = std::round(angle * 1e4) / 1e4;
    }
    return v;
}

std::vector<double> noiseColumn(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uni(rng);
    return v;
}

// ---- 逐位往返 ----------------------------------------------------------------

bool roundTrips(const std::vector<double>& v, ColumnStorage mode, size_t* bytes = nullptr,
                ColumnLayoutStats* outStats = nullptr) {
    std::vector<uint8_t> enc;
    ColumnLayoutStats st;
    if (!encodeColumnV3(ColKind::Double, v.data(), v.size(), mode, enc, &st)) return false;
    if (bytes) *bytes = enc.size();
    if (outStats) *outStats = st;
    std::vector<double> back;
    // LegacyV2 走的是**裸列字节**（没有 v3 容器头），要用列解码器；另外两种才是容器。
    const bool ok = (mode == ColumnStorage::LegacyV2)
                        ? decodeDoubleColumn(enc.data(), enc.size(), back)
                        : decodeColumnV3(enc.data(), enc.size(), back);
    if (!ok) return false;
    if (back.size() != v.size()) return false;
    if (v.empty()) return true;
    return std::memcmp(back.data(), v.data(), v.size() * sizeof(double)) == 0;
}

// ---- 各种输入形状都往返一遍（含 4096 边界附近）-------------------------------
void roundTripShapes() {
    struct Case { const char* name; size_t n; };
    // 4096 前后、正好整块、多一块、多一个 —— 块边界是这类容器最容易错的地方
    const Case cases[] = {
        {"空列", 0}, {"1 个值", 1}, {"4095", 4095}, {"4096（整一块）", 4096},
        {"4097（多一个）", 4097}, {"8192（整两块）", 8192}, {"4096*3+7", 4096 * 3 + 7},
    };
    for (const Case& c : cases) {
        const std::vector<double> v = smoothColumn(c.n, 1234, 0.37);
        bool ok = true;
        for (ColumnStorage m : {ColumnStorage::LegacyV2, ColumnStorage::SingleZstd,
                                ColumnStorage::BestSelf}) {
            if (!roundTrips(v, m)) ok = false;
        }
        check(ok, fmt("逐位往返 %s（三种布局各一遍）", c.name));
    }
    // 特殊位模式：-0.0 / NaN / inf / 极值 —— 字典按位模式走，必须原样回来
    std::vector<double> weird = {0.0, -0.0, std::nan(""), std::nan("1"), INFINITY, -INFINITY,
                                 1.7976931348623157e308, 5e-324, 999.0, -999.0};
    for (size_t i = 0; i < 4096 * 2; ++i) weird.push_back((double)(i % 3));
    check(roundTrips(weird, ColumnStorage::BestSelf), "逐位往返 -0.0/NaN/±inf/极值");
}

// ---- 多帧往返（每帧只装 ~85 块，少量块根本压不到多帧路径）-------------------
void multiFrame() {
    setMustBlockBytesForTest(4096);   // 强制走分块：否则这几档都会选"单块"，多帧解码路径根本没被跑到
    for (int blocks : {4, 16, 64, 200, 700}) {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> uni(0.0, 360.0);
        std::vector<double> v((size_t)blocks * kBlockElems);
        for (size_t i = 0; i < v.size(); ++i) v[i] = std::round(uni(rng) * 1e6) / 1e6;
        ColumnLayoutStats st;
        bool ok = roundTrips(v, ColumnStorage::BestSelf, nullptr, &st);
        check(ok && st.blocked, fmt("多帧往返（强制分块）：%d 块 / %zu 帧", blocks, st.frames));
    }
    setMustBlockBytesForTest(kMustBlockBytes);
}

// ---- 分段型收益（验收：1.66~3.29× 那一档）------------------------------------
void segmentedGain() {
    bool allGood = true;
    double lo = 1e9, hi = 0.0;
    for (double f : {0.05, 0.20, 0.40}) {
        const std::vector<double> v = segmentedColumn(415, f, 20261015);
        ColumnLayoutStats st;
        if (!roundTrips(v, ColumnStorage::BestSelf, nullptr, &st)) {
            check(false, fmt("分段列 f=%.0f%% 编码失败", f * 100));
            allGood = false;
            continue;
        }
        if (st.blockedBytes == 0) {
            check(false, fmt("分段列 f=%.0f%% 没有尝试分块（单块 %zu B）", f * 100, st.singleBytes));
            allGood = false;
            continue;
        }
        // 两个口径都要看（§8.5 明确说过 §8.3 那组数字是"合成列、**无段级压缩**"）：
        //   编码级 = 块 blob 之和 vs 整列 blob     —— §8.3 的 1.66~3.29× 就是这个口径
        //   存储级 = 帧（各自 zstd）vs 整列（一帧 zstd）—— 生产路径真正落盘的字节
        const double codecGain = (double)st.singleCodecBytes / (double)st.blockedCodecBytes;
        const double storeGain = (double)st.singleBytes / (double)st.blockedBytes;
        lo = std::min(lo, codecGain);
        hi = std::max(hi, codecGain);
        std::printf("     分段型 f=%.0f%%：编码级 %zu → %zu B（%.2f×）｜存储级 %zu → %zu B（%.2f×）"
                    "｜%zu 块 / %zu 帧\n",
                    f * 100, st.singleCodecBytes, st.blockedCodecBytes, codecGain,
                    st.singleBytes, st.blockedBytes, storeGain, st.blocks, st.frames);
        if (codecGain < 1.5) {
            check(false, fmt("分段列 f=%.0f%% 编码级收益只有 %.2f×（§8.3 的档位是 1.66~3.29×）",
                             f * 100, codecGain));
            allGood = false;
        }
    }
    check(allGood, fmt("分段型编码级收益：实测 %.2f×~%.2f×（§8.3 的档位是 1.66~3.29×）", lo, hi));
}

// ---- 负向对照：分块不许让"本来就同质"的列变大 --------------------------------
void blockedNeverWorse() {
    struct Case { const char* name; std::vector<double> v; };
    std::vector<Case> cases;
    cases.push_back({"同质滑音 1.7M", smoothColumn((size_t)415 * kBlockElems, 7, 0.25)});
    cases.push_back({"白噪声 1.7M", noiseColumn((size_t)415 * kBlockElems, 99)});
    // 字典主导：只有 8 个取值（分块会让每块重复付字典费 → 正是 §8.3 警告的那一族）
    std::vector<double> dictDominated((size_t)415 * kBlockElems);
    for (size_t i = 0; i < dictDominated.size(); ++i) dictDominated[i] = (double)(i % 8) * 45.0;
    cases.push_back({"字典主导（8 个取值）", dictDominated});

    for (const Case& c : cases) {
        ColumnLayoutStats st;
        if (!roundTrips(c.v, ColumnStorage::BestSelf, nullptr, &st)) {
            check(false, fmt("负向对照 %s：编码/往返失败", c.name));
            continue;
        }
        check(!st.blocked, fmt("负向对照 %s：分块没赢（单块 %zu B vs 分块 %zu B → 选了单块）",
                               c.name, st.singleBytes, st.blockedBytes));
    }
}

// ---- 强制分块：裸数据过阈值时"分块是要求，不是优化"（③ 的窗口化地基）----------
void forcedBlocking() {
    // 把阈值调到极小，好在单测里覆盖这条规则（否则要造 1600 万值的列）
    setMustBlockBytesForTest(4096);
    {
        // 同质方块列：按体积分块本来必输（负向对照里验过），但过阈值就必须分块
        std::vector<double> v((size_t)8 * kBlockElems);
        fillSmooth(v, 0, v.size());
        std::vector<uint8_t> enc; ColumnLayoutStats st;
        const bool ok = encodeColumnV3(ColKind::Double, v.data(), v.size(),
                                       ColumnStorage::BestSelf, enc, &st) &&
                        st.forced && st.blocked && st.blockedBytes > st.singleBytes;
        std::vector<double> back;
        check(ok && decodeColumnV3(enc.data(), enc.size(), back) && back == v,
              fmt("强制分块：分块更大（%zu > %zu B）也照样选分块，且逐位往返",
                  st.blockedBytes, st.singleBytes));
    }
    {
        // 常量级列（单块 ≤64 B）**不**强制：切了也只有一份数据，纯亏头部
        std::vector<double> v((size_t)8 * kBlockElems, 90.0);
        std::vector<uint8_t> enc; ColumnLayoutStats st;
        const bool ok = encodeColumnV3(ColKind::Double, v.data(), v.size(),
                                       ColumnStorage::BestSelf, enc, &st) && !st.forced && !st.blocked;
        std::vector<double> back;
        check(ok && decodeColumnV3(enc.data(), enc.size(), back) && back == v,
              "强制分块：常量级列（单块 ≤64 B）不强制（切了没有可寻址的数据）");
    }
    setMustBlockBytesForTest(kMustBlockBytes);   // 还原成生产阈值
}

// ---- 按块随机访问（③ 要消费的地基）------------------------------------------
void blockRandomAccess() {
    const std::vector<double> v = segmentedColumn(9, 0.35, 4242);   // 9 块，跨多帧
    std::vector<uint8_t> enc;
    ColumnLayoutStats st;
    if (!encodeColumnV3(ColKind::Double, v.data(), v.size(), ColumnStorage::BestSelf, enc, &st) ||
        !st.blocked) {
        check(false, "按块访问：分段列应当选分块（否则这条用例没覆盖到）");
        return;
    }
    ColumnView view;
    if (!openColumnV3(enc.data(), enc.size(), view) || !view.blocked) {
        check(false, "按块访问：openColumnV3 失败");
        return;
    }
    check(view.total == v.size() && view.blockElems == kBlockElems && view.frameCount == st.frames,
          fmt("按块访问：容器头自洽（%llu 值 / %u 一块 / %llu 帧）",
              (unsigned long long)view.total, view.blockElems, (unsigned long long)view.frameCount));

    // 全量解码做参照
    std::vector<double> all;
    check(decodeColumnV3(enc.data(), enc.size(), all) && all.size() == v.size(),
          "按块访问：全量解码可作参照");

    bool ok = true;
    const uint64_t nBlocks = (v.size() + kBlockElems - 1) / kBlockElems;
    for (uint64_t k = 0; k < nBlocks; ++k) {
        std::vector<double> blk;
        if (!decodeColumnBlock(view, k, blk)) { ok = false; break; }
        const size_t from = (size_t)k * kBlockElems;
        const size_t want = std::min((size_t)kBlockElems, v.size() - from);
        if (blk.size() != want ||
            std::memcmp(blk.data(), v.data() + from, want * sizeof(double)) != 0 ||
            std::memcmp(blk.data(), all.data() + from, want * sizeof(double)) != 0) {
            ok = false;
            break;
        }
    }
    check(ok, fmt("按块访问：%llu 块逐块解码都与全量/原值相同（每块独立）", (unsigned long long)nBlocks));
    std::vector<double> oob;
    check(!decodeColumnBlock(view, nBlocks, oob), "按块访问：越界的块号必须干净失败");
}

// ---- 坏数据必须干净失败（负向对照）------------------------------------------
void badData() {
    const std::vector<double> v = segmentedColumn(4, 0.5, 1);
    std::vector<uint8_t> enc;
    if (!encodeColumnV3(ColKind::Double, v.data(), v.size(), ColumnStorage::BestSelf, enc)) {
        check(false, "坏数据用例：编码失败");
        return;
    }
    ColumnView view;
    if (!openColumnV3(enc.data(), enc.size(), view) || !view.blocked) {
        check(false, "坏数据用例：应当选分块");
        return;
    }
    std::vector<double> sink;
    check(!openColumnV3(enc.data(), enc.size() / 3, view), "坏数据：截断的容器必须被拒（段表越界）");
    check(!openColumnV3(enc.data(), 8, view), "坏数据：连容器头都不完整必须被拒");

    // 把一个帧的字节范围改到容器外
    {
        std::vector<uint8_t> bad = enc;
        const size_t tableOff = kContainerHeader + 16;
        uint8_t* e = bad.data() + tableOff;
        const uint64_t huge = 0x7fffffffffffffffull;
        std::memcpy(e + 24, &huge, 8);              // compSize
        ColumnView v2;
        check(!openColumnV3(bad.data(), bad.size(), v2), "坏数据：帧字节范围越界必须被拒");
    }
    // 块号大于帧表覆盖范围
    {
        ColumnView v2;
        if (openColumnV3(enc.data(), enc.size(), v2)) {
            uint64_t f = 0, b = 0;
            check(!v2.locateBlock(v2.total, f, b), "坏数据：块号越界时 locateBlock 返回 false");
        }
    }
    // kind 不符：拿 Int64 解释 double 列
    {
        std::vector<int64_t> wrong;
        check(!decodeColumnV3(enc.data(), enc.size(), wrong), "坏数据：kind 不符必须被拒");
    }
    // blockElems = 0
    {
        std::vector<uint8_t> bad = enc;
        uint32_t zero = 0;
        std::memcpy(bad.data() + kContainerHeader, &zero, 4);
        ColumnView v2;
        check(!openColumnV3(bad.data(), bad.size(), v2), "坏数据：blockElems=0 必须被拒");
    }
    // 未知 layout
    {
        std::vector<uint8_t> bad = enc;
        bad[1] = 7;
        ColumnView v2;
        check(!openColumnV3(bad.data(), bad.size(), v2), "坏数据：未知 layout 必须被拒");
    }
    (void)sink;
}

// ---- int64 / u32 列也要能分块（Actions 的列不是 double）---------------------
void otherColumnKinds() {
    std::vector<int64_t> floors((size_t)12 * kBlockElems);
    for (size_t i = 0; i < floors.size(); ++i) floors[i] = (int64_t)i * 3 + (i % 1000);
    std::vector<uint8_t> enc;
    ColumnLayoutStats st;
    bool ok = encodeColumnV3(ColKind::Int64, floors.data(), floors.size(),
                             ColumnStorage::BestSelf, enc, &st);
    std::vector<int64_t> back;
    ok = ok && decodeColumnV3(enc.data(), enc.size(), back) && back == floors;
    check(ok, fmt("int64 列往返（%s，%zu B）", st.blocked ? "分块" : "单块", enc.size()));

    std::vector<uint32_t> types((size_t)12 * kBlockElems);
    for (size_t i = 0; i < types.size(); ++i) types[i] = (uint32_t)(i % 4);
    ok = encodeColumnV3(ColKind::U32, types.data(), types.size(), ColumnStorage::BestSelf, enc, &st);
    std::vector<uint32_t> backU;
    ok = ok && decodeColumnV3(enc.data(), enc.size(), backU) && backU == types;
    check(ok, fmt("u32 列往返（%s，%zu B）", st.blocked ? "分块" : "单块", enc.size()));
}

}  // namespace

int main() {
    std::printf("— .adocao v3 分块列容器自测 —\n");
    roundTripShapes();
    multiFrame();
    segmentedGain();
    blockedNeverWorse();
    forcedBlocking();
    blockRandomAccess();
    badData();
    otherColumnKinds();
    if (g_fail) { std::printf("\n%d 项失败\n", g_fail); return 1; }
    std::printf("\n全部通过\n");
    return 0;
}
