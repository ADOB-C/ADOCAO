// PositionTrack 的 justThisTile 消费侧语义（官方：false=**继承**到之后所有层；true=**只作用于本层**）
//
// 为什么需要它：解析器一直把 justThisTile 读进 `FastAction::flag` → `TilePositionOffset::justThisTile`，
// 但 `applyPositionTrackOffsets()` **从不读它**，于是 justThisTile 事件被当成永久偏移 ——
// 事件自己那层是对的，**后面整条轨道被平移**。The Moon 有 64 处这种事件（层 120812~120929，
// positionOffset 全非零），实测抓帧 50.5% 像素不同、砖块被"扇形铺开"。
// 解析侧早有 fixture（f24 覆盖 "Enabled"/"true"/1/"Disabled" 四种写法），缺的就是这个消费侧用例。
//
// 断言的两件事 + 负向对照：
//   1. Enabled/true/1 → 点加（只动本层）；2. 缺省/Disabled → 后缀加（本层及其后所有层）；
//   3. 负向对照：按旧的"一律继承"算，floor 3 必须**过不了**同一条断言（否则断言没牙）。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_position_offset_test）

#include "core/level/LevelData.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

    // 解析侧：两种写法都要被认出来（`justThisTile` 是 flag 的来源）
    if (lv.tilePositionOffsets.size() != 2) {
        std::printf("✗ tilePositionOffsets 应有 2 条，实际 %zu\n", lv.tilePositionOffsets.size());
        return 1;
    }
    if (!lv.tilePositionOffsets.at(2).justThisTile || lv.tilePositionOffsets.at(4).justThisTile) {
        std::printf("✗ flag 解析不对：floor2 应为 true、floor4 应为 false\n");
        return 1;
    }

    // ① 基础：没有偏移的层
    expectPos(lv, 0, 0.0, 0.0, "起点");
    expectPos(lv, 1, 1.0, 0.0, "基础前缀和");

    // ② Enabled（floor 2，+[3,0]）→ 点加：本层位移，**下一层不带**
    expectPos(lv, 2, 5.0, 0.0, "justThisTile 只作用于本层");
    expectPos(lv, 3, 3.0, 0.0, "justThisTile **不许**继承到下一层（本用例的核心断言）");

    // ③ Disabled（floor 4，+[0,2]）→ 后缀加：本层及其后所有层（含末尾 extra 砖）
    expectPos(lv, 4, 4.0, 2.0, "非 justThisTile 作用于本层");
    expectPos(lv, 5, 5.0, 2.0, "非 justThisTile 继承到下一层");
    expectPos(lv, 6, 6.0, 2.0, "继承一直延伸到末尾 extra 砖");

    // ④ 负向对照：复刻"一律继承"的旧行为，它必须在 floor 3 上过不了同一条断言
    {
        const double oldAt3 = 3.0 + 3.0;   // 基础 (3,0) + 累积 3（floor 2 的偏移被永久叠加）
        if (oldAt3 == 3.0) {
            std::printf("  ✗ 负向对照失效：旧行为竟然也算出 (3,0)，说明断言没牙\n");
            failures++;
        } else {
            std::printf("  负向对照：旧行为在 floor 3 得到 (%.1f, 0.0) ≠ (3.0, 0.0) → 断言有牙 ✓\n", oldAt3);
        }
    }

    if (failures == 0) std::printf("position_offset: OK\n");
    return failures == 0 ? 0 : 1;
}
