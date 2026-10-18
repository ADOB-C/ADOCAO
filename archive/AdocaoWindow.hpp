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
#include "core/level/FloorWindow.hpp"    // ③ 共享层：FloorCheckpoint / FloorWindow(Input)
#include "archive/AdocaoBlocked.hpp"    // ColumnView（分块列容器）
#include "archive/AdocaoReader.hpp"     // SectionView（按段取载荷）
#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {

using adofai::FloorCheckpoint;
using adofai::FloorWindow;
using adofai::FloorWindowInput;

// 按块重算（实现留在 archive；由 archive::install() 注册给 core）
bool computeFloorWindow(const LevelData::Settings& settings, const FloorWindowInput& in,
                        FloorWindow& out, std::string& err);

// 一块（4096 层）的**原始**数据：从容器里按块取出来，不含任何全谱数组。
struct FloorBlock {
    uint64_t first = 0;                                   // i0
    size_t count = 0;                                     // 这一块实际有多少层（最后一块可能不满）
    std::vector<double> angles;                           // 原始 angleData（999 保留）
    std::vector<LevelData::FastAction> actions;            // floor ∈ [i0, i0+count) 的事件
    FloorCheckpoint checkpoint;
    bool hasCheckpoint = false;
};

// ③ 的"按需开窗"：打开一个 `.adocao`，之后 `readBlock(b)` **只解压/解码第 b 块要用的东西**。
//
// 为什么要有这一层：`unpackLevel()` 是全量的（它要把整份 LevelData 解出来）。窗口化要的是
// "给一块，别的都不碰"，所以这里直接走段视图 + 分块列容器 + 按层索引。
// `data/length` 必须活到这个对象析构（与 `loadFromBuffer` 的 mmap 语义一致）。
class FloorWindowSource {
public:
    bool open(const uint8_t* data, size_t length, std::string& err);

    uint32_t stride() const { return m_stride; }
    uint64_t blockCount() const { return m_blocks; }
    // angleData 是分块布局（按块寻址的前提）；action 列可能是 legacy（那就整段解一次再切，
    // 仍然正确、但少了那一侧的内存收益）
    bool   windowable() const { return m_windowable; }
    bool   actionsWindowable() const { return m_actionsV3; }
    const char* notWindowableWhy() const { return m_why; }
    // 最近的不大于 block 的 checkpoint（块号；没有则返回 nullptr）
    const FloorCheckpoint* checkpointForBlock(uint64_t block) const;
    // 只解压/解码第 block 块需要的东西
    bool readBlock(uint64_t block, FloorBlock& out, std::string& err);
    // 账：这一路累计解压/解码了多少字节（给"内存与 n 解耦"的实测留证据）
    uint64_t bytesDecoded() const { return m_bytesDecoded; }
    // 字符串池（`atStates` 的动画名要它；很小，open() 时一次读出）
    const std::vector<std::string>& strings() const { return m_strings; }

private:
    uint32_t m_stride = kCheckpointStride;
    uint64_t m_floors = 0;
    uint64_t m_blocks = 0;
    bool m_windowable = false;
    const char* m_why = "";

    SectionView m_angleSec, m_actionsSec, m_derivedSec, m_indexSec, m_poolSec;
    std::vector<std::string> m_strings;
    ColumnView m_angleCol;
    bool m_angleBlocked = false;
    std::vector<double> m_angleWhole;          // 非分块时的整列（退化路径）

    // Actions 段：6 条列（floor/type/strId/flag/val1/val2），每条一个容器视图
    struct ActionColumn {
        ColumnView view;
        bool blocked = false;
        bool sparse = false;
        uint32_t mask = 0;
        uint64_t used = 0;
        std::vector<uint8_t> whole;            // 非分块时整列的原始容器字节
        std::vector<uint32_t> wholeU;           // 退化路径：整列解出来的 u32
        std::vector<int64_t> wholeI;
    };
    ActionColumn m_cols[6];
    bool m_actionsV3 = false;

    uint32_t m_ckStride = 0;
    std::vector<FloorCheckpoint> m_ck;
    std::vector<ActionIndexEntry> m_index;     // 空 = 没有按层索引（退回全扫）
    uint64_t m_bytesDecoded = 0;
};

// 快照里"上一层（i0-1）的有效方向"（`calculateTilePositions` 的 floats[i0-1]）：
// 999 连跑时 = lastAngleDeg + 180 × sentinelRun。方便测试与消费方复用。
float checkpointPrevDirection(const FloorCheckpoint& ck);

}  // namespace adocao
}  // namespace adofai
