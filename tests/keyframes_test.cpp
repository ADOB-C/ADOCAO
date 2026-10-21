// 逐砖关键帧时间轴的语义用例（对齐 Re_ADOJAS TimelineManager 的 addKeyframe/pushTween/
// addTweenKillComplete/sample）。设计见 docs/animation.md。
#include "core/timeline/Keyframes.hpp"

#include <cmath>
#include <cstdio>

using namespace adofai;

static int g_fail = 0;
static void expectNear(const char* what, double got, double want, double eps = 1e-5) {
    if (std::fabs(got - want) > eps) {
        std::printf("FAIL %s: got %.6f want %.6f\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("ok   %s = %.6f\n", what, got);
    }
}

int main() {
    // 1) 单帧：常数
    {
        Channel c;
        c.addKeyframe(0.0, 2.0f);
        expectNear("单帧 t=5", c.sample(5.0), 2.0);
    }
    // 2) Linear tween：0→10 在 [0,1]
    {
        Channel c;
        c.addTween(0.0, 1.0, 0.0f, 10.0f, Ease::Linear, 0.0f);
        expectNear("linear t=0.5", c.sample(0.5), 5.0);
        expectNear("linear t=2 (结束后保持终点)", c.sample(2.0), 10.0);
        expectNear("linear t=-1 (开始前保持起点)", c.sample(-1.0), 0.0);
    }
    // 3) OutQuad tween：中点 1-(1-0.5)^2 = 0.75
    {
        Channel c;
        c.addTween(0.0, 1.0, 0.0f, 100.0f, Ease::OutQuad, 0.0f);
        expectNear("outquad t=0.5", c.sample(0.5), 75.0);
    }
    // 4) Kill(complete)：旧 tween 在 t=1 先跳到自己终点(10)，再从 10 tween 到 20
    {
        Channel c;
        c.addTween(0.0, 2.0, 0.0f, 10.0f, Ease::Linear, 0.0f);
        c.addTween(1.0, 3.0, 10.0f, 20.0f, Ease::Linear, 10.0f);
        expectNear("kill: t=0.5 仍在旧 tween 上", c.sample(0.5), 5.0);
        expectNear("kill: t=1.0 跳到旧终点并作为新起点", c.sample(1.0), 10.0);
        expectNear("kill: t=2.0 新 tween 中点", c.sample(2.0), 15.0);
        expectNear("kill: t=3.0 终点", c.sample(3.0), 20.0);
    }
    // 5) 零时长（瞬时）：t0 之后直接是新值
    {
        Channel c;
        c.addKeyframe(0.0, 1.0f);
        c.addTween(2.0, 2.0, 7.0f, 7.0f, Ease::Linear, 1.0f);
        expectNear("瞬时: t=1 仍为旧值", c.sample(1.0), 1.0);
        expectNear("瞬时: t=2 直接 7", c.sample(2.0), 7.0);
    }
    // 6) instantKeyframe 清掉之后的 tween
    {
        Channel c;
        c.addTween(0.0, 5.0, 0.0f, 100.0f, Ease::Linear, 0.0f);
        c.instantKeyframe(2.0, 42.0f);
        expectNear("instant: t=3 保持 42", c.sample(3.0), 42.0);
    }
    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("keyframes: 全部通过\n");
    return 0;
}
