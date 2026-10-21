// 时间基准（Re_ADOJAS `getTimeOrigin()`）用例：separateCountdownTime / offset / countdownTicks。
// 2026-10-21 的老坑：之前 preRoll = max(offset, 2 beats)，separateCountdownTime 打开时
// 关卡时间整体晚 countdownTicks*60/bpm，播放/动画时刻与参考实现错位。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;
static int g_fail = 0;
static void near(const char* what, double got, double want, double eps = 1e-3) {
    if (std::fabs(got - want) > eps) { std::printf("FAIL %s: got %.5f want %.5f\n", what, got, want); ++g_fail; }
    else std::printf("ok   %s = %.5f\n", what, got);
}

static void run(const char* name, const char* json, double wantPreRoll) {
    LevelData lv; Timeline tl;
    if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL %s: load\n", name); ++g_fail; return; }
    tl.build(lv, false);
    std::printf("[%s] separateCountdownTime=%d\n", name, (int)lv.settings.separateCountdownTime);
    near(name, tl.preRoll(), wantPreRoll);
}

int main() {
    // 180 BPM、4 ticks、offset 1360ms、separateCountdownTime = true
    // origin = 4*60/180 + 1.36 − tileStartTimes[1]（归一化后 = 0）= 2.69333
    // 实测锚点：Re_ADOJAS elapsed=6200ms 时当前砖 = 14（chart 3.5s）→ 6.2 − 3.5 = 2.7 ≈ 2.693。
    run("sep=true",
        R"({"angleData":[0,0,0,0,0,0,0,0],"settings":{"bpm":180,"offset":1360,"countdownTicks":4,
        "separateCountdownTime":true},"actions":[]})", 2.69333);
    // 同一张谱但 separateCountdownTime = false → origin = offset = 1.36
    run("sep=false",
        R"({"angleData":[0,0,0,0,0,0,0,0],"settings":{"bpm":180,"offset":1360,"countdownTicks":4,
        "separateCountdownTime":false},"actions":[]})", 1.36);
    // 缺字段 → 默认 false（与 Re_ADOJAS 的 `isEnabled(undefined)` 一致）
    run("missing",
        R"({"angleData":[0,0,0,0,0,0,0,0],"settings":{"bpm":120,"offset":500},"actions":[]})", 0.5);
    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("time_origin: 全部通过\n");
    return 0;
}
