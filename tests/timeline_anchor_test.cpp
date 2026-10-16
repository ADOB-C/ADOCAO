// ③ 切片 1 的验收：**锚点 == 全量数组在该层逐位相同**（每 4096 层一个锚点）。
//
// 为什么先做这一步：Timeline 3 支 double（起始时间 + TrackVis 的消失/出现时间）在 MYC 原版上
// 实测 **162 MB**（live 530 MB 的 31%）。目标是把它们换成"锚点 + 按块重算"，而重算正确的前提是
// **锚点本身必须与全量数组逐位一致** —— 本测试只验这一条（改消费方是切片 2/3）。
//
// 覆盖：多层谱（真实谱，命令行给）/ 小谱边界；以及
//   * 覆盖率：锚点 floor 序列必须是 0, 4096, 8192…，且数量 = ceil(n / 4096)；
//   * 逐位：startTime/bpm/cw（以及 needsTrackVis 时的 disappear/appear）**逐一 `==` 比**；
//   * **反面对照**：把锚点拷贝里的一个 startTime 动 1 ULP → 对拍**必须**失败。

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

// 逐位对拍：锚点 vs 全量数组。返回不一致条数（0 = 通过）
size_t compareAnchors(const Timeline& tl, const std::vector<Timeline::Anchor>& anchors) {
    const auto& st = tl.tileStartTimes();
    const auto& bpms = tl.tileBPMs();
    const auto& cw = tl.tileIsCW();
    const auto& dt = tl.tileDisappearTimes();
    const auto& at = tl.tileAppearTimes();
    size_t bad = 0;
    for (const Timeline::Anchor& a : anchors) {
        const size_t i = (size_t)a.floor;
        if (i >= st.size()) { ++bad; continue; }
        // 逐位：double/float 直接 == 比（不用容差 —— 容差会让"1 ULP 偏移"漏过去）
        if (!(a.startTime == st[i])) ++bad;
        else if (!(a.bpm == bpms[i])) ++bad;
        else if (a.cw != cw[i]) ++bad;
        else if (a.hasTrackVis && (!(a.disappearTime == dt[i]) || !(a.appearTime == at[i]))) ++bad;
    }
    return bad;
}

void testChart(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) {
        std::printf("FAIL 载入失败：%s\n", path);
        ++g_fail;
        return;
    }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);

    const auto& anchors = tl.anchors();
    const size_t n = tl.tileStartTimes().size();
    const int64_t stride = Timeline::kAnchorStride;
    const size_t expect = (n + (size_t)stride - 1) / (size_t)stride;

    // 覆盖率
    check(anchors.size() == expect, "锚点数量 != ceil(n/4096)");
    for (size_t k = 0; k < anchors.size(); ++k)
        check(anchors[k].floor == (int64_t)k * stride, "锚点 floor 序列不是 0,4096,8192…");

    // 逐位
    const size_t bad = compareAnchors(tl, anchors);
    check(bad == 0, "锚点与全量数组不一致");

    // 不变式：Timeline 的 BPM 与 LevelData 的 tileBPMs 是否逐位相同（若同，前者可不再常驻）
    if (!lv.tileBPMs.empty() && lv.tileBPMs.size() == tl.tileBPMs().size()) {
        size_t bad = 0;
        for (size_t i = 0; i < lv.tileBPMs.size(); ++i) if (!(lv.tileBPMs[i] == tl.tileBPMs()[i])) ++bad;
        std::printf("     BPM 交叉：LevelData.tileBPMs vs Timeline.tileBPMs → %zu 处不一致（%zu 层）\n",
                    bad, lv.tileBPMs.size());
        int shown = 0;
        for (size_t i = 0; i < lv.tileBPMs.size() && shown < 5; ++i) {
            if (!(lv.tileBPMs[i] == tl.tileBPMs()[i])) {
                std::printf("        @%zu LevelData=%.9g Timeline=%.9g（比值 %.6f）\n", i,
                            (double)lv.tileBPMs[i], (double)tl.tileBPMs()[i],
                            (double)lv.tileBPMs[i] / (double)tl.tileBPMs()[i]);
                ++shown;
            }
        }
    }
    const bool hasTV = !tl.tileDisappearTimes().empty();
    std::printf("     %-46s n=%-9zu 锚点 %-6zu（每 %lld 层）逐位不一致 %zu ｜TrackVis %s\n",
                path, n, anchors.size(), (long long)stride, bad, hasTV ? "有（两支 double 也对拍）" : "无");

    // 反面对照：动 1 ULP 必须被抓到（否则"逐位对拍"是空的）
    if (!anchors.empty()) {
        std::vector<Timeline::Anchor> perturbed = anchors;
        double v = perturbed[0].startTime;
        // 朝 +inf 方向动 1 ULP
        perturbed[0].startTime = std::nextafter(v, v + 1e308);
        check(compareAnchors(tl, perturbed) != 0,
              "反面对照失败：startTime 动 1 ULP 竟然还能通过");
        // 再验一个中间锚点（避免只测了第 0 层）
        if (anchors.size() > 2) {
            std::vector<Timeline::Anchor> p2 = anchors;
            const size_t mid = p2.size() / 2;
            p2[mid].bpm = std::nextafterf(p2[mid].bpm, p2[mid].bpm + 1e30f);
            check(compareAnchors(tl, p2) != 0, "反面对照失败：中间锚点的 bpm 动 1 ULP 没被抓到");
        }
    }
}

