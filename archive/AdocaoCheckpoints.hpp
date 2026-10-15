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

namespace adofai {

struct LevelData;

namespace adocao {

inline constexpr uint32_t kCheckpointStride = 4096;   // **与分块边界同一个数**（§8.8：三者同界）
inline constexpr size_t   kCheckpointBytes = 96;      // 定长记录（§8.8 估的 ~88 B + 中旋游程）

// `flags` 的位：哪些分量真的算过（稀疏）。读取方必须按位判，别把 0 当成真值。
enum : uint32_t {
    kCkBase       = 1u << 0,   // baseX/baseY（angleData 前缀和）
    kCkCum        = 1u << 1,   // cumX/cumY（PositionTrack 继承偏移）
    kCkLastAngle  = 1u << 2,   // lastAngleDeg
    kCkLastSentinel = 1u << 3, // i0-1 层是哨兵 999（midspin 续算要用）
    kCkTime       = 1u << 4,   // timeSec（Timeline 前缀和）—— 需要 Timeline 的续算实现
    kCkBpm        = 1u << 5,   // bpm（SetSpeed 继承）——      同上
    kCkRadius     = 1u << 6,   // radiusScale（ScaleRadius 继承）—— 本仓库还没实现该事件
    kCkCw         = 1u << 7,   // cw（Timeline Phase 1 的 tileIsCW 初值）
    kCkAngleDir   = 1u << 8,   // angleDirDeg（进这一层的"角度方向"，ADOFAI-JS 语义）
    kCkSentinelRun = 1u << 9,  // sentinelRun（紧跟 i0 之前的连续 999 个数）
};

struct FloorCheckpoint {
    uint64_t floor = 0;         // 快照对应的层号 i0（4096 的倍数）
    double baseX = 0.0, baseY = 0.0;   // angleData 前缀和（第 i0 层的位置，**不含** offsets）
    double cumX = 0.0, cumY = 0.0;     // PositionTrack 继承偏移
    double timeSec = 0.0;              // tileStartTimes[i0]
    double bpm = 0.0;                  // 第 i0 层的 BPM（SetSpeed 之后）
    double radiusScale = 1.0;          // ScaleRadius 继承（预留；未实现该事件时 = 1.0）
    double lastAngleDeg = 0.0;         // 最后一个**真** angleData 原值（0..360）
    // 进第 i0 层的"角度方向"（0..360，**归一化过所以有界** —— §8.8 担心的"累积方向会无界增长"
    // 指的是未归一化的累加量，这里不是那个东西）。Timeline 续算要用它算第 i0 层的 relAngle。
    double angleDirDeg = 180.0;
    uint32_t flags = 0;                // 上面那些位
    // 紧跟 i0 之前**连续**的 999 个数。要它是因为 `calculateTilePositions` 的
    // `floats[i] = floats[i-1] + 180`（999 时的规则）是**沿用上一个"有效"方向**，
    // 而有效方向在 999 连跑时要靠这个游程长度才能从 lastAngleDeg 推回来：
    //   floats[i0-1] = lastAngleDeg + 180 × sentinelRun
    // （存 `floats[i0-1]` 本身也行，但那正是 §8.8 提醒的"会无界增长"的那个量，所以存游程。）
    uint32_t sentinelRun = 0;
    uint8_t  cw = 1;                   // 进第 i0 层时的 tileIsCW 初值（是否顺时针）
    uint8_t  pad[7] = {0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(FloorCheckpoint) == kCheckpointBytes,
              "FloorCheckpoint 必须与落盘的定长记录逐字节一致（不要往里塞指针/vtable）");

// 从**已加载**的 LevelData 算 checkpoint（写入方用）。用与 `calculateTilePositions` /
// `applyPositionTrackOffsets` **同一套算式**，所以"从快照重算"能逐位对上（§8.10⑥）。
// stride 必须是 4096 的语义等价值（默认 kCheckpointStride）。
bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        std::vector<FloorCheckpoint>& out, std::string& err);

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
