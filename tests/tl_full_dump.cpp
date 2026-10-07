// 全量 dump：把渲染路径会读的**所有** Timeline 量打出来（全精度），用于二进制级对照。
// 这些访问器在 #1 前后都存在（#1 只动三个派生数组），所以这一个工具在两个状态下都能编。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PositionSolver.hpp"
#include <glm/glm.hpp>
#include <cstdio>
#include <string>
using namespace adofai;
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("用法：%s <谱.adofai>\n", argv[0]); return 2; }
    LevelData lvl;
    if (!lvl.loadFromFile(argv[1])) { std::printf("# 读不了 %s\n", argv[1]); return 1; }
    Timeline tl;
    tl.build(lvl, false);
    const auto& st = tl.tileStartTimes();
    std::printf("# n=%zu preRoll=%.17g audioStartOffset=%.17g totalDuration=%.17g\n",
                st.size(), (double)tl.preRoll(), (double)tl.audioStartOffset(), tl.totalDuration());
    for (size_t i = 0; i < st.size(); ++i)
        std::printf("%zu st=%.17g bpm=%.9g cw=%d dt=%.17g at=%.17g\n", i, (double)st[i],
                    (double)tl.tileBPMs()[i], tl.tileIsCW()[i] ? 1 : 0,
                    i < tl.tileDisappearTimes().size() ? tl.tileDisappearTimes()[i] : 0.0,
                    i < tl.tileAppearTimes().size() ? tl.tileAppearTimes()[i] : 0.0);
    // 真正画到屏幕上的是这个：逐层的行星位置（PositionSolver 的输出）。
    for (size_t i = 0; i < st.size(); ++i) {
        glm::dvec2 red(0.0), blue(0.0);
        PositionSolver::positionAtTile(tl, (double)st[i], (int)i, red, blue);
        std::printf("pos %zu %.17g %.17g %.17g %.17g\n", i, red.x, red.y, blue.x, blue.y);
    }
    const auto ts = tl.getHitsoundTimestamps();
    std::printf("# hitsounds=%zu\n", ts.size());
    for (size_t i = 0; i < ts.size() && i < 100000; ++i) std::printf("hs %zu %.17g\n", i, ts[i]);
    return 0;
}
