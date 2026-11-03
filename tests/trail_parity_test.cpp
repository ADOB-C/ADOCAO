// 拖尾「快路径」与行星求值器的**逐位对拍**（2026-11-03）
//
// 背景：拖尾采样走 `trailPointAt`（缓存常量 + 每点刷新活值），行星走 `PositionSolver::positionAtTile`。
// 这是**两份实现**，已经因为分家出过两次事故：
//   ① 长按期间圆心在平移，快路径不知道 → 拖尾画成一条水平直线；
//   ② 补了 slide 但算在"进砖那一刻"的缓存里 → 到达瞬间被拉成跨屏横带。
// 这个用例把"两份必须逐位相同"钉住：以后 `positionAtTile` 多一个随时间变的输入而快路径没跟，
// ctest 会当场红，不用等人肉截图发现。
//
// 覆盖：普通砖 / MoveTrack 砖 / AnimateTrack 砖 / 长按砖 / 首砖与末砖特例。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_trail_parity_test）

#include "core/level/LevelData.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

static int failures = 0;
static int compared = 0;

// 一块砖上取 5 个时刻（含两端），逐位比 red/blue 两个坐标
static void compareTile(const Timeline& tl, const TileAnimation* anim, int idx, const char* why) {
    const double t0 = tl.startTimeAt(idx);
    const double t1 = tl.startTimeAt(idx + 1);
    for (int k = 0; k < 5; ++k) {
        const double t = t0 + (t1 - t0) * (double)k / 4.0;
        glm::dvec2 fr(0), fb(0), sr(0), sb(0);
        PositionSolver::positionAtTile(tl, t, idx, fr, fb, anim);          // 行星的评价器
        PositionSolver::trailPointForTest(tl, idx, t, anim, sr, sb);       // 拖尾的快路径
        ++compared;
        if (std::memcmp(&fr.x, &sr.x, sizeof(double)) != 0 ||
            std::memcmp(&fr.y, &sr.y, sizeof(double)) != 0 ||
            std::memcmp(&fb.x, &sb.x, sizeof(double)) != 0 ||
            std::memcmp(&fb.y, &sb.y, sizeof(double)) != 0) {
            std::printf("  ✗ 砖 %d（%.1f%%）%s：行星=(%.17g,%.17g|%.17g,%.17g) 拖尾=(%.17g,%.17g|%.17g,%.17g)\n",
                        idx, 100.0 * k / 4.0, why, fr.x, fr.y, fb.x, fb.y, sr.x, sr.y, sb.x, sb.y);
            ++failures;
        }
    }
}

int main() {
    // 一张小谱：普通砖 + MoveTrack + AnimateTrack + Hold（长按的滑动圆心是重点）
    static const char* kLevel = R"JSON({"settings": {"bpm": 120},
 "angleData": [0, 0, 30, 0, 0, 0, 45, 0, 0, 0],
 "actions": [
   {"floor": 2, "eventType": "MoveTrack", "duration": 1, "positionOffset": [2, 1]},
   {"floor": 4, "eventType": "AnimateTrack", "duration": 1, "trackAnimation": "PositionTrack",
    "positionOffset": [1, -1]},
   {"floor": 6, "eventType": "Hold", "duration": 3, "distanceMultiplier": 100}
 ]})JSON";

    LevelData lv;
    if (!lv.loadFromBuffer(kLevel, std::strlen(kLevel))) {
        std::printf("✗ 用例谱面没加载成功\n");
        return 1;
    }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);
    if (tl.tileCount() < 3) {
        std::printf("✗ timeline 只有 %lld 层\n", (long long)tl.tileCount());
        return 1;
    }

    TileAnimation anim;
    anim.build(lv, tl);

    const int n = (int)lv.tileCount();
    std::printf("# 砖数 %d，逐砖 5 个时刻对拍\n", n);
    for (int i = 0; i + 1 < n; i += 1) {
        const bool isHold = lv.holdLengthAt(i) >= 0;
        compareTile(tl, &anim, i, isHold ? "长按砖" : (i == 2 ? "MoveTrack" : (i == 4 ? "AnimateTrack" : "普通")));
    }
    // 无动画版本也要一致（anim = nullptr 时枢轴不做活值刷新）
    for (int i = 0; i + 1 < n; ++i) compareTile(tl, nullptr, i, "无动画");

    std::printf("# 对拍 %d 组\n", compared);
    if (failures) { std::printf("trail_parity: %d 处不一致\n", failures); return 1; }
    std::printf("trail_parity: 全部逐位相同（快路径 == positionAtTile）\n");
    return 0;
}
