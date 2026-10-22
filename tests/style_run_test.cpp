// 样式段（`LevelData::trackStyleRunEnd`）的区间不变量测试（ctest `style_run`）。
//
// 主张：对每个 floor，`trackStyleAt(f)` 在 f ∈ [floor, trackStyleRunEnd(floor)) 上必须与
// `trackStyleAt(floor)` **逐字符相同**。这是 render/TileMesh.cpp 的 deriveRange 按 run 复用
// 样式的正确性前提 —— 一旦区间算宽了，整片轨道的 trackStyle（几何档位）会错，而
// `ADOCAO_CULL_DUMP` 只含**可见砖号**、抓不到这类错误（像素门槛也只覆盖有限状态）。
//
// 用例：项目自带的 f39（ColorTrack + justThisTile + inactive/editorOnly + 同层多条）与
// f40（RecolorTrack：负向范围、angleOffset 90、gapLength 1、active:false）。
// 两个 Timeline 口径（有/无）都验。**必须调 buildRecolorIndex**（app 的 LevelLoader 在
// Timeline 建好后调它；测试里漏了的话 RecolorTrack 全被忽略，用例会变成假绿）。
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

using namespace adofai;

namespace {

int g_fail = 0;

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const double kInf = std::numeric_limits<double>::infinity();

bool hasActiveRecolor(const LevelData& lv) {
    for (const LevelData::ColorEvent& e : lv.colorEvents)
        if ((e.flags & LevelData::kColorIsRecolor) &&
            !(e.flags & (LevelData::kColorInactive | LevelData::kColorEditorOnly)))
            return true;
    return false;
}

// 返回该谱最长的一个 run 长度（>=1）；顺带断言"逐层不变"。
// `allowSingleRun` 置位 = 该谱只有一个 4096 块且块里有 RecolorTrack 触发（此时"不复用"正确）。
int64_t checkChart(const char* path, bool withTimeline, bool* allowSingleRun) {
    *allowSingleRun = false;
    LevelData lv;
    if (!lv.loadFromString(readAll(path))) {
        std::printf("FAIL 载入 %s\n", path);
        ++g_fail;
        return 0;
    }
    Timeline tl;
    if (withTimeline) {
        tl.build(lv);
        lv.buildRecolorIndex(tl);        // 与 app/LevelLoader.cpp 的加载顺序一致
    }
    if (!withTimeline)
        std::printf("  %s: n=%lld colorEvents=%zu\n", path, (long long)lv.tileCount(),
                    lv.colorEvents.size());
    const Timeline* tp = withTimeline ? &tl : nullptr;
    const int64_t n = (int64_t)lv.tileCount();
    // 整张谱只有一个块、且块里有触发 → 保守实现下没有可复用区间是**正确**的。
    *allowSingleRun = withTimeline && hasActiveRecolor(lv) && n <= 4096;
    int64_t longest = 1;
    for (int64_t i = 0; i < n; ++i) {
        const std::string a = lv.trackStyleAt(i, kInf, tp);
        const LevelData::StyleRun run = lv.trackStyleRunEnd(i, kInf, tp);
        // `end` 在两种情况下都只是"边界"：reusable=true 时是**可复用区间**的上界，
        // reusable=false 时是**本块结束层号**（本块不可复用，调用方逐层查询）。
        const int64_t end = run.end;
        if (end <= i) {
            std::printf("FAIL %s（tl=%d）floor=%lld: runEnd=%lld <= floor\n", path, (int)withTimeline,
                        (long long)i, (long long)end);
            ++g_fail;
            continue;
        }
        if (run.reusable && end > i) {
            // 真正可复用的区间：区间内每一层都必须与起点逐字符相同。
            if (end - i > longest) longest = end - i;
            for (int64_t f = i + 1; f < end && f < n; ++f) {
                if (lv.trackStyleAt(f, kInf, tp) != a) {
                    std::printf("FAIL %s（tl=%d）floor=%lld: f=%lld 的样式与区间起点不同\n", path,
                                (int)withTimeline, (long long)i, (long long)f);
                    ++g_fail;
                    break;
                }
            }
            i = end - 1;   // 区间已整体验过
        }
        // reusable=false = 本块不可复用：调用方逐层查询，没有需要验证的常量区间。
    }
    return longest;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: %s <fixture...>\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; ++a) {
        for (int withTl = 0; withTl <= 1; ++withTl) {
            bool allowSingleRun = false;
            const int64_t longest = checkChart(argv[a], withTl != 0, &allowSingleRun);
            std::printf("  %s（tl=%d）最长 run %lld 层\n", argv[a], withTl, (long long)longest);
            // 退化护栏：实现若永远只返回 floor+1（"正确但完全不复用"），这里报警。
            if (longest < 2 && !allowSingleRun) {
                std::printf("FAIL %s（tl=%d）：没有任何可复用的区间（最长 run < 2）\n", argv[a], withTl);
                ++g_fail;
            }
        }
    }
    if (g_fail) {
        std::printf("style_run: %d 处失败\n", g_fail);
        return 1;
    }
    std::printf("style_run: OK\n");
    return 0;
}
