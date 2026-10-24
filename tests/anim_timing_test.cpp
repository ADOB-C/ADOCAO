// 回归测试：**事件时间 = 拍**（60/bpm），不是"这一砖自己的时长"。
//
// 2026-10-24 真踩过：MoveTrack / RecolorTrack / TweenColor 都拿 `timeline.durationAt(floor)`
// 当"一拍"，于是转角 0° 的直线砖 durationAt = 0 → 动画时长 = 0 → 砖瞬间飞走（MM2 开头一半砖是 0°）。
// 这里用一张**混合角度**的合成谱把三类事件都钉死：120 BPM（一拍 = 0.5 s）、
// 2 拍的事件必须正好 1.0 s，angleOffset 90° 的触发必须正好晚 0.25 s。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}
static void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol;
    if (!ok) std::printf("FAIL %s（got %.4f want %.4f）\n", what.c_str(), got, want);
    else std::printf("ok   %s\n", what.c_str());
}

int main() {
    // 0° / 90° / 180° 混排：0° 砖的 durationAt 为 0（正是会暴露 bug 的那种）
    const std::string json = R"({
      "angleData": [0, 0, 0, 90, 90, 180, 180, 0, 0, 90],
      "settings": {"bpm": 120, "offset": 0, "countdownTicks": 0, "pitch": 100,
                   "trackColor": "debb7b", "secondaryTrackColor": "ffffff",
                   "backgroundColor": "000000", "trackStyle": "Standard",
                   "trackAnimation": "None", "beatsAhead": 0,
                   "trackDisappearAnimation": "None", "beatsBehind": 0},
      "actions": [
        {"floor": 1, "eventType": "MoveTrack", "startTile": [2, "ThisTile"],
         "endTile": [2, "ThisTile"], "gapLength": 0, "duration": 2,
         "positionOffset": [4, 0], "ease": "Linear"},
        {"floor": 3, "eventType": "RecolorTrack", "startTile": [1, "ThisTile"],
         "endTile": [4, "ThisTile"], "duration": 2, "trackColor": "ff0000",
         "ease": "Linear", "angleOffset": 90}
      ]
    })";

    const std::string path = "anim_timing_test.adofai";
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }

    LevelData lv;
    if (!lv.loadFromFile(path, nullptr)) { std::printf("FAIL 加载合成谱\n"); return 1; }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);
    // 生产路径里由 LevelLoader 调这三个；测试里必须自己调，否则索引是空的
    lv.buildRecolorIndex(tl);
    lv.buildFlashIndex(tl);
    lv.buildMoveTrackIndex(tl);

    const double secPerBeat = 60.0 / 120.0;   // 0.5 s
    const double t1 = tl.startTimeAt(1);
    // 目标砖 = tile 1 + 2 = 3（90° 砖），事件从它自己的时间开始
    const double trig = t1 + 2.0 * tl.durationAt(1);   // 只用来定位"事件大约何时开始"

    checkNear((double)tl.durationAt(3), 0.5 * 90.0 / 180.0, 1e-6, "前置：tile 3 是 90° 砖（时长 0.25 s）");

    // ── MoveTrack：duration = 2 拍 = 1.0 s；两拍后位移正好 4 ──
    {
        check(!lv.moveEvents().empty(), "MoveTrack 事件已展开");
        const auto& ev = lv.moveEvents().front();
        checkNear(ev.durSec, 1.0, 1e-6, "MoveTrack：2 拍 @120BPM = 1.0 s（不是 durationAt(floor)）");
        check(ev.lo == 3 && ev.hi == 3, "MoveTrack：范围落到 tile 3");
        checkNear(ev.trigger, t1, 1e-6, "MoveTrack：trigger = floor 1 的起始时间");

        const auto at0 = lv.moveTransformAt(3, ev.trigger + 0.01);
        checkNear(at0.moveX, 0.04, 0.05, "MoveTrack：刚开始（0.01 s）位移仍接近 0");

        const auto mid = lv.moveTransformAt(3, ev.trigger + 0.5);
        checkNear(mid.moveX, 2.0, 0.05, "MoveTrack：半程（0.5 s）位移 ≈ 2（= 4 x 50%）");

        const auto end = lv.moveTransformAt(3, ev.trigger + 1.0 + 1e-3);
        checkNear(end.moveX, 4.0, 0.02, "MoveTrack：2 拍（1.0 s）后位移 = 4");
    }

    // ── RecolorTrack：angleOffset 90° = 半拍 = 0.25 s 的提前量 ──
    {
        const double t3 = tl.startTimeAt(3);
        const auto before = lv.tileColorsAt(4, t3 + 0.10, 1.0f, &tl);
        const auto during = lv.tileColorsAt(4, t3 + 0.35, 1.0f, &tl);
        const auto late = lv.tileColorsAt(4, t3 + 1.50, 1.0f, &tl);
        check(late.fill[0] == 0xFF && late.fill[1] == 0x00, "RecolorTrack：2 拍后已到目标色 ff0000");
        // 触发前是底色 debb7b，触发后开始向 ff0000 渐变
        check(before.fill[0] == 0xDE && before.fill[1] == 0xBB, "RecolorTrack：触发前仍是底色");
        check(during.fill[0] > before.fill[0] || during.fill[1] < before.fill[1],
              "RecolorTrack：angleOffset 90°（0.25 s）之后已开始变色");
    }

    // ── 反面对照：把"一拍"错当成 durationAt，快慢会差成 1/2 或 2 倍 ──
    {
        // 90° 砖：durationAt = 0.25 s = 0.5 拍 → 错误算法给 2*0.25 = 0.5 s（快一倍）
        const double quarter = (double)tl.durationAt(3);
        checkNear(quarter, 0.5 * secPerBeat, 1e-6, "反面对照：90° 砖 durationAt = 0.5 拍");
        checkNear(2.0 * quarter, 0.5, 1e-6, "反面对照：错误算法 -> 0.5 s（比正确的 1.0 s 快一倍）");

        // U-turn 砖（转 180°）：durationAt = 2 拍 → 错误算法给 2 s（慢一倍）
        const double uturn = (double)tl.durationAt(7);
        checkNear(uturn, 2.0 * secPerBeat, 1e-6, "反面对照：U-turn 砖 durationAt = 2 拍");
        checkNear(2.0 * uturn, 2.0, 1e-6, "反面对照：错误算法 -> 2.0 s（比正确的 1.0 s 慢一倍）");
    }

    (void)trig;
    std::printf("anim_timing: %s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}
