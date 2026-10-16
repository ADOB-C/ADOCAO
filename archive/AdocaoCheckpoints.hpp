#pragma once

// `.adocao` v3 的 `Derived` 段：**每 4096 层的续算快照**（`docs/adocao-format.md` §8.8 ⑧）。
//
// 为什么需要它：把一列按 4096 切成"可寻址的块"只解决了**数据**在哪；要真的从一个窗口开始算，
// 还需要"**算到第 i0 层时状态是什么**"。其中
//   * 基础位置只是 `angleData` 的前缀和（纯函数，窗口内可续算）；
//   * 而 `cumX/cumY`（PositionTrack 的继承偏移）是**事件驱动状态** —— 从第 i0 层续算要么扫遍
//     `floor < i0` 的全部事件（O(层数)），要么就有快照。**它是唯一"不能只靠窗口内数据算出来"的东西**
//     （§8.8 的★优先项）。
//
// 三条不变量（§8.8，不遵守就会重犯别人的 bug）：
//   1. 快照语义 = "`floor < i0` 的事件**全部处理完**"之后的状态（闭包语义）；
//   2. 窗口重算必须**从快照重新生成** position，绝不原地累加；
//   3. 窗口的事件扫描必须**包含 `floor == i0`** 的事件（`justThisTile` 点加是"只作用于本层"）。
//
// ⚠️ 字段一律 **f64**，不许出现 f32：`baseX/baseY` 与 `cumX/cumY` 都要参与"从快照重算 = 全谱一次算"
// 的**逐位**对拍，f32 的 6e-8 相对误差过不了 memcmp（§8.8 实测过这个坑）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/level/FloorWindow.hpp"   // ③ 共享层：FloorCheckpoint / 常量 / 位标志 / computeCheckpoints

namespace adofai {

struct LevelData;

namespace adocao {

// 这些搬去了 core（明文路径也要用）：这里保留名字，archive 内部代码不用改。
using adofai::FloorCheckpoint;
using adofai::kCheckpointBytes;
using adofai::kCheckpointStride;
using adofai::computeCheckpoints;

// `Derived` 段载荷：u32 stride | u32 flags | u64 count | count × 80 B 记录
bool encodeCheckpoints(const std::vector<FloorCheckpoint>& ck, uint32_t stride,
                       std::vector<uint8_t>& out);
bool decodeCheckpoints(const uint8_t* p, size_t n, uint32_t& stride,
                       std::vector<FloorCheckpoint>& out);

// `ActionIndex` 段载荷（每 4096 层一条）：
//   u32 stride | u32 flags | u64 count | count × 条目
//   flags & 1 → 条目 32 B：{ u64 firstAction, u64 usedStrId, u64 usedVal1, u64 usedVal2 }
//   否则      → 条目  8 B：{ u64 firstAction }（早期布局，读侧仍认）
// `usedXxx` 是稀疏列（strId/val1/val2）**按出现次数**编号的起点 —— 窗口要从稀疏列里取
// action 区间 [a0,a1)，就得知道"第 a0 个 action 之前这一列用了多少个"，否则只能从头数。
struct ActionIndexEntry {
    uint64_t firstAction = 0;
    uint64_t usedStrId = 0, usedVal1 = 0, usedVal2 = 0;
};
bool encodeActionIndex(const std::vector<ActionIndexEntry>& entries, uint32_t stride,
                       std::vector<uint8_t>& out);
bool decodeActionIndex(const uint8_t* p, size_t n, uint32_t& stride,
                       std::vector<ActionIndexEntry>& out);

// **§8.10⑥ 的对拍**：从 checkpoint 重算 [i0, i1) 的 position，与 `level.tiles[i].position`
// **逐位**比较（memcmp）。返回不一致的层数；0 = 全同。`firstBad` 给出第一个不一致的层号。
size_t verifyCheckpointResume(const LevelData& level, const FloorCheckpoint& ck,
                              size_t i0, size_t i1, size_t* firstBad = nullptr);

}  // namespace adocao
}  // namespace adofai
