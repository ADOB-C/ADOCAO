// PositionTrack 的 `justThisTile`：**当前决定**与"为什么还没翻案"
//
// 本用例钉的是"positionOffset 一律后缀加（累积）"，不是"点加"。这不是因为点加没道理，而是因为
// 两边的证据冲突、而**画面站在累积这一边**（2026-10-11 查证）：
//
//   * 编辑器指南（adofaieditor.gitbook.io → Position Track → "Only change this tile"）说
//     "If enabled, all settings from this event will only affect the event tile" → 支持点加；
//   * Re_ADOJAS 的移植（src/lib/Player/PositionTrackManager.ts）也把 Enabled 实现成点加；
//   * **但官方对 The Moon 的实际渲染是累积**：该谱 64 个 Enabled 且 positionOffset 非零的事件，
//     分成 4 组 × 16 个，每组偏移方向每步转 22.5°（16 步整整一圈）——累积这 16 个旋转向量
//     **正好画出一个环**，与原版画面里那 4 个"花结"逐个吻合（环径 ≈ 5 砖）。点加只会得到 ≈3 砖的一坨。
//
// 所以：本用例把"画面一致的行为"钉住 —— 谁要改成点加，必须先拿到原版的**定点实验**证据
// （最小谱面：一个 Enabled + 非零 positionOffset，看**下一块砖**有没有跟着动），否则这个用例会红，
// 强制做一次有意识的决定。
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

    // Enabled（floor 2，+[3,0]）：当前决定 = 后缀加 → 本层与**之后所有层**都吃到这 3
    expectPos(lv, 2, 5.0, 0.0, "事件砖本身");
    expectPos(lv, 3, 6.0, 0.0, "positionOffset 累积（画面一致的行为；点加的话这里是 (3,0)）");

    // Disabled（floor 4，+[0,2]）：同样后缀加
    expectPos(lv, 4, 7.0, 2.0, "第二个事件砖");
    expectPos(lv, 5, 8.0, 2.0, "累积继续");
    expectPos(lv, 6, 9.0, 2.0, "一直延伸到末尾 extra 砖");

    // 把"另一种读法"的数值摆出来：翻案时这两个数就是判别式
    std::printf("  点加读法会得到：floor 3 = (3.0, 0.0)、floor 4 = (4.0, 2.0)（本用例故意不采用）\n");

    if (failures == 0) std::printf("position_offset: OK（positionOffset = 一律后缀加）\n");
    return failures == 0 ? 0 : 1;
}
