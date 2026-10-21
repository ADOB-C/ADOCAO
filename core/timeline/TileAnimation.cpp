#include "core/timeline/TileAnimation.hpp"

#include <algorithm>
#include <cmath>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"

namespace adofai {
namespace {
// 关键帧总量上限：超过就停止建帧并告警（超大谱保护；Re_ADOJAS 用 GLOBAL_ANIM_TILE_LIMIT）。
constexpr size_t kMaxKeyframes = 4'000'000;
constexpr size_t kMaxAnimatedTiles = 500'000;
}  // namespace

size_t TileAnimation::keyframeCount() const {
    size_t n = 0;
    for (const auto& f : m_frames)
        n += f.posX.size() + f.posY.size() + f.rot.size() + f.scaleX.size() + f.scaleY.size() +
             f.opacity.size();
    return n;
}

uint32_t TileAnimation::frameIndexFor(int64_t tile) const {
    auto it = std::lower_bound(m_index.begin(), m_index.end(), tile,
                               [](const Entry& e, int64_t v) { return e.tile < v; });
    if (it == m_index.end() || it->tile != tile) return UINT32_MAX;
    return it->idx;
}

TileKeyframes& TileAnimation::ensureFrame(int64_t tile, double rangeStart, double rangeEnd) {
    auto it = std::lower_bound(m_index.begin(), m_index.end(), tile,
                               [](const Entry& e, int64_t v) { return e.tile < v; });
    if (it != m_index.end() && it->tile == tile) return m_frames[it->idx];
    if (m_frames.size() >= kMaxAnimatedTiles) {
        m_truncated = true;
        static bool warned = false;
        if (!warned) { LOG_W("TileAnimation: 动画砖数超过上限 %zu，后续砖不建帧", kMaxAnimatedTiles); warned = true; }
        // 返回一个丢弃用的槽（保持引用语义）：把它挂到 tile=-1
        static TileKeyframes dummy;
        dummy = TileKeyframes{};
        return dummy;
    }
    const uint32_t idx = (uint32_t)m_frames.size();
    m_frames.emplace_back();
    m_frames.back().rangeStart = rangeStart;
    m_frames.back().rangeEnd = rangeEnd;
    m_index.insert(it, Entry{tile, idx});
    return m_frames.back();
}

void TileAnimation::build(const LevelData& lv, const Timeline& tl) {
    m_index.clear();
    m_frames.clear();
    m_truncated = false;

    // 关键帧就用**我们的 chart 时间**（tile1 = 0）。Re_ADOJAS 的关键帧建在"未归一化"的
    // tileStartTimes 上（tile1 = (ticks−1)·SPB），但它的 `getTimeOrigin()` 同样减掉了这个基准，
    // 所以采样时刻与关键帧时刻在**同一条时间线**上 —— 不需要在这里加偏移。
    // （2026-10-21 我先误判成"动画晚 shift 秒"，后来用 harness 以本仓库网格复算才确认：
    //  它的 sample(t) 与我们的 chart 时间一一对应。）
    const double animShift = 0.0;

    // ---- 1) MoveTrack：逐砖建 tween（**先**建，之后被 AnimateTrack 覆盖） ----
    const auto& moves = lv.moveEvents();
    if (!moves.empty()) {
        // 按砖分组：范围 [lo..hi] 步长 gap+1，全部展开成 (tile, event) 对再排序，
        // 排序键 = (trigger, order) —— 与 Re_ADOJAS 的 floor 升序、同层按 id 升序一致。
        struct Item { int64_t tile; uint32_t ev; };
        std::vector<Item> items;
        size_t reserve = 0;
        for (const auto& e : moves) reserve += (size_t)(e.hi - e.lo) / (size_t)(e.gap + 1) + 1;
        items.reserve(std::min(reserve, (size_t)8'000'000));
        for (uint32_t i = 0; i < (uint32_t)moves.size(); ++i) {
            const auto& e = moves[i];
            if (e.flags & (LevelData::kMoveInactive | LevelData::kMoveEditorOnly)) continue;
            if ((e.flags & 0x1F) == 0) continue;
            const int64_t step = (int64_t)e.gap + 1;
            for (int64_t f = e.lo; f <= e.hi; f += step) items.push_back({f, i});
        }
        std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            const auto& ea = moves[a.ev];
            const auto& eb = moves[b.ev];
            if (ea.trigger != eb.trigger) return ea.trigger < eb.trigger;
            if (ea.order != eb.order) return ea.order < eb.order;
            return a.tile < b.tile;
        });
        for (const Item& it : items) {
            const auto& e = moves[it.ev];
            const double t0 = e.trigger + animShift;
            TileKeyframes& fr = ensureFrame(it.tile, t0, t0 + e.durSec + TileKeyframes::kRangeGrace);
            fr.rangeStart = std::min(fr.rangeStart, t0);
            fr.rangeEnd = std::max(fr.rangeEnd, t0 + e.durSec + TileKeyframes::kRangeGrace);
            // **旋转 = 在砖自身朝向上叠加**：我们的几何已经把砖朝向烘进顶点，而 Re_ADOJAS 的
            // `rotationOffset` 也是"相对其网格"的旋转 —— 两边都不再额外减基准角。
            // （2026-10-21 用 tile 14 的真实帧定案：减基准会让竖直暗列变成横条。）
            // `base` = 该通道的"无动画时"的值（位移 0、缩放/透明度 1）：
            // 第一条事件必须从 base 渐变到目标，不能从 target 开始（那会变成瞬间跳变）。
            const auto add = [&](Channel& ch, float target, float base) {
                const float from = ch.empty() ? base : ch.sample(t0);
                ch.addTween(t0, t0 + e.durSec, from, target, (Ease)e.ease, from);
            };
            if (e.flags & LevelData::kMoveOffX) add(fr.posX, e.offX, 0.0f);
            if (e.flags & LevelData::kMoveOffY) add(fr.posY, e.offY, 0.0f);
            if (e.flags & LevelData::kMoveRot)
                add(fr.rot, e.rotDeg * 3.14159265358979323846f / 180.0f, 0.0f);
            if (e.flags & LevelData::kMoveScale) {
                add(fr.scaleX, e.scaleX, 1.0f); add(fr.scaleY, e.scaleY, 1.0f);
            }
            if (e.flags & LevelData::kMoveOpacity) add(fr.opacity, e.opacity, 1.0f);
        }
    }

    // ---- 2) AnimateTrack：appear/disappear 各类型建帧（**后**建，覆盖 MoveTrack） ----
    // 类型 id 与 Timeline::animId() 一致：1 Extend 2 Assemble 3 Assemble_Far 4 Grow 5 Grow_Spin
    // 7 Fade 8 Drop 9 Rise | 10 Scatter 11 Scatter_Far 12 Retract 13 Shrink 14 Shrink_Spin
    {
        const auto& aT = tl.animAppearType();
        const auto& aS = tl.animAppearStart();
        const auto& aE = tl.animAppearEnd();
        const auto& dT = tl.animDisappearType();
        const auto& dS = tl.animDisappearStart();
        const auto& dE = tl.animDisappearEnd();
        const auto seeded = [](double seed) {
            const double x = std::sin(seed * 12.9898 + seed * 78.233) * 43758.5453;
            return x - std::floor(x);
        };
        const size_t n = aT.size();
        for (size_t i = 0; i < n; ++i) {
            const uint8_t at = aT[i], dt = (i < dT.size()) ? dT[i] : 0;
            if (!at && !dt) continue;
            const int64_t tile = (int64_t)i;
            double rStart = 1e300, rEnd = -1e300;
            if (at) { rStart = std::min(rStart, aS[i] + animShift); rEnd = std::max(rEnd, aE[i] + animShift); }
            if (dt && i < dS.size()) {
                rStart = std::min(rStart, dS[i] + animShift); rEnd = std::max(rEnd, dE[i] + animShift);
            }
            TileKeyframes& fr = ensureFrame(tile, rStart, rEnd + TileKeyframes::kRangeGrace);
            fr.rangeStart = std::min(fr.rangeStart, rStart);
            fr.rangeEnd = std::max(fr.rangeEnd, rEnd + TileKeyframes::kRangeGrace);
            // appear：从"起始状态"渐变到 base（位移 0 / 旋转 0 / 缩放 1 / 透明度 1）
            if (at) {
                const double t0 = aS[i] + animShift, t1 = aE[i] + animShift;
                switch (at) {
                    case 1:   // Extend：只缩放（位置由相邻砖决定，这里不模拟）
                    case 4:   // Grow
                    case 5: { // Grow_Spin
                        fr.scaleX.addTween(t0, t1, 0.0f, 1.0f, at == 4 ? Ease::OutQuad : Ease::OutSine, 0.0f);
                        fr.scaleY.addTween(t0, t1, 0.0f, 1.0f, at == 4 ? Ease::OutQuad : Ease::OutSine, 0.0f);
                        if (at == 5) fr.rot.addTween(t0, t1, -3.14159265f, 0.0f, Ease::OutSine, -3.14159265f);
                        break;
                    }
                    case 2: case 3: {   // Assemble(_Far)
                        const double range = (at == 3) ? 8.0 : 4.0;
                        const double seed = (double)tile * 7919.0;
                        const double dx = seeded(seed) * range * 2 - range;
                        const double dy = seeded(seed + 1) * range * 2 - range;
                        const double dr = (seeded(seed + 2) * 150 - 75) * 3.14159265358979323846 / 180.0;
                        fr.posX.addTween(t0, t1, (float)dx, 0.0f, Ease::OutSine, 0.0f);
                        fr.posY.addTween(t0, t1, (float)dy, 0.0f, Ease::OutSine, 0.0f);
                        fr.rot.addTween(t0, t1, (float)dr, 0.0f, Ease::OutSine, 0.0f);
                        break;
                    }
                    case 7:   // Fade
                        fr.opacity.addTween(t0, t1, 0.0f, 1.0f, Ease::Linear, 0.0f);
                        break;
                    case 8: case 9: {   // Drop / Rise：位移 ±8 → 0 + 前 1/8 时长缩放到位
                        const float dir = (at == 8) ? 8.0f : -8.0f;
                        fr.posY.addTween(t0, t1, dir, 0.0f, Ease::Linear, 0.0f);
                        const double sd = std::min(t1, t0 + (t1 - t0) / 8.0);
                        fr.scaleX.addTween(t0, sd, 0.0f, 1.0f, Ease::OutQuad, 0.0f);
                        fr.scaleY.addTween(t0, sd, 0.0f, 1.0f, Ease::OutQuad, 0.0f);
                        break;
                    }
                    default: break;
                }
            }
            // disappear：从 base 渐变到"消失状态"
            if (dt && i < dS.size()) {
                const double t0 = dS[i] + animShift, t1 = dE[i] + animShift;
                switch (dt) {
                    case 7:   // Fade
                        fr.opacity.addTween(t0, t1, 1.0f, 0.0f, Ease::Linear, 1.0f);
                        break;
                    case 10: case 11: {  // Scatter(_Far)
                        const double range = (dt == 11) ? 8.0 : 4.0;
                        const double seed = (double)tile * 3571.0 + 1000.0;
                        const double dx = seeded(seed) * range * 2 - range;
                        const double dy = seeded(seed + 1) * range * 2 - range;
                        const double dr = (seeded(seed + 2) * 150 - 75) * 3.14159265358979323846 / 180.0;
                        fr.posX.addTween(t0, t1, 0.0f, (float)dx, Ease::OutSine, 0.0f);
                        fr.posY.addTween(t0, t1, 0.0f, (float)dy, Ease::OutSine, 0.0f);
                        fr.rot.addTween(t0, t1, 0.0f, (float)dr, Ease::OutSine, 0.0f);
                        break;
                    }
                    case 12:  // Retract：缩到 0（位移到下一砖在本实现里省略，见 docs/animation.md）
                    case 13:  // Shrink
                        fr.scaleX.addTween(t0, t1, 1.0f, 0.0f, Ease::OutSine, 1.0f);
                        fr.scaleY.addTween(t0, t1, 1.0f, 0.0f, Ease::OutSine, 1.0f);
                        break;
                    case 14:  // Shrink_Spin
                        fr.scaleX.addTween(t0, t1, 1.0f, 0.0f, Ease::OutSine, 1.0f);
                        fr.scaleY.addTween(t0, t1, 1.0f, 0.0f, Ease::OutSine, 1.0f);
                        fr.rot.addTween(t0, t1, 0.0f, -3.14159265f, Ease::OutSine, 0.0f);
                        break;
                    default: break;
                }
            }
        }
    }

    // 建帧完：缓存每块砖"区间之前/之后是否完全不可见"
    for (TileKeyframes& f : m_frames) {
        const auto visible = [](const TileKeyframes& g, double t) {
            const float op = g.opacity.empty() ? 1.0f : g.opacity.sample(t);
            const float sx = g.scaleX.empty() ? 1.0f : g.scaleX.sample(t);
            const float sy = g.scaleY.empty() ? 1.0f : g.scaleY.sample(t);
            return !(op <= 0.002f || (std::fabs(sx) <= 0.002f && std::fabs(sy) <= 0.002f));
        };
        f.hiddenBefore = !visible(f, f.rangeStart - 1.0);
        f.hiddenAfter  = !visible(f, f.rangeEnd + 1.0);
    }

    // 索引必须按砖号升序（采样用二分）
    std::sort(m_index.begin(), m_index.end(), [](const Entry& a, const Entry& b) { return a.tile < b.tile; });
    // 重新映射 idx（ensureFrame 插入时用的是当时的序号，排序后要重排）
    // 简化：重建 m_frames 顺序与 m_index 一致。
    if (m_index.size() > 1) {
        std::vector<TileKeyframes> reordered(m_frames.size());
        for (size_t i = 0; i < m_index.size(); ++i) reordered[i] = std::move(m_frames[m_index[i].idx]);
        for (size_t i = 0; i < m_index.size(); ++i) m_index[i].idx = (uint32_t)i;
        m_frames = std::move(reordered);
    }
    if (keyframeCount() > kMaxKeyframes) {
        m_truncated = true;
        LOG_W("TileAnimation: 关键帧总数 %zu 超过上限 %zu", keyframeCount(), kMaxKeyframes);
    }
    LOG_I("TileAnimation: %zu 砖有动画，%zu 条关键帧（%.1f KB，截断=%d）",
          m_index.size(), keyframeCount(),
          (double)(keyframeCount() * sizeof(Channel::Key)) / 1024.0, (int)m_truncated);
}

