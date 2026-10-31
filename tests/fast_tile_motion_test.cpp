// 极快砖（duration < 100 µs）下的行星位置。
//
// 2026-10-31 抓到的真 bug：三处 progress 都写成
//     double progress = (duration > 0.0001) ? (t - startTime) / duration : 1.0;
// 那个 0.0001 是"防除零"，但它同时把**所有快于 100 µs 的砖**钉死在 progress = 1
// （= 砖的终点）。2.64e6 BPM 一砖只有 22.7 µs → 行星永远在砖心/终点上，
// 轨迹变成"砖心连砖心"的直线，拖尾跟着变成一条直线（用户截图："不应该贴着砖走"）。
//
// 这个测试：BPM 2.64e6 的谱，取一块砖的中点，行星必须**离开终点**、落在弧上。
#include "core/level/LevelData.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

static int g_fail = 0;
static void check(const char* what, bool ok) {
    if (ok) std::printf("ok   %s\n", what);
    else { std::printf("FAIL %s\n", what); ++g_fail; }
}

int main() {
    const char* json = R"({"angleData":[0,90,0,90,0,90,0,90,0,90,0,90],
        "settings":{"bpm":2640000}})";
    LevelData lv; Timeline tl;
    if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL load\n"); return 1; }
    tl.build(lv, false);

    const double dur = tl.durationAt(2);
    std::printf("     tile 2 duration = %.9f s (%.2f µs)\n", dur, dur * 1e6);
    check("前提：这块砖确实 < 100 µs（旧阈值会在这里触发）", dur > 0.0 && dur < 1e-4);

    const double tMid = tl.startTimeAt(2) + dur * 0.5;
    const double tEnd = tl.startTimeAt(2) + dur * 0.999999;
    glm::dvec2 r1{}, b1{}, r2{}, b2{};
    PositionSolver::positionAt(tl, tMid, r1, b1);
    PositionSolver::positionAt(tl, tEnd, r2, b2);

    const double d = std::hypot(b1.x - b2.x, b1.y - b2.y);
    std::printf("     中点 vs 终点：mover 相差 %.6f 砖（枢轴相差 %.2e）\n", d,
                std::hypot(r1.x - r2.x, r1.y - r2.y));
    check("中点必须离开终点（旧代码 progress 被钉成 1 → 差 0）", d > 0.05);
    check("中点与终点都在轨道上（离枢轴 ≈ 半径 1）",
          std::fabs(std::hypot(b1.x - r1.x, b1.y - r1.y) - 1.0) < 0.05);

    // 再取同一块砖的两个不同时刻，行星必须连续移动（不是跳变）。
    glm::dvec2 r3{}, b3{};
    PositionSolver::positionAt(tl, tl.startTimeAt(2) + dur * 0.25, r3, b3);
    const double d2 = std::hypot(b1.x - b3.x, b1.y - b3.y);
    check("0.25 与 0.5 处位置不同（progress 真的在走）", d2 > 0.02);

    if (g_fail) { std::printf("fast_tile_motion: %d 条失败\n", g_fail); return 1; }
    std::printf("fast_tile_motion: 全部通过\n");
    return 0;
}
