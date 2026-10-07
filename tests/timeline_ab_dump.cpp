// 量出"换个代码位置重算"会把 Timeline 的三个派生数组改变多少。
//
// 背景：TODO 的 2^31 任务 ②(b) 曾认为这 12 B/层 可以整个不存、改成按需重算（省 26 GB）。
// 实测发现重算出来**不是同一个 float**（同一算式在不同函数里会被 -O3 编成不同结构），
// 于是像素门槛 51 个状态里有 31 个变色。这个工具就是用来把"差多少"从推测变成数字的。
//
// 用法：adocao_timeline_ab_dump <谱.adofai> [更多谱…]
// 它**不参与生产路径**，也不做断言：只打印差异统计（最大绝对差、出现在第几层、前缀和漂移）。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {
struct Stat { int diff = 0; double maxAbs = 0.0; int at = -1; };

Stat compare(const std::vector<float>& a, const std::vector<float>& b) {
    Stat s;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) == 0) continue;   // 逐位比
        s.diff++;
        const double d = std::fabs((double)a[i] - (double)b[i]);
        if (d > s.maxAbs) { s.maxAbs = d; s.at = (int)i; }
    }
    return s;
}

void report(const char* what, const Stat& s, double scale, int n) {
    if (s.diff == 0) { std::printf("  %-13s 逐位相同 ✓（%d 层）\n", what, n); return; }
    std::printf("  %-13s 不同 %d/%d 层；max|Δ|=%.3g（第 %d 层）；相对 %.2g\n",
                what, s.diff, n, s.maxAbs, s.at, scale > 0 ? s.maxAbs / scale : 0.0);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("用法：%s <谱.adofai> …\n", argv[0]); return 2; }
    int bad = 0;
    for (int ai = 1; ai < argc; ++ai) {
        LevelData lvl;
        if (!lvl.loadFromFile(argv[ai])) { std::printf("  跳过（读不了）：%s\n", argv[ai]); ++bad; continue; }
        Timeline tl;
        tl.build(lvl, false);
        std::vector<float> rs, rt, rd;
        tl.recomputeForTest(rs, rt, rd);
        const int n = (int)tl.tileDurations().size();
        std::printf("%s（%d 层）\n", argv[ai], n);
        report("durations",   compare(tl.tileDurations(),    rd), 1.0, n);
        report("startAngles", compare(tl.tileStartAngles(),  rs), 3.14159265, n);
        report("totalAngles", compare(tl.tileTotalAngles(),  rt), 3.14159265, n);

        // 下游：用"现算的 durations"重做前缀和，和留存下来的 tileStartTimes 比 —— 这才是
        // 真正会移动画面的量（durations 的差会沿层累积）。
        // 注意：真实的 tileStartTimes 做过一次整体平移（Phase 3 末尾把 st[1] 减掉），
        // 所以比较必须从**同一个基准**出发 —— 否则会量出一个常量偏移（我第一版就这么错了 ✗）。
        const auto& st = tl.tileStartTimes();
        double sum = st.empty() ? 0.0 : (double)st[0], maxDrift = 0.0; int driftAt = -1;
        for (int i = 0; i < n; ++i) {
            const double d = std::fabs(sum - (double)st[i]);
            if (d > maxDrift) { maxDrift = d; driftAt = i; }
            if (i < n - 1) sum += (double)rd[i];
        }
        std::printf("  %-13s max|Δ|=%.3g s（第 %d 层）；相对 %.2g\n",
                    "前缀和", maxDrift, driftAt, st.empty() ? 0.0 : maxDrift / std::fabs((double)st[n - 1]));
        std::printf("\n");
    }
    return bad == argc - 1 ? 1 : 0;
}
