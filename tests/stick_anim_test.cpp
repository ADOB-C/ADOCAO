// 粘性轨道 + 轨道动画：**行星与拖尾要跟着被 MoveTrack 搬走的砖走**。
//
// 用户 2026-10-30 追问："移动轨道不也是按时间轴吗？为什么不能联动？" —— 能：
// `moveTransformAt(tile,t)` / `TileAnimation::sample(tile,t)` 都是纯函数，参考实现也是用
// `getTilePositionAtTime` 按时间重算（见 docs/level-loading.md「跟随 MoveTrack 动画」）。
//
// 四条断言：
//  ① 有动画时**枢轴星**落在"砖位 + 位移"上（MoveTrack 的 positionOffset 是砖单位）；
//  ② 移动星在砖起点（progress=0）恰好落在**上一砖的活位置**上（活邻居口径，不是静态）；
//  ③ `stickToFloors: false` 时行星**不跟**（位移不加，回到原始砖位）；
//  ④ 拖尾解析式采样与"直接算行星位置"逐点一致（这条把"头跟了、尾巴没跟"的假直线钉死）。

#include "core/level/LevelData.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

// 8 块砖、120 BPM、砖 2 被 MoveTrack **瞬时**搬走 (3, 0) 砖。
// 注意 `startTile`/`endTile` 的 offset 是**相对事件 floor** 的（`[0,"ThisTile"]` = 事件那一砖本身），
// 写成 `[2,"ThisTile"]` 会解析成 floor+2（踩过一次）。
std::string chartJson(bool stickToFloors, bool movePrevToo = false) {
    std::string s = R"({"angleData":[180,180,180,180,180,180,180,180],
      "settings":{"bpm":120,"offset":0,"countdownTicks":0,"pitch":100,"rotation":0,
                  "trackColor":"debb7b","backgroundColor":"000000","trackStyle":"Standard",
                  "trackAnimation":"None","trackDisappearAnimation":"None")";
    s += stickToFloors ? R"(,"stickToFloors":true)" : R"(,"stickToFloors":false)";
    s += R"(},
      "actions":[
        {"floor":2,"eventType":"MoveTrack","startTile":[0,"ThisTile"],"endTile":[0,"ThisTile"],
         "gapLength":0,"duration":0,"positionOffset":[3,0],"ease":"Linear"})";
    if (movePrevToo)
        s += R"(,
        {"floor":1,"eventType":"MoveTrack","startTile":[0,"ThisTile"],"endTile":[0,"ThisTile"],
         "gapLength":0,"duration":0,"positionOffset":[0,5],"ease":"Linear"})";
    s += R"(
      ]})";
    return s;
}

bool load(const std::string& json, LevelData& lv, Timeline& tl, TileAnimation& anim) {
    if (!lv.loadFromBuffer(json.data(), json.size())) return false;
    tl.build(lv, false);
    lv.buildMoveTrackIndex(tl);       // 生产路径里由 LevelLoader 调
    anim.build(lv, tl);
    return true;
}

struct Pose {
    glm::dvec2 red{}, blue{};
};

Pose planets(const Timeline& tl, double t, const TileAnimation* anim) {
    std::vector<PositionSolver::PlanetPos> ps;
    PositionSolver::planetsAt(tl, t, ps, anim);
    Pose p;
    for (auto& q : ps) {
        if (q.id == 0) p.red = q.pos;
        if (q.id == 1) p.blue = q.pos;
    }
    return p;
}

