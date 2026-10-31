// 增量可见性扫描与"逐块全扫"必须**逐块等价**。
//
// 背景（2026-10-31）：`LevelScene::updateTileVisibility()` 原来每帧对全部动画砖调一次
// `hiddenAt()`。2.64e6 那张谱有 997,664 块动画砖 → 实测 **24.7 ms/帧**（24.8 ns/砖）。
// 现在改成 `TileAnimation::updateVisibility()`：区间外的砖返回的是建帧时缓存的
// `hiddenBefore/hiddenAfter`（与时间无关），所以只扫"活跃区间与 (lastT, t] 相交"的砖；
// 全扫只发生在第一次和时间跳变 > 1 s 时。
//
// 这个测试把一个时间序列分别喂给两条路，逐块比：
//   A) 参考：每步都对**全部**砖算 hiddenAt
//   B) 增量：`updateVisibility()`（内部只扫窗口）
// 任何一步不一致就 FAIL。序列里包含：正常前进、原地不动、反向（倒放）、超过 1 s 的跳变。
#include "core/level/LevelData.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

static int g_fail = 0;

int main() {
    // 2000 块砖 + 全局 Fade（beatsAhead 4 / beatsBehind 0）→ 每块砖都有自己的 appear 窗口。
    std::string angles;
    for (int i = 0; i < 2000; ++i) {
        if (i) angles += ',';
        angles += (i % 3 == 0) ? "0" : "90";
    }
    std::string json = R"({"angleData":[)" + angles +
        R"(],"settings":{"bpm":120,"trackAnimation":"Fade","trackDisappearAnimation":"Fade",)"
        R"("beatsAhead":4,"beatsBehind":0}})";
    LevelData lv; Timeline tl;
    if (!lv.loadFromBuffer(json.data(), json.size())) { std::printf("FAIL load\n"); return 1; }
    tl.build(lv, false);
    TileAnimation anim;
    anim.build(lv, tl);

    const auto& es = anim.entries();
    std::printf("     %zu 块动画砖 / %zu 条关键帧\n", es.size(), anim.keyframeCount());
    if (es.empty()) { std::printf("FAIL 没有动画砖\n"); return 1; }

    // 时间序列：正常前进 → 原地 → 倒放 → 大跳
    std::vector<double> times;
    const double t0 = tl.startTimeAt(0);
    for (int i = 0; i < 120; ++i) times.push_back(t0 + i * 0.03);          // 前进
    for (int i = 0; i < 10; ++i) times.push_back(t0 + 119 * 0.03);         // 原地不动
    for (int i = 0; i < 40; ++i) times.push_back(t0 + (119 - i * 2) * 0.03); // 倒放
    times.push_back(t0 + 40.0);                                            // > 1 s 的跳变
    for (int i = 0; i < 40; ++i) times.push_back(t0 + 40.0 + i * 0.02);

    std::vector<uint8_t> refState(es.size(), 2), incState;
    double lastT = -1e300;
    size_t steps = 0, flips = 0, mismatches = 0;
    for (double t : times) {
        // A) 参考：全扫
        for (size_t k = 0; k < es.size(); ++k)
            refState[k] = anim.hiddenAt(es[k].tile, t) ? 1 : 0;
        // B) 增量
        anim.updateVisibility(t, incState, lastT, [&](int64_t, bool) { ++flips; });
        ++steps;
        for (size_t k = 0; k < es.size(); ++k) {
            if (incState[k] != refState[k]) {
                if (mismatches < 5)
                    std::printf("FAIL t=%.4f tile=%lld: 增量=%d 全扫=%d\n", t, (long long)es[k].tile,
                                (int)incState[k], (int)refState[k]);
                ++mismatches;
            }
        }
    }
    std::printf("     %zu 步 / %zu 次翻转 / %zu 处不一致\n", steps, flips, mismatches);
    if (mismatches) ++g_fail;

    if (g_fail) { std::printf("tileanim_active: 失败\n"); return 1; }
    std::printf("tileanim_active: 全部通过（增量扫描与全扫逐块等价）\n");
    return 0;
}
