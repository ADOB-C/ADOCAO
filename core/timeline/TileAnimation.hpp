#pragma once
// 逐砖动画 = 关键帧时间轴（对齐 Re_ADOJAS `TimelineManager`）。
// 设计：docs/animation.md。**性能约束**（用户 2026-10-21 特别要求）：
//   * 只有**真的有动画**的砖才建时间轴（稀疏：`排序的砖号 + 连续存储`，不做 per-tile map）；
//   * 每帧只对"活跃砖"做采样（活跃区间按 start 排序 + 前缀 max end，二分定位）；
//   * 采样 = 每通道一次二分 + 一次缓动，**不做分配**（输出写进调用方的结构体）。
#include <cstdint>
#include <vector>

#include "core/timeline/Keyframes.hpp"

namespace adofai {

class LevelData;
class Timeline;

// 一块砖在时刻 t 的动画增量（相对"砖自身几何"的偏移；与旧的 MoveTransform 同语义）。
struct TileAnimSample {
    float posX = 0.0f, posY = 0.0f;   // 位移（砖单位）
    float rot = 0.0f;                 // 旋转增量（弧度，已含"绝对角 − 砖方向"）
    float scaleX = 1.0f, scaleY = 1.0f;
    float opacity = 1.0f;
    bool  animated = false;           // false = 该砖没有任何动画（快路径）
};

class TileAnimation {
public:
    struct Entry { int64_t tile; uint32_t idx; };
    // 建帧：先 MoveTrack、后 AnimateTrack（后者覆盖前者，与 Re_ADOJAS 的顺序一致）。
    // `tl` 提供逐层时间/BPM；`lv` 提供已展开的 MoveTrack 事件与 AT 状态。
    void build(const LevelData& lv, const Timeline& tl);

    bool empty() const { return m_index.empty(); }
    size_t animatedTiles() const { return m_index.size(); }
    size_t keyframeCount() const;
    // 建帧是否因为超出上限被截断（超大谱保护，对应 Re_ADOJAS 的 GLOBAL_ANIM_TILE_LIMIT 思路）
    bool truncated() const { return m_truncated; }

    // 有动画的砖（按砖号升序）—— 逐帧做"透明/缩到 0 → 不可见"的判定用。
    const std::vector<Entry>& entries() const { return m_index; }

    // 该砖在 t 时刻是否"完全不可见"（全部通道都收到 0）。没有动画的砖恒为可见。
    // 区间外走建帧时缓存好的 hiddenBefore/hiddenAfter（O(1)），区间内才采样。
    bool hiddenAt(int64_t tile, double t) const;

    // 采样：tile 不在索引里 → 返回 false（调用方走"无动画"快路径）。
    bool sample(int64_t tile, double t, TileAnimSample& out) const;

    // 活跃砖（活跃区间覆盖 t）—— 供将来的"只处理活跃砖"优化用；现在是纯查询。
    void activeAt(double t, std::vector<int64_t>& out) const;

private:
    std::vector<Entry> m_index;               // 按 tile 升序（二分查找）
    std::vector<TileKeyframes> m_frames;      // 与 m_index 平行
    bool m_truncated = false;

    uint32_t frameIndexFor(int64_t tile) const;   // 返回 UINT32_MAX = 没有
    TileKeyframes& ensureFrame(int64_t tile, double rangeStart, double rangeEnd);
};

}  // namespace adofai
