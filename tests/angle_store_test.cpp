// ③ 切片 4 另一半验收：`angleData` → **位精确紧凑存储**（字典 + 位打包下标）。
//
// 三件事必须逐位不变（都用 `==`/`memcmp`，不用容差）：
//   1. `angleAt(i)` 与释放前的 `angleData[i]`；
//   2. `Timeline::totalAngleAt(i)` —— 它内部读 `angleAt(i)`（行星/拖尾转动的那一支）；
//   3. `TileShape::keyForTile()` 的三个输出（形状分组用，含 `angleAt(i) == 999.0` 的中旋判定）。
// 反面对照：`setAngleForTest` 把某层改成字典里**另一个**取值 → 该层 `angleAt` 必须变
// （否则"逐位对拍"是空的）。另外报出 B/层与取值集大小，方便盯住"字典退化"。

#include "core/level/AngleStore.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "render/TileShape.hpp"

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

    const int64_t n = lv.angleCount();
    // 基准：释放前
    std::vector<double> wantAngle((size_t)n);
    std::vector<float> wantTotal((size_t)n);
    std::vector<float> wantSa((size_t)n), wantEa((size_t)n);
    std::vector<uint8_t> wantMid((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        wantAngle[(size_t)i] = lv.angleAt(i);
        wantTotal[(size_t)i] = tl.totalAngleAt(i);
        float sa, ea; bool mid;
        TileShape::keyForTile(lv, (int)i, sa, ea, mid);
        wantSa[(size_t)i] = sa; wantEa[(size_t)i] = ea; wantMid[(size_t)i] = mid ? 1 : 0;
    }

    std::string err;
    if (!lv.releaseAngleData(&err)) {
        std::printf("     跳过（未启用：%s）\n", err.c_str());
        return;
    }
    check(lv.angleData.empty(), "释放后 angleData 应为空");
    check(lv.anglesCompacted(), "anglesCompacted() 应为真");

    size_t badAngle = 0, badTotal = 0, badKey = 0, firstBad = (size_t)-1;
    for (int64_t i = 0; i < n; ++i) {
        if (!(lv.angleAt(i) == wantAngle[(size_t)i])) { if (!badAngle) firstBad = (size_t)i; ++badAngle; }
        if (!(tl.totalAngleAt(i) == wantTotal[(size_t)i])) ++badTotal;
        float sa, ea; bool mid;
        TileShape::keyForTile(lv, (int)i, sa, ea, mid);
        if (!(sa == wantSa[(size_t)i]) || !(ea == wantEa[(size_t)i]) || (mid ? 1 : 0) != wantMid[(size_t)i])
            ++badKey;
    }
    if (badAngle) std::printf("     首个 angleAt 不一致在第 %zu 层\n", firstBad);
    check(badAngle == 0, std::string(path) + "：angleAt 与释放前不一致");
    check(badTotal == 0, std::string(path) + "：totalAngleAt 与释放前不一致");
    check(badKey == 0, std::string(path) + "：keyForTile 与释放前不一致");
    std::printf("     %-46s %lld 层 / %lld 个取值 / %d bit → %.2f B/层（%.1f MB → %.1f MB）"
                "｜不一致 angle %zu total %zu key %zu\n",
                path, (long long)n, (long long)lv.angleStoreUnique(), lv.angleStoreBitsPerIndex(),
                (double)lv.angleStoreBytes() / (double)(n ? n : 1),
                (double)n * 8.0 / 1048576.0, (double)lv.angleStoreBytes() / 1048576.0,
                badAngle, badTotal, badKey);

    // 反面对照：改成字典里另一个取值 → angleAt 必须变
    if (n > 100) {
        const int64_t mid = n / 2;
        const double orig = lv.angleAt(mid);
        double other = orig;
        for (int64_t k = 0; k < n && other == orig; ++k) other = wantAngle[(size_t)k];   // 找一个不同取值
        if (other != orig) {
            lv.setAngleForTest(mid, other);
            check(!(lv.angleAt(mid) == orig), "反面对照失败：改成另一个取值后 angleAt 没变");
            lv.setAngleForTest(mid, orig);
            check(lv.angleAt(mid) == orig, "复原后应回到原值");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("用法: %s <谱>…\n", argv[0]); return 2; }
    std::printf("=== ③ 切片 4：angleData 紧凑存储逐位对拍 ===\n");
    for (int i = 1; i < argc; ++i) testChart(argv[i]);
    if (g_fail == 0) std::printf("全部通过\n");
    else std::printf("%d 项不通过\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
