#pragma once
// 逐砖关键帧时间轴 —— 对齐 Re_ADOJAS `TimelineManager` 的数据模型。
// 语义权威：`~/Repos/Re_ADOJAS/src/lib/Player/TimelineManager.ts`（addKeyframe / pushTween /
// addTweenKillComplete / sample）。设计说明见 docs/animation.md。
#include <cstdint>
#include <vector>

#include "core/util/Easing.hpp"

namespace adofai {

// 一条属性的关键帧序列（time 升序）。语义：
//   * `addKeyframe`：同刻覆盖；
//   * tween = 两个关键帧，后一个带 ease；采样时在 [prev.time, next.time] 内按 ease 插值；
//   * **Kill(complete)**：旧 tween 在事件时刻先跳到自己终点（`jumpToEnd`），再从那里开始新 tween。
class Channel {
public:
    struct Key {
        double time = 0.0;
        float value = 0.0f;
        uint8_t ease = 255;   // 255 = 瞬时（无 tween 段）
    };

    bool empty() const { return m_keys.empty(); }
    size_t size() const { return m_keys.size(); }
    const std::vector<Key>& keys() const { return m_keys; }

    void clear() { m_keys.clear(); }

    // 同刻覆盖（保留原位置）；否则插入到有序位置。
    void addKeyframe(double t, float v);

    // Kill(complete) + tween：[t0, t1] 从 from 到 to，缓动 e。
    // `fallback` 用于"此刻之前没有任何关键帧"时补一个起点（与 Re_ADOJAS 的 fallback 一致）。
    void addTween(double t0, double t1, float from, float to, Ease e, float fallback);

    // 采样：t 之前最后一个关键帧；若它开启了 tween 且下一帧在 t 之后 → 插值。
    float sample(double t) const;

    // t <= t0 时把 keyframe 值设为 v，**不**保留其后 tween（Re_ADOJAS 的 instantKeyframe）。
    void instantKeyframe(double t0, float v);

private:
    std::vector<Key> m_keys;
};

// 一块砖的 6 条通道 + 活跃区间（只给"真的有动画"的砖建）。
struct TileKeyframes {
    Channel posX, posY, rot, scaleX, scaleY, opacity;
    double rangeStart = 0.0;   // 动画活跃区间（含 RANGE_GRACE）
    double rangeEnd = 0.0;

    // 区间**之前/之后**的可见性（建帧后算一次）：绝大多数砖在任一时刻都落在这两段里，
    // 逐帧判定于是变成 O(1)，不必每帧对每块砖做 6 次二分（2026-10-21 的性能要求）。
    bool hiddenBefore = false;
    bool hiddenAfter  = false;

    static constexpr double kRangeGrace = 0.1;   // Re_ADOJAS: RANGE_GRACE
    bool activeAt(double t) const { return t >= rangeStart && t < rangeEnd; }
    // 超过任何关键帧之后的状态就是"最后一帧的值"；没有关键帧时返回 fallback。
    float value(const Channel& c, double t, float fallback) const {
        return c.empty() ? fallback : c.sample(t);
    }
};

}  // namespace adofai
