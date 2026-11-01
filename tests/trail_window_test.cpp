// 拖尾窗口/采样率：**用户显式给的固定率不能被"点数上限"静默吃掉**。
//
// 2026-10-31 的真事：`--trail-sample-rate 262144`（2^18）在 2.64e6 BPM 那张谱上
// 实际只跑 20480/s —— 因为 `maxPoints` 当时是 8192，0.4 s 的窗口只能装 8192 点。
// 那一段一秒走 17.6 万砖，8192 点 = **每 8.6 砖一个点**，拖尾直接塌成直线。
//
// 这个测试钉三件事：
//   ① 上限够大时，`sampleRate` 必须**原样**等于请求值（不许被 rateMax 之类夹）；
//   ② 上限真的绑住时才截断，且截断值 = maxPoints / span；
//   ③ 默认上限 ≥ 2^18（回归防线：谁再把它调回几千，②的期望值会立刻对不上）。
#include "core/level/LevelData.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

static int g_fail = 0;
static void near(const char* what, double got, double want, double eps = 1e-3) {
    if (std::fabs(got - want) <= eps) std::printf("ok   %s = %.3f\n", what, got);
    else { std::printf("FAIL %s: got %.3f want %.3f\n", what, got, want); ++g_fail; }
}
static void check(const char* what, bool ok) {
    if (ok) std::printf("ok   %s\n", what);
    else { std::printf("FAIL %s\n", what); ++g_fail; }
}

int main() {
    const char* json = R"({"angleData":[0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,
        0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90,0,90],"settings":{"bpm":120}})";
    LevelData lv; Timeline tl;
    if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL load\n"); return 1; }
    tl.build(lv, false);
    const double t = tl.startTimeAt(10);

    PositionSolver::TrailSamplingConfig cfg;
    cfg.duration = 0.4f;
    cfg.fixedRate = 262144.0f;   // = 2^18，用户显式给
    cfg.adaptive = false;
    cfg.sagittaPx = 0.0f;
    cfg.speedAware = false;

    {
        cfg.maxPoints = 262144.0f;
        const auto w = PositionSolver::trailWindow(tl, t, cfg);
        near("① 上限足够 → sampleRate = 请求值", (double)w.sampleRate, 262144.0);
        check("① 没有被 rateMax(4000) 夹", (double)w.sampleRate > 4000.0);
    }
    {
        cfg.maxPoints = 100.0f;   // 0.4 s 窗口装 100 点 → 250/s
        const auto w = PositionSolver::trailWindow(tl, t, cfg);
        near("② 上限绑住 → 截断到 maxPoints/span", (double)w.sampleRate, 250.0);
    }
    {
        cfg.maxPoints = PositionSolver::TrailSamplingConfig{}.maxPoints;   // 默认值
        check("③ 默认点数上限 ≥ 2^18（8192 那个坑不许回来）",
              (double)cfg.maxPoints >= 262144.0 - 1e-6);
        const auto w = PositionSolver::trailWindow(tl, t, cfg);
        near("③ 默认上限下 2^18 仍然原样生效", (double)w.sampleRate, 262144.0);
    }

    {
        // ④ sagitta 是**用户显式**打开的判据，不许被 governor 的 rateMax 夹。
        // 2026-10-31 实测：2.64e6 BPM 的 tile 141628 上 `--trail-sagitta 0.5` 被
        // rateMax=4000 夹成 4000/s（0.01 s 窗口 41 点 = 每砖 0.09 点），而该砖实际要 3.5 万/s。
        PositionSolver::TrailSamplingConfig s4;
        s4.duration = 0.01f;
        s4.fixedRate = 100000.0f;
        s4.rateMax = 4000.0f;
        s4.sagittaPx = 0.5f;
        s4.pixelsPerTile = 90.0f;
        s4.adaptive = false;
        s4.maxPoints = 262144.0f;
        const auto w = PositionSolver::trailWindow(tl, t, s4);
        near("④ sagitta 模式不被 rateMax 夹", (double)w.sampleRate, 100000.0);
        // 反面对照：把 governor 打开，才该被夹。
        s4.adaptive = true;
        const auto w2 = PositionSolver::trailWindow(tl, t, s4);
        near("④ governor 打开时才会夹到 rateMax", (double)w2.sampleRate, 4000.0, 50.0);
    }

    {
        // ⑤ sagitta 的 R 必须**和行星真正绕的半径同源**（ScaleRadius），不是恒 1。
        //    以前用 `Timeline::tileStartDist/tileEndDist`（对所有非末层恒 1.0f）→ `scale:200`
        //    的谱 R 差 2 倍 → sMax 偏大 → 采样率欠采样。
        const char* json2 = R"({"angleData":[0,90,0,90,0,90,0,90,0,90,0,90],
            "settings":{"bpm":120},
            "actions":[{"floor":1,"eventType":"ScaleRadius","scale":200}]})";
        LevelData lv2; Timeline tl2;
        if (!lv2.loadFromBuffer(json2, std::strlen(json2))) { std::printf("FAIL load2\n"); return 1; }
        tl2.build(lv2, false);
        const double t2 = tl2.startTimeAt(3) + tl2.durationAt(3) * 0.5;
        near("⑤ curvatureRadius = ScaleRadius 给的 2.0", PositionSolver::curvatureRadius(tl2, t2), 2.0, 0.05);
    }

    if (g_fail) { std::printf("trail_window: %d 条失败\n", g_fail); return 1; }
    std::printf("trail_window: 全部通过\n");
    return 0;
}
