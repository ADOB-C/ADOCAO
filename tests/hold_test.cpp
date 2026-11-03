// Hold（DLC 长按）的解析（2026-11-03）
//
// 权威 `Re_ADOJAS/src/lib/Player/Player.ts::buildHoldLengths`（b4590e4）：
//   * 逐砖独立、**无继承**；没事件的砖 = -1；
//   * `holdLength = duration`（整型，≥0 才算长按）；
//   * `floor >= tiles-1` 的 Hold 直接跳过（没有下一砖挂不住）。
// 它不是"时间线"的一部分（砖的时长仍由角度算），只决定图标与长按带。
//
// 真实用例：`Sot-C - Fledgling/Fledgling.adofai` floor 2768 `duration: 2`；
// `goTe EX - Frums/whiterabbit.adofai` 9 条全是 `duration: 0`。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_hold_test）

#include "core/level/LevelData.hpp"

#include <cstdio>
#include <cstring>

using namespace adofai;

static int failures = 0;

int main() {
    static const char* kLevel = R"JSON({"settings": {"bpm": 60},
 "angleData": [0, 0, 0, 0, 0, 0],
 "actions": [
   {"floor": 2, "eventType": "Hold", "duration": 0},
   {"floor": 4, "eventType": "Hold", "duration": 3, "distanceMultiplier": 150},
   {"floor": 6, "eventType": "Hold", "duration": 2}
 ]})JSON";

    LevelData lv;
    if (!lv.loadFromBuffer(kLevel, std::strlen(kLevel))) {
        std::printf("✗ 用例谱面没加载成功\n");
        return 1;
    }
    const int64_t n = (int64_t)lv.tiles.size();
    std::printf("# n=%lld（angleData 6 项 + 末尾 extra 砖）\n", (long long)n);

    if (lv.holdLengthAt(2) != 0) {
        std::printf("✗ floor2 期望 holdLength=0（短按），实际 %d\n", lv.holdLengthAt(2)); failures++;
    }
    if (lv.holdLengthAt(4) != 3) {
        std::printf("✗ floor4 期望 holdLength=3，实际 %d\n", lv.holdLengthAt(4)); failures++;
    }
    if (lv.holdMultAt(4) != 1.5f) {
        std::printf("✗ floor4 期望 distanceMultiplier 1.5，实际 %.3f\n", lv.holdMultAt(4)); failures++;
    }
    if (lv.holdMultAt(2) != 1.0f) {
        std::printf("✗ 没写 distanceMultiplier 的砖应为 1.0，实际 %.3f\n", lv.holdMultAt(2)); failures++;
    }
    // 无继承：没事件的砖是 -1
    if (lv.holdLengthAt(3) != -1 || lv.holdLengthAt(5) != -1) {
        std::printf("✗ Hold 不该继承（floor3=%d floor5=%d 都应为 -1）\n",
                    lv.holdLengthAt(3), lv.holdLengthAt(5)); failures++;
    }
    // 末尾 extra 砖上写 Hold → 没有下一砖，跳过
    if (lv.holdLengthAt(n - 1) != -1) {
        std::printf("✗ 最后一块砖上的 Hold 应被跳过，实际 %d\n", lv.holdLengthAt(n - 1)); failures++;
    }

    if (failures) { std::printf("hold: %d 处不一致\n", failures); return 1; }
    std::printf("hold: 全部通过（逐砖 / 无继承 / distanceMultiplier / 末砖跳过）\n");
    return 0;
}
