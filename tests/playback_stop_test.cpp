// 退出播放必须**回到初始状态**（用户 2026-10-28 报的 bug）。
//
// 原来的 `PlaybackClock::stop()` 只把 `m_elapsedTime` 归零，`m_frame` 仍停在被停住的那一刻
// （行星位置 / 相机跟随 / 砖的可见性全部来自这一帧）→ 画面上看起来"没回到开头"。
// 本用例钉住：stop() 之后 frame 的时间、砖号、行星位置都要与"从未播放"一致。

#include "core/level/LevelData.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

static const char* kLevel = R"JSON({"settings": {"bpm": 120},
 "angleData": [0, 90, 0, 90, 0, 90, 0, 90],
 "actions": []})JSON";

int main() {
    LevelData lv;
    if (!lv.loadFromBuffer(kLevel, std::strlen(kLevel))) {
        std::printf("✗ 用例谱面没加载成功\n");
        return 1;
    }
    Timeline tl;
    tl.build(lv, false);

    PlaybackClock clock;
    clock.attachTimeline(&tl);

    // 起点 = **一个从未播放过的时钟 stop() 后的帧**（stop() 现在会重算帧，
    // 所以这就是"初始状态"的权威定义；不能用默认构造的帧比 —— 它的 tileIndex 是 0，
    // 而 t=0 的合法砖号由 findTileIndex 决定，可能是 1）。
    PlaybackClock ref;
    ref.attachTimeline(&tl);
    ref.stop();
    const PlaybackFrame before = ref.frame();

    // 播到中段再停
    clock.startAtLevelTime(1.5);
    clock.update(16.0f);
    const PlaybackFrame mid = clock.frame();
    if (std::fabs(mid.timeInLevel - before.timeInLevel) < 1e-9) {
        std::printf("✗ 用例无效：播放后时间没变（%.6f）\n", mid.timeInLevel);
        return 1;
    }
    clock.stop();

    const PlaybackFrame after = clock.frame();
    int bad = 0;
    if (std::fabs(after.timeInLevel - before.timeInLevel) > 1e-9) {
        std::printf("✗ stop 后 timeInLevel=%.6f，起点是 %.6f\n", after.timeInLevel, before.timeInLevel);
        bad++;
    }
    if (after.tileIndex != before.tileIndex) {
        std::printf("✗ stop 后当前砖=%d，起点是 %d\n", after.tileIndex, before.tileIndex);
        bad++;
    }
    // 位置按**逐位**比（stop 与 ref.stop 走同一条 updateFrame，结果必须完全一致；
    // 差一个 -0.0 就说明两条路径的算式不同）。
    if (std::memcmp(&after.redPosition, &before.redPosition, sizeof(after.redPosition)) != 0) {
        std::printf("✗ stop 后行星位置 (%f,%f)，起点是 (%f,%f)\n",
                    after.redPosition.x, after.redPosition.y,
                    before.redPosition.x, before.redPosition.y);
        bad++;
    }
    if (bad == 0) std::printf("playback_stop: OK（stop() 回到 t=0 的同一帧）\n");
    return bad == 0 ? 0 : 1;
}
