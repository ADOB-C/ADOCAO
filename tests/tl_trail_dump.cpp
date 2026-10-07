// 只 dump 拖尾（trail）：PositionSolver::trailWindow + sampleTrailRange 的全部采样点（全精度）。
// 动机：像素对照图上唯一不同的是红色拖尾；而这是唯一没被 dump 过的量。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PositionSolver.hpp"
#include <glm/glm.hpp>
#include <cstdio>
using namespace adofai;
int main(int argc, char** argv) {
    if (argc < 3) { std::printf("用法：%s <谱.adofai> <时间秒> [更多时间…]\n", argv[0]); return 2; }
    LevelData lvl;
    if (!lvl.loadFromFile(argv[1])) { std::printf("# 读不了 %s\n", argv[1]); return 1; }
    Timeline tl;
    tl.build(lvl, false);
    for (int ai = 2; ai < argc; ++ai) {
        const double t = std::atof(argv[ai]);
        PositionSolver::TrailSamplingConfig cfg{};          // 默认：duration 0.4s、fixedRate 200Hz
        const auto w = PositionSolver::trailWindow(tl, t, cfg);
        glm::dvec2 red(0.0), blue(0.0);
        const int idx = tl.findTileIndex(t);
        PositionSolver::positionAtTile(tl, t, idx < 0 ? 0 : idx, red, blue);
        std::vector<glm::dvec2> rs, bs;
        PositionSolver::sampleTrailRange(tl, w.startTime, t, w.sampleRate, red, blue, rs, bs);
        std::printf("# t=%.17g startTime=%.17g sampleRate=%.9g points=%zu idx=%d\n", t, w.startTime,
                    (double)w.sampleRate, rs.size(), idx);
        for (size_t i = 0; i < rs.size(); ++i)
            std::printf("%zu %.17g %.17g %.17g %.17g\n", i, rs[i].x, rs[i].y, bs[i].x, bs[i].y);
    }
    return 0;
}
