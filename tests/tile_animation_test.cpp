// 逐砖关键帧（TileAnimation）的 MoveTrack 建帧/采样对拍：
// 与旧的 `LevelData::moveTransformAt` 在**位移/缩放/透明度**上必须逐值一致
// （旋转按新语义：绝对角 − 砖方向）。
#include "core/level/LevelData.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;
static int g_fail = 0;
static void near(const char* what, double got, double want, double eps = 2e-3) {
    if (std::fabs(got - want) > eps) {
        std::printf("FAIL %s: got %.5f want %.5f\n", what, got, want); ++g_fail;
    } else std::printf("ok   %s = %.5f\n", what, got);
}

int main() {
    const char* json = R"({"angleData":[0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90],
        "settings":{"bpm":120},
        "actions":[
          {"floor":4,"eventType":"MoveTrack","startTile":[-2,"ThisTile"],"endTile":[3,"ThisTile"],
           "positionOffset":[1,0],"rotationOffset":90,"scale":[200,150],"opacity":0,
           "duration":2,"ease":"Linear"}]})";
    LevelData lv; Timeline tl;
    if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL load\n"); return 1; }
    tl.build(lv, false);
    lv.buildMoveTrackIndex(tl);

    TileAnimation anim;
    anim.build(lv, tl);
    std::printf("有动画的砖: %zu，关键帧 %zu\n", anim.animatedTiles(), anim.keyframeCount());

    // 关键帧就在我们的 chart 时间线上（tile1 = 0）—— 不额外加偏移；
    // Re_ADOJAS 的"未归一化 tile1"由 getTimeOrigin 抵掉（见 tests/time_origin_test.cpp）。
    const double shift = 0.0;
    const double trig = tl.startTimeAt(4);
    const double dur = 2.0 * tl.durationAt(4);
    for (int tile : {2, 3, 4}) {
        for (double f : {0.25, 0.5, 1.0}) {
            const double t = trig + dur * f;
            const auto oldv = lv.moveTransformAt(tile, t);
            TileAnimSample s{};
            const bool has = anim.sample(tile, t + shift, s);   // 旧路径是 chart time，新路径是 chart+shift
            char buf[96];
            std::snprintf(buf, sizeof buf, "tile %d f=%.2f moveX", tile, f);
            near(buf, has ? s.posX : 0.0f, oldv.moveX);
            std::snprintf(buf, sizeof buf, "tile %d f=%.2f moveY", tile, f);
            near(buf, has ? s.posY : 0.0f, oldv.moveY);
            std::snprintf(buf, sizeof buf, "tile %d f=%.2f scaleX", tile, f);
            near(buf, has ? s.scaleX : 1.0f, oldv.scaleX);
            std::snprintf(buf, sizeof buf, "tile %d f=%.2f opacity", tile, f);
            near(buf, has ? s.opacity : 1.0f, oldv.alpha);
        }
    }
    // 范围外：没有动画
    TileAnimSample s{};
    if (anim.sample(20, 1.0, s)) { std::printf("FAIL 范围外 tile 20 不该有动画\n"); ++g_fail; }
    else std::printf("ok   范围外 tile 20 无动画\n");

    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("tile_animation: 全部通过\n");
    return 0;
}
