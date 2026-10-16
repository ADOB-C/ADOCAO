#pragma once

#include <memory>

// ③「每砖结构窗口化」的**共享层**：checkpoint 状态、窗口输入/输出类型、以及"按块重算"的钩子。
//
// 为什么放在 core：`LevelData` 要**存** checkpoint、要**缓存**一个窗口，所以这些**类型**必须在
// core；而重算的算式（`computeFloorWindow`）目前住在 archive（它要与分块列容器对接）。
// core 不依赖 archive —— 于是走**依赖倒置**（和 `ArchiveBackend::decodeAdocao` 同一套）：
// core 定义函数指针类型 + 安装点，`archive::install()` 把 `computeFloorWindow` 注册进来。
//
// 三条不变量（§8.8，不遵守就会在窗口边界上丢一层）：
//   1. checkpoint = "`floor < i0` 的事件**全部处理完**"之后的状态；
//   2. 位置**从快照重新生成**，绝不原地累加；
//   3. 窗口的事件扫描必须**包含 `floor == i0`**（`justThisTile` 点加是"只作用于本层"）。
//
// ⚠️ 字段一律 f64：`baseX/baseY`/`cumX/cumY` 都要参与"从快照重算 = 全谱一次算"的**逐位**对拍。

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/level/LevelData.hpp"   // 窗口类型里用到 LevelData 的嵌套类型（Tile/FastAction/ATState）

namespace adofai {

inline constexpr uint32_t kCheckpointStride = 4096;   // 与分块边界/剔除块/锚点同一个数
inline constexpr size_t   kCheckpointBytes = 96;      // 定长记录

// `flags` 的位：哪些分量真的算过（稀疏）。读取方必须按位判，别把 0 当成真值。
enum : uint32_t {
    kCkBase         = 1u << 0,   // baseX/baseY（angleData 前缀和）
    kCkCum          = 1u << 1,   // cumX/cumY（PositionTrack 继承偏移）
    kCkLastAngle    = 1u << 2,   // lastAngleDeg
    kCkLastSentinel = 1u << 3,   // i0-1 层是哨兵 999（midspin 续算要用）
    kCkTime         = 1u << 4,   // timeSec（Timeline 前缀和）
    kCkBpm          = 1u << 5,   // bpm（SetSpeed 继承）
    kCkRadius       = 1u << 6,   // radiusScale（ScaleRadius 继承）—— 本仓库还没实现该事件
    kCkCw           = 1u << 7,   // cw（Timeline Phase 1 的 tileIsCW 初值）
    kCkAngleDir     = 1u << 8,   // angleDirDeg（进这一层的"角度方向"，ADOFAI-JS 语义）
    kCkSentinelRun  = 1u << 9,   // sentinelRun（紧跟 i0 之前的连续 999 个数）
};

struct FloorCheckpoint {
    uint64_t floor = 0;         // 快照对应的层号 i0（4096 的倍数）
    double baseX = 0.0, baseY = 0.0;   // angleData 前缀和（第 i0 层的位置，**不含** offsets）
    double cumX = 0.0, cumY = 0.0;     // PositionTrack 继承偏移
    double timeSec = 0.0;              // tileStartTimes[i0]
    double bpm = 0.0;                  // 第 i0 层的 BPM（SetSpeed 之后）
    double radiusScale = 1.0;          // ScaleRadius 继承（预留；未实现该事件时 = 1.0）
    double lastAngleDeg = 0.0;         // 最后一个**真** angleData 原值（0..360）
    double angleDirDeg = 180.0;        // 进第 i0 层的"角度方向"（归一化过所以有界）
    uint32_t flags = 0;                // 上面那些位
    uint32_t sentinelRun = 0;          // 紧跟 i0 之前连续 999 的个数
    uint8_t  cw = 1;                   // 进第 i0 层时的 tileIsCW 初值
    uint8_t  pad[7] = {0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(FloorCheckpoint) == kCheckpointBytes,
              "FloorCheckpoint 必须与落盘的定长记录逐字节一致（不要往里塞指针/vtable）");

struct FloorWindowInput {
    FloorCheckpoint checkpoint;                      // i0 = checkpoint.floor（必须按 stride 对齐）
    const std::vector<double>* angles = nullptr;      // [i0, i0+count) 的**原始**角度（999 保留）
    const std::vector<LevelData::FastAction>* events = nullptr;  // floor ∈ [i0, i0+count) 的事件
    const std::vector<std::string>* strings = nullptr;           // 事件字符串驻留表（可为空）
};

struct FloorWindow {
    uint64_t first = 0;                          // i0
    std::vector<LevelData::Tile> tiles;          // position / direction（逐位对齐 calculateTilePositions）
    std::vector<float> tileBPMs;                 // 逐位对齐 Timeline::tileBPMs()
    std::vector<double> tileStartTimes;          // 逐位对齐 Timeline::tileStartTimes()
    std::vector<bool>  tileHasTwirl;             // 对齐 LevelData::tileHasTwirl 的窗口切片
    std::vector<bool>  tileHasSetSpeed;          // 同上
    std::map<int, LevelData::ATState> atStates;  // 键 = **相对窗口起点**的层号
    bool hasAtStates = false;
    // 故意不做：`tileHitsounds`/`tileHitsoundVolumes`（跨层前缀，消费方是音频）、
    // `bookmarkFloors`（有全局语义）。见 docs/adocao-format.md §8.13。
};

// 从**已加载**的 LevelData 算 checkpoint（写入方用；明文路径加载收尾也用它）。
// 用与 `calculateTilePositions` / `applyPositionTrackOffsets` **同一套算式**，
// 所以"从快照重算"能逐位对上（§8.10⑥）。stride 必须是 4096 的语义等价值。
bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        std::vector<FloorCheckpoint>& out, std::string& err);
// 同上，但时间/BPM 由调用方给（加载路径已经建过 Timeline，别重复建一次 32 B/层的数组）。
bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        const std::vector<double>& times, const std::vector<float>& bpms,
                        std::vector<FloorCheckpoint>& out, std::string& err);

