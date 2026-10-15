#pragma once

// ③「每砖结构窗口化」的核心：**从一个 checkpoint + 一块 angleData + 这一块里的事件**，
// 重算出该窗口的每砖数据（`Tile::position/direction`、`tileBPMs`、`tileStartTimes`），
// **不碰任何全谱数组**。
//
// 为什么必须是"纯函数 + 逐位对拍"：这些量在加载路径里是三条**串行前缀和**（位置 / BPM / 时间），
// 从中间某个 i0 续算只有在"起点状态完全正确 + 算式逐字相同 + 浮点类型逐字相同"时才与全谱一次算
// 逐位相同。§8.8 已经把状态收进了 checkpoint，这里负责把算式对齐 —— 验收方式只有一个：
// 与全量加载的数组 **memcmp**（tests/adocao_window_test.cpp）。
//
// 三条与 §8.8 对应的语义（踩错就会在窗口边界上丢一层）：
//   1. checkpoint = "`floor < i0` 的事件全部处理完"之后的状态；
//   2. 本层事件（`floor == i0`）由**本函数**处理（不变量 3）；
//   3. 位置**从快照重新生成**，绝不原地累加（不变量 2）。
//
// ⚠️ 浮点类型必须与生产路径**逐字一致**：`Timeline` 里 `currentBPM/angleDir/tileBPM` 都是
// `float`，`tileStartTimes/position` 是 `double`。类型不一致会在多次 SetSpeed 之后差出 ULP
// （2026-10-15 就是被这条抓出来的）。

#include <cstdint>
#include <string>
#include <vector>

#include "archive/AdocaoCheckpoints.hpp"
#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {

struct FloorWindowInput {
    FloorCheckpoint checkpoint;                      // i0 = checkpoint.floor（必须按 stride 对齐）
    const std::vector<double>* angles = nullptr;      // [i0, i0+count) 的**原始**角度（999 保留）
    const std::vector<LevelData::FastAction>* events = nullptr;  // floor ∈ [i0, i0+count) 的事件
};

struct FloorWindow {
    uint64_t first = 0;                          // i0
    std::vector<LevelData::Tile> tiles;          // position / direction（逐位对齐 calculateTilePositions）
    std::vector<float> tileBPMs;                 // 逐位对齐 Timeline::tileBPMs()
    std::vector<double> tileStartTimes;          // 逐位对齐 Timeline::tileStartTimes()
};

// 失败时 err 是原因（checkpoint 缺分量 / i0 与快照不符 / 缺输入）。
bool computeFloorWindow(const LevelData::Settings& settings, const FloorWindowInput& in,
                        FloorWindow& out, std::string& err);

// 快照里"上一层（i0-1）的有效方向"（`calculateTilePositions` 的 floats[i0-1]）：
// 999 连跑时 = lastAngleDeg + 180 × sentinelRun。方便测试与消费方复用。
float checkpointPrevDirection(const FloorCheckpoint& ck);

}  // namespace adocao
}  // namespace adofai
