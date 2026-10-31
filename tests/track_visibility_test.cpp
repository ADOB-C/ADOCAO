// 轨道可见性：AnimateTrack 的 appear 动画在**淡入窗口之前**必须不可见。
//
// 权威：Re_ADOJAS `TimelineManager.buildAppearKeyframes`（每个 case 的第一句都是
// `addKeyframe(entity, <通道>, 0, <初始状态>, null)` —— 写的是"还没出现"，不是 base）。
//
// 这个测试钉三件事，缺一条都会退回"不应该看见的看见了"：
//   ① `trackAnimation: Fade` + `beatsAhead: N`：远处砖在 t=0 必须**不可见**（opacity 0）；
//   ② 该砖在自己的淡入窗口结束后必须**可见**（opacity 1）—— 防止一刀切全黑；
//   ③ 只有 disappear 的砖（`trackDisappearAnimation: Shrink`）在 t=0 必须**仍然可见** ——
//      这是 `TileAnimation::build` 里那段通用 t=0 基准帧存在的理由（MM2 开头整条轨道消失）。
#include "core/level/LevelData.hpp"
#include "core/timeline/TileAnimation.hpp"
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
static void near(const char* what, double got, double want, double eps = 2e-3) {
    if (std::fabs(got - want) <= eps) std::printf("ok   %s = %.5f\n", what, got);
    else { std::printf("FAIL %s: got %.5f want %.5f\n", what, got, want); ++g_fail; }
}

// 30 块砖、BPM 120（一拍 0.5 s）。`settings` 由调用方给，用来切换全局动画类型。
static const char* kAngles =
    "0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,"
    "0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90";

static bool buildChart(const char* settings, LevelData& lv, Timeline& tl, TileAnimation& anim) {
    char json[2048];
    std::snprintf(json, sizeof(json),
                  R"({"angleData":[%s],"settings":%s})", kAngles, settings);
    if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL load\n"); return false; }
    tl.build(lv, false);
    lv.buildMoveTrackIndex(tl);
    anim.build(lv, tl);
    return true;
}

int main() {
    // ---- ① / ② Fade + beatsAhead ----
    {
        LevelData lv; Timeline tl; TileAnimation anim;
        if (!buildChart(R"({"bpm":120,"trackAnimation":"Fade","beatsAhead":4,"beatsBehind":0})",
                        lv, tl, anim)) return 1;
        // 30 块砖都在 4 拍（2 s）之外 → 除了最初几块，全部应当不可见。
        TileAnimSample s20{};
        const bool has20 = anim.sample(20, 0.0, s20);
        check("tile 20 有动画帧", has20);
        near("① tile 20 @t=0 opacity", s20.opacity, 0.0);
        check("① tile 20 @t=0 hiddenAt", anim.hiddenAt(20, 0.0));

        // 砖 20 的入场时间 ≈ 20 * 0.5 = 10 s；淡入提前 4 拍（2 s）开始、0.25 s 结束。
        TileAnimSample sMid{}, sAfter{};
        anim.sample(20, tl.startTimeAt(20), sMid);
        anim.sample(20, tl.startTimeAt(20) + 1.0, sAfter);
        near("② tile 20 @入场时刻 opacity", sMid.opacity, 1.0);
        near("② tile 20 @入场后 1 s opacity", sAfter.opacity, 1.0);
        check("② tile 20 @入场后 1 s 可见", !anim.hiddenAt(20, tl.startTimeAt(20) + 1.0));

        // 近处那块（行星起点的下一砖）在自己的窗口内也该是可见的。
        TileAnimSample s1{};
        anim.sample(1, tl.startTimeAt(1) + 0.5, s1);
        near("① tile 1 @入场后 opacity", s1.opacity, 1.0);

        // ④ 倒计时（t < 0）：参考实现的 `sample()` 在**第一条关键帧之前**返回 undefined，
        // 调用方回落到 base（透明 1 = 可见）。我们的 `Channel::sample` 会把首帧的值
        // （appear 的"还没出现"= 0）当成 t<0 的值 —— 于是开场/暂停整条轨道是黑的。
        TileAnimSample sNeg{};
        anim.sample(20, -2.0, sNeg);
        near("④ tile 20 @t=-2 opacity（首帧之前回落 base）", sNeg.opacity, 1.0);
        check("④ tile 20 @t=-2 可见（倒计时不许黑屏）", !anim.hiddenAt(20, -2.0));
    }

    // ---- ③ 只有 disappear 的砖不能被 t=0 基准帧连带隐藏 ----
    {
        LevelData lv; Timeline tl; TileAnimation anim;
        if (!buildChart(R"({"bpm":120,"trackDisappearAnimation":"Shrink","beatsBehind":2})",
                        lv, tl, anim)) return 1;
        TileAnimSample s10{};
        const bool has10 = anim.sample(10, 0.0, s10);
        if (has10) {
            near("③ tile 10 @t=0 opacity", s10.opacity, 1.0);
            near("③ tile 10 @t=0 scaleX", s10.scaleX, 1.0);
            check("③ tile 10 @t=0 可见（MM2 那条护栏）", !anim.hiddenAt(10, 0.0));
        } else {
            // 没有帧也意味着可见（走无动画快路径），同样满足这条约束。
            std::printf("ok   ③ tile 10 没有 disappear 帧 → 走可见快路径\n");
        }
    }

    if (g_fail) { std::printf("track_visibility: %d 条失败\n", g_fail); return 1; }
    std::printf("track_visibility: 全部通过\n");
    return 0;
}