double dist(glm::dvec2 a, glm::dvec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

int main() {
    // ── ① 枢轴星落在"砖位 + 位移"上 ──────────────────────────────
    {
        LevelData lv;
        Timeline tl;
        TileAnimation anim;
        if (!load(chartJson(true), lv, tl, anim)) {
            std::printf("FAIL 合成谱加载失败（sticky=true）\n");
            return 1;
        }
        const int64_t tile = 2;                       // 偶数砖 → 红是枢轴
        const double t = tl.startTimeAt(tile) + (double)tl.durationAt(tile) * 0.5;
        const auto base = lv.tilePositionAt(tile);
        const Pose with = planets(tl, t, &anim);
        const Pose without = planets(tl, t, nullptr);
        const glm::dvec2 want(base[0] + 3.0, base[1]);
        if (dist(with.red, want) > 1e-9) {
            std::printf("FAIL 枢轴星没跟上位移：得到 (%.6f,%.6f)，应为 (%.6f,%.6f)\n", with.red.x,
                        with.red.y, want.x, want.y);
            return 1;
        }
        if (dist(without.red, glm::dvec2(base[0], base[1])) > 1e-9) {
            check(false, "没有动画时枢轴星不在原始砖位上（静态路径被动了）");
            return 1;
        }
        check(dist(with.red, without.red) > 1.0, "带/不带动画的枢轴星一样（位移没生效）");
        std::printf("ok   ① 枢轴星 = 砖位 + (3,0)：带动画 (%.3f,%.3f)、不带 (%.3f,%.3f)\n",
                    with.red.x, with.red.y, without.red.x, without.red.y);

        // ── ② 砖起点处移动星 = 上一砖静态位置 + **本砖位移**（刚体平移） ──
        // 半径不跟着位移伸缩：这里同时校验 |移动星 − 枢轴| == 静态 startDist。
        const double t0 = tl.startTimeAt(tile);
        const Pose at0 = planets(tl, t0, &anim);
        const auto prev = lv.tilePositionAt(tile - 1);
        const glm::dvec2 wantMover(prev[0] + 3.0, prev[1]);
        if (dist(at0.blue, wantMover) > 1e-9) {
            std::printf("FAIL 砖起点处移动星在 (%.6f,%.6f)，应为 (%.6f,%.6f)（上一砖 + 本砖位移）\n",
                        at0.blue.x, at0.blue.y, wantMover.x, wantMover.y);
            return 1;
        }
        const double rStatic = (double)tl.tileStartDist(tile);
        const double rGot = dist(at0.blue, at0.red);
        if (std::fabs(rGot - rStatic) > 1e-9) {
            std::printf("FAIL 半径被位移带歪了：%.6f vs 静态 %.6f\n", rGot, rStatic);
            return 1;
        }
        std::printf("ok   ② 砖起点处移动星 = 上一砖 + (3,0)，半径 %.3f 不变\n", rGot);

        // ── ④ 拖尾解析式采样 vs 直接算行星位置（逐点一致） ──
        const double trailT = t;
        const double trailStart = trailT - 0.30;
        std::vector<glm::dvec2> red, blue;
        PositionSolver::sampleTrailRange(tl, trailStart, trailT, 400.0f, with.red, with.blue, red,
                                         blue, &anim);
        if (red.size() < 10) {
            std::printf("FAIL 拖尾采样点太少（%zu）\n", red.size());
            return 1;
        }
        double worst = 0.0;
        for (size_t i = 0; i + 1 < red.size(); i++) {   // 最后一点是绑定的头，跳过
            const double tt = trailStart + (double)i / 400.0;
            const Pose p = planets(tl, tt, &anim);
            worst = std::max(worst, std::max(dist(red[i], p.red), dist(blue[i], p.blue)));
        }
        if (worst > 1e-9) {
            std::printf("FAIL 拖尾与行星位置不一致（最大 %.3e 砖）—— 头尾分叉会画成假直线\n", worst);
            return 1;
        }
        std::printf("ok   ④ 拖尾 %zu 点与行星位置逐点一致（最大偏差 %.1e 砖）\n", red.size(), worst);
    }

    // ── ③ stickToFloors: false → 行星不跟 ───────────────────────
    {
        LevelData lv;
        Timeline tl;
        TileAnimation anim;
        if (!load(chartJson(false), lv, tl, anim)) {
            std::printf("FAIL 合成谱加载失败（sticky=false）\n");
            return 1;
        }
        const int64_t tile = 2;
        const double t = tl.startTimeAt(tile) + (double)tl.durationAt(tile) * 0.5;
        const auto base = lv.tilePositionAt(tile);
        const Pose with = planets(tl, t, &anim);
        if (dist(with.red, glm::dvec2(base[0], base[1])) > 1e-9) {
            std::printf("FAIL stickToFloors=false 时枢轴星还是跟着动画走了： (%.6f,%.6f)\n",
                        with.red.x, with.red.y);
            return 1;
        }
        std::printf("ok   ③ stickToFloors=false：枢轴星仍在原始砖位 (%.3f,%.3f)\n", with.red.x,
                    with.red.y);
    }

    // ── ⑤ 半径只由 ScaleRadius 决定：**邻居被搬走也不影响** ──────
    // 这条区分两个模型：按"活邻居"重算半径的实现（旧版/参考实现的实况路径）在邻居
    // 位移 (0,5) 时半径会变；"只有 ScaleRadius 改半径"的实现不会。
    {
        LevelData a, b;
        Timeline ta, tb;
        TileAnimation anA, anB;
        if (!load(chartJson(true, false), a, ta, anA) || !load(chartJson(true, true), b, tb, anB)) {
            std::printf("FAIL ⑤ 合成谱加载失败\n");
            return 1;
        }
        const int64_t tile = 2;
        const double t = tb.startTimeAt(tile);
        const Pose pa = planets(ta, t, &anA);
        const Pose pb = planets(tb, t, &anB);
        if (dist(pa.blue, pb.blue) > 1e-9) {
            std::printf("FAIL ⑤ 上一砖的位移影响了半径/角度：%.6f vs %.6f\n", pa.blue.x, pb.blue.x);
            return 1;
        }
        const double rStatic = (double)tb.tileStartDist(tile);
        if (std::fabs(dist(pb.blue, pb.red) - rStatic) > 1e-9) {
            std::printf("FAIL ⑤ 半径被邻居位移带歪：%.6f vs 静态 %.6f\n", dist(pb.blue, pb.red),
                        rStatic);
            return 1;
        }
        std::printf("ok   ⑤ 邻居被搬 (0,5) 时行星位置/半径与不搬时完全相同（半径 %.3f）\n", rStatic);
    }

    if (g_fail) {
        std::printf("stick_anim: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("stick_anim: 全部通过\n");
    return 0;
}