// ③ 切片 4：每砖数据的**按块物化缓存**。`LevelData` 持有它；`dropTiles()` 成功后
// `tilePositionAt()/tileDirectionAt()` 就走这里。
//
// 关键取舍（实测决定）：物化一块本需要"块内事件"，而 `releaseMemory()` 会释放 `actions`
// （MYC 实测 6,155,055 条 Twirl = 99 MB 的绝大部分）。所以这里**不读 `actions`**，改成
// 从**已经保留的每层数组**现造块内事件（每块只有 4096 层）：
//   * `tileIsCW[i]` 翻转        → 合成一条 Twirl
//   * `tileBPMs[i]` 变化        → 合成一条 SetSpeed（绝对赋值，flag=false）
//   * `tilePositionOffsets[i]`  → 合成一条 PositionTrack（val1/val2 = 偏移，flag = justThisTile）
// 于是喂给**已经逐位验证过的** `computeFloorWindow`（走 `floorRecompute()` 钩子），零新增算式。
// 有 AnimateTrack 的谱不从这条走（`atStates` 需要事件本体）→ 保留密集数组，行为不变。
class FloorWindowCache {
public:
    FloorWindowCache();
    ~FloorWindowCache();
    FloorWindowCache(const FloorWindowCache&) = delete;
    FloorWindowCache& operator=(const FloorWindowCache&) = delete;

    // 加载收尾（finishLoad 之后、Timeline 建好之后、releaseMemory 之前）调用一次
    bool init(LevelData& level, const std::vector<double>& startTimes,
              const std::vector<float>& bpms, const std::vector<bool>& isCW, std::string& err);
    bool ready() const { return m_ready; }
    int64_t tileCount() const { return m_tileCount; }   // 丢数组之后仍然权威（= 当初的 tiles.size()）
    bool windowed() const { return m_windowed; }
    const char* why() const { return m_why; }
    size_t blocksMaterialized() const { return m_blocks; }
    // 第 i 层的 Tile（物化所在块；i == angleData.size() 时是末尾那块合成砖）
    const LevelData::Tile* tileAt(int64_t i) const;
    // 丢掉密集 `tiles` 数组（失败则原样不动）
    bool dropTiles(LevelData& level);
    // 给并行剔除用：**克隆一份只读状态 + 全新可变窗口**（每线程一份 → 不写共享缓存、无竞争；
    // 纯函数派生 → 结果与共享缓存逐位相同）。只读部分（checkpoints / isCW / 砖数）共享同一批值，
    // 所以克隆很便宜（一次，随线程存活）。
    std::unique_ptr<FloorWindowCache> clone() const;

    // **仅测试用**：让单块缓存失效（反面对照要"改了输入 → 必须重算"，
    // 而"读另一块挤掉缓存"在目标层恰好就在块 0 时不成立）
    void invalidateWindowForTest() const { m_block = -1; }

private:
    bool materialize(int64_t block) const;

    LevelData* m_level = nullptr;
    std::vector<FloorCheckpoint> m_ck;
    std::vector<bool> m_isCW;
    LevelData::Tile m_extra{};
    bool m_hasExtra = false;
    int64_t m_tileCount = 0;
    bool m_ready = false;
    bool m_windowed = false;
    const char* m_why = "未初始化";
    mutable int64_t m_block = -1;
    // 复用的重算输出（**不每次重建**：容量保留 → 没有 churn；`tileAt` 直接指进这里）
    mutable FloorWindow m_scratch;
    mutable size_t m_blocks = 0;
};

// 从 checkpoint + 一块 angleData + 这一块里的事件，重算出该窗口的每砖数据（不碰任何全谱数组）。
// 实现由 archive 注册（`archive::install()`）；未注册时返回 false + err。
using FloorRecomputeFn = bool (*)(const LevelData::Settings& settings, const FloorWindowInput& in,
                                  FloorWindow& out, std::string& err);
void installFloorRecompute(FloorRecomputeFn fn);
FloorRecomputeFn floorRecompute();

}  // namespace adofai
