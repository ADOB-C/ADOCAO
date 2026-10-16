// ③ 切片 4 验收：**丢掉密集 `tiles` 之后**，`tilePositionAt()/tileDirectionAt()` 走"按块物化"
// 必须与丢之前的密集数组**逐位**相同（`==`，不用容差）。
//
// 做法：加载 → 建 Timeline → 先把密集数组的每层 position/direction 抄一份基准 → 建缓存 → 丢 tiles
// → 逐层比。反面对照：改动 `angleData` 某一层（足以改变该层方向）→ 换一块把单块缓存挤掉 → 重新取
// **必须**与基准不同（否则"逐位对拍"是空的）。第一层用一个非零角度，避免"改了也看不出来"。

#include "archive/Install.hpp"
#include "core/level/FloorWindow.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace adofai;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
}

void testChart(const char* path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::printf("FAIL 载入失败：%s\n", path); ++g_fail; return; }
    Timeline tl;
    tl.build(lv, /*exportOnly=*/false);

    const int64_t n = lv.tileCount();
    // 基准：丢之前的密集数组（逐位）
    std::vector<std::array<double, 2>> wantPos((size_t)n);
    std::vector<float> wantDir((size_t)n);
    for (int64_t i = 0; i < n; ++i) { wantPos[(size_t)i] = lv.tilePositionAt(i); wantDir[(size_t)i] = lv.tileDirectionAt(i); }

    std::string err;
    if (!lv.buildFloorWindowCache(tl.tileStartTimes(), tl.tileBPMs(), tl.tileIsCW(), &err)) {
        std::printf("     跳过（未接：%s）\n", err.c_str());
        return;
    }
    check(lv.dropTilesArray(), "dropTilesArray 应该成功");
    check(lv.tilesWindowed(), "丢完后 tilesWindowed() 应为真");
    check(lv.tiles.empty(), "丢完后密集 tiles 应为空");

    size_t bad = 0, firstBad = (size_t)-1;
    for (int64_t i = 0; i < n; ++i) {
        const bool ok = (lv.tilePositionAt(i) == wantPos[(size_t)i]) &&
                        (lv.tileDirectionAt(i) == wantDir[(size_t)i]);
        if (!ok) { if (bad == 0) firstBad = (size_t)i; ++bad; }
    }
    if (bad) std::printf("     首个不一致在第 %zu 层\n", firstBad);
    check(bad == 0, std::string(path) + "：窗口化 provider 与密集数组不一致");
    std::printf("     %-46s 窗口化 provider：%lld 层逐位不一致 %zu（物化 %zu 块）\n",
                path, (long long)n, bad, lv.windowBlocksMaterialized());

    // 反面对照：改 angleData 的一层 → 挤掉缓存 → 该层方向（以及下一层位置）必须变
    if (n > 5000) {
        // 选一个**非哨兵**（999）的层：哨兵层的方向是沿用前一个有效角度，改它可能看不出变化。
        // 扰动幅度要**可见**：direction 是 float，1e-9 直接落在分辨率以下（我第一版就栽在这）。
        int64_t mid = n / 2;
        while (mid + 1 < n && lv.angleData[(size_t)mid] == 999.0) ++mid;
        const double orig = lv.angleData[(size_t)mid];
        lv.angleData[(size_t)mid] = orig + 37.5;
        lv.invalidateFloorWindowForTest();              // 测试钩子：强制重算（别靠"读另一块挤缓存"）
        const bool dirChanged = !(lv.tileDirectionAt(mid) == wantDir[(size_t)mid]);
        const bool posChanged = (mid + 1 < n) &&
                                !(lv.tilePositionAt(mid + 1) == wantPos[(size_t)(mid + 1)]);
        check(dirChanged || posChanged, "反面对照失败：改了角度后重算竟然没变");
        lv.angleData[(size_t)mid] = orig;
        lv.invalidateFloorWindowForTest();
        const bool dirBack = (lv.tileDirectionAt(mid) == wantDir[(size_t)mid]);
        const bool posBack = (mid + 1 >= n) || (lv.tilePositionAt(mid + 1) == wantPos[(size_t)(mid + 1)]);
        check(dirBack && posBack, "复原后应回到基准值（方向与下一层位置都要回来）");
    }
}

}  // namespace

int main(int argc, char** argv) {
    archive::install();
    if (argc < 2) { std::printf("用法: %s <谱>…\n", argv[0]); return 2; }
    std::printf("=== ③ 切片 4：按块物化 provider 逐层对拍 ===\n");
    for (int i = 1; i < argc; ++i) testChart(argv[i]);
    if (g_fail == 0) std::printf("全部通过\n");
    else std::printf("%d 项不通过\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