bool TileAnimation::hiddenAt(int64_t tile, double t) const {
    const uint32_t idx = frameIndexFor(tile);
    if (idx == UINT32_MAX) return false;
    const TileKeyframes& f = m_frames[idx];
    if (t < f.rangeStart) return f.hiddenBefore;
    if (t >= f.rangeEnd) return f.hiddenAfter;
    const float op = f.opacity.empty() ? 1.0f : f.opacity.sample(t);
    const float sx = f.scaleX.empty() ? 1.0f : f.scaleX.sample(t);
    const float sy = f.scaleY.empty() ? 1.0f : f.scaleY.sample(t);
    return op <= 0.002f || (std::fabs(sx) <= 0.002f && std::fabs(sy) <= 0.002f);
}

bool TileAnimation::sample(int64_t tile, double t, TileAnimSample& out) const {
    const uint32_t idx = frameIndexFor(tile);
    if (idx == UINT32_MAX) return false;
    const TileKeyframes& f = m_frames[idx];
    out = TileAnimSample{};
    out.posX = f.posX.empty() ? 0.0f : f.posX.sample(t);
    out.posY = f.posY.empty() ? 0.0f : f.posY.sample(t);
    out.rot = f.rot.empty() ? 0.0f : f.rot.sample(t);
    out.scaleX = f.scaleX.empty() ? 1.0f : f.scaleX.sample(t);
    out.scaleY = f.scaleY.empty() ? 1.0f : f.scaleY.sample(t);
    out.opacity = f.opacity.empty() ? 1.0f : f.opacity.sample(t);
    out.animated = true;
    return true;
}

void TileAnimation::activeAt(double t, std::vector<int64_t>& out) const {
    out.clear();
    for (size_t i = 0; i < m_frames.size(); ++i)
        if (m_frames[i].activeAt(t)) out.push_back(m_index[i].tile);
}

}  // namespace adofai
