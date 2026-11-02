// 一块砖在 t 时刻的"活变换"：MoveTrack（解析式）+ AnimateTrack（逐砖关键帧，**后者覆盖前者**）。
//
// 为什么要有这个头：`render/LevelScene.cpp::trackXformAt()` 一直在算这件事（给实例流用），
// 而**行星与拖尾**也要能在**任意历史时刻**问同一件事 —— 拖尾必须解析式采样（不能记历史），
// 所以两边必须调同一个函数，否则"头跟了动画、尾巴没跟"就会拉出一条假直线。
//
// 纯函数：只依赖 (level, anim, tile, t)，没有帧状态。
#pragma once

#include "core/level/LevelData.hpp"
#include "core/timeline/TileAnimation.hpp"

#include <cstdint>

namespace adofai {

struct TileXform {
    double moveX = 0.0, moveY = 0.0;      // 位移（砖单位）
    double rot = 0.0;                     // 旋转增量（弧度）
    double scaleX = 1.0, scaleY = 1.0;
    double opacity = 1.0;
    bool fromAnimation = false;           // true = 走了 AnimateTrack 关键帧（否则是 MoveTrack）
};

inline TileXform tileXformAt(const LevelData& lv, const TileAnimation* anim, int64_t tile,
                             double t) {
    TileXform x;
    const LevelData::MoveTransform tr = lv.moveTransformAt(tile, t);
    x.moveX = (double)tr.moveX;
    x.moveY = (double)tr.moveY;
    x.rot = (double)tr.rotDelta;
    x.scaleX = (double)tr.scaleX;
    x.scaleY = (double)tr.scaleY;
    x.opacity = (double)tr.alpha;
    // PositionTrack 的 rotation/scale/opacity（权威 `Player.ts:6855`：作为 mesh 的**基础**变换）。
    // 这里当**修饰量**叠在 MoveTrack/AnimateTrack 之上：没有其它动画的砖得到的就是事件值本身，
    // 两者都有的砖按乘积/相加合成。没有 PositionTrack 富字段的谱只多一次布尔判断。
    if (lv.hasPosTrackXform()) {
        const LevelData::PosTrackXform pt = lv.posTrackXformAt(tile);
        if (pt.rotDeg != 0.0f) x.rot += (double)pt.rotDeg * 3.14159265358979323846 / 180.0;
        if (pt.scale != 1.0f) { x.scaleX *= (double)pt.scale; x.scaleY *= (double)pt.scale; }
        if (pt.opacity != 1.0f) x.opacity *= (double)pt.opacity;
    }
    if (anim) {
        TileAnimSample an;
        if (anim->sample(tile, t, an)) {
            x.moveX = (double)an.posX;
            x.moveY = (double)an.posY;
            x.rot = (double)an.rot;
            x.scaleX = (double)an.scaleX;
            x.scaleY = (double)an.scaleY;
            x.opacity = (double)an.opacity;
            x.fromAnimation = true;
        }
    }
    return x;
}

}  // namespace adofai