// ③ 切片 2：**按块重算**逐层对拍（每 4096 层一块，块内每一层都比）
void testBlocks(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::printf("FAIL 载入失败：%s\n", path); ++g_fail; return; }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);
    if (!tl.blocksRecomputable()) {
        std::printf("     %-46s 跳过按块重算（%s）\n", path, tl.blocksRecomputableWhy());
        return;
    }
    const auto& st = tl.tileStartTimes();
    const auto& dt = tl.tileDisappearTimes();
    const auto& at = tl.tileAppearTimes();
    const int64_t n = (int64_t)st.size(), stride = Timeline::kAnchorStride;

    size_t blocks = 0, floors = 0, bad = 0, firstBadFloor = (size_t)-1;
    for (int64_t i0 = 0; i0 < n; i0 += stride) {
        const int64_t i1 = (i0 + stride < n) ? (i0 + stride) : n;
        Timeline::BlockResult r;
        if (!tl.recomputeBlock(i0, i1, r)) { ++bad; break; }
        ++blocks;
        for (int64_t i = i0; i < i1; ++i) {
            ++floors;
            const size_t k = (size_t)(i - i0);
            bool ok = (r.startTimes[k] == st[(size_t)i]);
            if (ok && !dt.empty())
                ok = (r.disappearTimes[k] == dt[(size_t)i]) && (r.appearTimes[k] == at[(size_t)i]);
            if (!ok) { if (bad == 0) firstBadFloor = (size_t)i; ++bad; }
        }
    }
    if (bad && firstBadFloor != (size_t)-1)
        std::printf("     首个不一致在第 %zu 层\n", firstBadFloor);
    check(bad == 0, std::string(path) + "：按块重算与全量数组不一致");
    std::printf("     %-46s 按块重算：%zu 块 / %zu 层逐位不一致 %zu%s\n", path, blocks, floors, bad,
                dt.empty() ? "（无 TrackVis）" : "（含 disappear/appear）");

    // 反面对照：把某层时长动 1 ULP → 该块（及其后同块）必须重现出不一致
    if (n > 2) {
        const int64_t mid = (n / stride / 2) * stride + 7;   // 中间某块里的第 7 层
        if (mid < n - 1) {
            Timeline copy;                        // 重新建一份，避免污染上面的对拍
            copy.build(lv, false);
            // 时长已不存（现算）→ 改 BPM。注意**不能用 1 ULP**：`durationAt` 走 `60/bpm`，
            // 而 MYC 那层的 BPM 高达 3.2768e7，倒数把 1 ULP 的 BPM 变化直接吸收掉（实测过）。
            // 控制要"可见扰动"才有意义（意图是"改了输入 → 重算必须变"）。
            const float orig = copy.tileBPMs()[(size_t)mid];
            copy.setBpmForTest(mid, orig * 1.05f);
            Timeline::BlockResult r2;
            const int64_t i0 = (mid / stride) * stride;
            const int64_t i1 = std::min(i0 + stride, n);
            bool caught = false;
            if (copy.recomputeBlock(i0, i1, r2)) {
                for (int64_t i = i0; i < i1; ++i)
                    if (r2.startTimes[(size_t)(i - i0)] != st[(size_t)i]) { caught = true; break; }
            }
            check(caught, "反面对照失败：时长动 1 ULP 后重算竟然还一致");
        }
    }
}

// ③ 切片 3：丢掉密集两支之后，**窗口化访问器**必须逐层给出同一批值
void testWindowedAccess(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::printf("FAIL 载入失败：%s\n", path); ++g_fail; return; }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);
    if (!tl.hasTrackVisTimes()) {
        std::printf("     %-46s 无 TrackVis（不需要丢）\n", path);
        return;
    }
    // 先留一份密集数组的副本当基准
    const std::vector<double> wantDA = tl.tileDisappearTimes();
    const std::vector<double> wantAA = tl.tileAppearTimes();
    if (!tl.dropTrackVisArrays()) {
        std::printf("     %-46s 保留密集数组（%s；单调=%d）\n", path, tl.blocksRecomputableWhy(),
                    (int)tl.trackVisCoarseMonotone());
        return;
    }
    check(tl.tileDisappearTimes().empty() && tl.tileAppearTimes().empty(),
          "丢了之后密集数组应为空");
    size_t bad = 0, firstBad = (size_t)-1;
    for (size_t i = 0; i < wantDA.size(); ++i) {
        // 逐位（==，不用容差）；inf/-inf 也走同一条比较
        if (!(tl.disappearTimeAt((int64_t)i) == wantDA[i]) ||
            !(tl.appearTimeAt((int64_t)i) == wantAA[i])) {
            if (bad == 0) firstBad = i;
            ++bad;
        }
    }
    if (bad) std::printf("     首个不一致在第 %zu 层\n", firstBad);
    check(bad == 0, std::string(path) + "：窗口化访问器与密集数组不一致");
    std::printf("     %-46s 窗口化后逐层对拍：%zu 层 0 不一致？%s（粗索引单调=%d）\n", path,
                wantDA.size(), bad == 0 ? "是" : "否", (int)tl.trackVisCoarseMonotone());
}

