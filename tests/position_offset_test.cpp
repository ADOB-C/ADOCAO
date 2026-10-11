// PositionTrack 的 `justThisTile`：**只作用于本层**（点加）
//
// 三份来源一致（2026-10-11 查证，别再翻来覆去）：
//   * 编辑器指南（adofaieditor.gitbook.io → Position Track → "Only change this tile"）：
//     "If enabled, all settings from this event will only affect the event tile"；
//   * Re_ADOJAS 的移植（src/lib/Player/PositionTrackManager.ts）：justThisTile → pointX[floor] += …，
//     非 justThisTile 才 bitAdd 后缀加；
//   * The Moon 那段上的渲染量出来也对得上：花瓣环离星心 1.5~2.2 倍星半径（点加中位 1.46 / 最大 2.28），
//     而"一律累积"会把花瓣甩到 3~5 倍外 —— 图里没有。
//
// 本用例把这条钉住：Enabled → 只动本层；缺省/Disabled → 本层及之后所有层。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_position_offset_test）

#include "core/level/LevelData.hpp"

#include <cstdio>
#include <cstring>

using namespace adofai;

// angleData 全 0 → 每步都朝 +x，于是"基础位置"就是 (i, 0)，偏移量一眼可读。
static const char* kLevel = R"JSON({"settings": {"bpm": 60},
 "angleData": [0, 0, 0, 0, 0, 0],
 "actions": [
   {"floor": 2, "eventType": "PositionTrack", "positionOffset": [3, 0],   "justThisTile": "Enabled"},
   {"floor": 4, "eventType": "PositionTrack", "positionOffset": [0, 2],   "justThisTile": "Disabled"}
 ]})JSON";

static int failures = 0;

static void expectPos(const LevelData& lv, int floor, double x, double y, const char* why) {
    const auto& p = lv.tiles[(size_t)floor].position;
    if (p[0] == x && p[1] == y) return;
    std::printf("  ✗ floor %d: 期望 (%.3f, %.3f)，实际 (%.3f, %.3f)  —— %s\n",
                floor, x, y, p[0], p[1], why);
    failures++;
}

int main() {
    LevelData lv;
    if (!lv.loadFromBuffer(kLevel, std::strlen(kLevel))) {
        std::printf("✗ 用例谱面没加载成功\n");
        return 1;
    }
    std::printf("# n=%zu（angleData 6 项 + 末尾 extra 砖）\n", lv.tiles.size());

    // 解析侧：两种写法都要被认出来（`justThisTile` 是 flag 的来源，语义待定但解析必须在）
    if (lv.tilePositionOffsets.size() != 2) {
        std::printf("✗ tilePositionOffsets 应有 2 条，实际 %zu\n", lv.tilePositionOffsets.size());
        return 1;
    }
    if (!lv.tilePositionOffsets.at(2).justThisTile || lv.tilePositionOffsets.at(4).justThisTile) {
        std::printf("✗ flag 解析不对：floor2 应为 true、floor4 应为 false\n");
        return 1;
    }

    // 基础：无偏移的层
    expectPos(lv, 0, 0.0, 0.0, "起点");
    expectPos(lv, 1, 1.0, 0.0, "基础前缀和");

    // Enabled（floor 2，+[3,0]）→ 点加：本层位移，**下一层不带**
    expectPos(lv, 2, 5.0, 0.0, "justThisTile 只作用于本层");
    expectPos(lv, 3, 3.0, 0.0, "justThisTile **不许**继承到下一层（本用例的核心断言）");

    // Disabled（floor 4，+[0,2]）→ 后缀加：本层及其后所有层（含末尾 extra 砖）
    expectPos(lv, 4, 4.0, 2.0, "非 justThisTile 作用于本层");
    expectPos(lv, 5, 5.0, 2.0, "非 justThisTile 继承到下一层");
    expectPos(lv, 6, 6.0, 2.0, "继承一直延伸到末尾 extra 砖");

    if (failures == 0) std::printf("position_offset: OK（Enabled=点加 / 缺省=后缀加）\n");
    return failures == 0 ? 0 : 1;
}
