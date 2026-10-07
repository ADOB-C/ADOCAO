// 把 Timeline 的三个派生数组**全精度**打出来（%.17g），用于"改动前 vs 改动后"的二进制级对照。
// 现在这份代码（改动前）用 tileDurations()/tileStartAngles()/tileTotalAngles()；
// 改动后（#1）这三个访问器会被 durationAt()/startAngleAt()/totalAngleAt() 取代 —— 届时改这里即可。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include <cstdio>
#include <string>
using namespace adofai;
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("用法：%s <谱.adofai>\n", argv[0]); return 2; }
    LevelData lvl;
    if (!lvl.loadFromFile(argv[1])) { std::printf("# 读不了 %s\n", argv[1]); return 1; }
    Timeline tl;
    tl.build(lvl, false);
    const auto& d = tl.tileDurations();
    const auto& s = tl.tileStartAngles();
    const auto& t = tl.tileTotalAngles();
    const auto& st = tl.tileStartTimes();
    std::printf("# n=%zu\n", d.size());
    for (size_t i = 0; i < d.size(); ++i)
        std::printf("%zu %.9g %.9g %.9g %.17g\n", i, (double)d[i], (double)s[i], (double)t[i], (double)st[i]);
    return 0;
}