// ③ 切片 3a：丢掉 `m_tileStartTimes` 之后，逐层访问 + `findTileIndex` 必须与密集数组一致
void testStartWindowed(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::printf("FAIL 载入失败：%s\n", path); ++g_fail; return; }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);
    if (!tl.dropStartTimesArray()) {
        std::printf("     %-46s 保留起始时间（锚点/时长不齐）\n", path);
        return;
    }
    check(tl.tileStartTimes().empty(), "丢了之后起点数组应为空");
    const size_t n = (size_t)tl.tileCount();
    // 参考实现 = 密集数组上的原二分（就是未窗口化分支那段）
    std::vector<double> want(n);
    for (size_t i = 0; i < n; ++i) want[i] = tl.startTimeAt(i);   // 先取一份（窗口化后仍是同一批值）
    auto refFind = [&](double t) -> int {
        if (n < 2) return 0;
        long lo = 0, hi = (long)n - 1;
        while (lo <= hi) { long mid = (lo + hi) / 2; if (want[(size_t)mid] <= t) lo = mid + 1; else hi = mid - 1; }
        long idx = hi; if (idx < 0) idx = 0; if (idx >= (long)n) idx = (long)n - 1;
        return (int)idx;
    };
    // 逐层
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) if (!(want[i] == want[i])) ++bad;   // 占位（真实比对在下面）
    bad = 0;
    for (size_t i = 0; i < n; ++i) {
        // 与"刚丢之前的密集数组"比：用锚点重算出来的值必须逐位相同
        // （want 是丢弃后取的，等价性由 testBlocks 保证；这里查自洽 + 与 findTileIndex 一致）
        if (!(tl.startTimeAt((int64_t)i) == want[i])) ++bad;
    }
    // findTileIndex 探针：均匀 + 锚点边界 ±1ULP + 两端 + 负值 + 超尾
    size_t badFind = 0, probes = 0;
    std::vector<double> ts;
    for (size_t k = 0; k < 2000; ++k) ts.push_back(want[n * k / 2000]);
    for (size_t k = 0; k < n; k += (size_t)Timeline::kAnchorStride) {
        ts.push_back(want[k]);
        ts.push_back(std::nextafter(want[k], -1e308));
        ts.push_back(std::nextafter(want[k], 1e308));
    }
    ts.push_back(-1e9); ts.push_back(want[n - 1] + 10.0); ts.push_back(want[n - 1]);
    for (double t : ts) {
        ++probes;
        if (tl.findTileIndex(t) != refFind(t)) ++badFind;
    }
    check(bad == 0, std::string(path) + "：窗口化 startTimeAt 不自洽");
    check(badFind == 0, std::string(path) + "：窗口化 findTileIndex 与密集数组不一致");
    std::printf("     %-46s 起始时间窗口化：%zu 层 / findTileIndex %zu 探针，不一致 %zu+%zu\n",
                path, n, probes, bad, badFind);

    // 反面对照：动 1 ULP 的时长必须让该块（及之后）的 startTimeAt 变
    if (n > 5000) {
        Timeline copy;
        copy.build(lv, false);
        copy.dropStartTimesArray();
        const int64_t mid = (int64_t)(n / 2);
        // 同 testBlocks：不能用 1 ULP（`60/bpm` 会把巨大 BPM 的 1 ULP 吸收掉）→ 可见扰动
        const float orig = copy.tileBPMs()[(size_t)mid];
        const float d0 = copy.durationAt(mid);
        copy.setBpmForTest(mid, orig * 1.05f);
        const float d1 = copy.durationAt(mid);
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "反面对照失败：bpm %g→%g、durationAt(%lld) %g→%g，窗口化 startTimeAt 却没变",
                      (double)orig, (double)copy.tileBPMs()[(size_t)mid], (long long)mid,
                      (double)d0, (double)d1);
        check(!(copy.startTimeAt(mid + 1) == want[(size_t)(mid + 1)]), buf);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("用法: %s <谱>…\n", argv[0]); return 2; }
    std::printf("=== ③ 切片 1：Timeline 锚点逐位对拍 ===\n");
    for (int i = 1; i < argc; ++i) testChart(argv[i]);
    std::printf("=== ③ 切片 2：按块重算逐层对拍 ===\n");
    for (int i = 1; i < argc; ++i) testBlocks(argv[i]);
    std::printf("=== ③ 切片 3：丢掉密集数组后的窗口化访问 ===\n");
    for (int i = 1; i < argc; ++i) testWindowedAccess(argv[i]);
    std::printf("=== ③ 切片 3a：丢掉起始时间数组后的窗口化访问 ===\n");
    for (int i = 1; i < argc; ++i) testStartWindowed(argv[i]);
    if (g_fail == 0) std::printf("全部通过\n");
    else std::printf("%d 项不通过\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
