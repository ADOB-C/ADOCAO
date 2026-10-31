#include "core/timeline/TileAnimation.hpp"
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"

namespace adofai {
namespace {
// 关键帧总量上限：超过就停止建帧并告警（超大谱保护；Re_ADOJAS 用 GLOBAL_ANIM_TILE_LIMIT）。
// **2026-10-31 用户："你的各种东西都太保守了"** —— 这两个数是自创的，参考实现只限制
// "没有任何 AT 事件 + 超过 100000 砖的纯全局动画"。原来的 4M/500k 在 2.64e6 那张谱上
// （997,664 砖）会**截掉后半张**：全局 Fade 只作用在前 50 万砖，后面的砖一直可见。
// 现在按"每条关键帧 ~16 B"给预算：20M 条 ≈ 320 MB，够一张百万砖、每砖 20 帧的谱。
// 真要 2^31 层的谱本来也不能逐砖建帧（那有 Re_ADOJAS 那条 100k 全局动画闸兜着）。
constexpr size_t kMaxKeyframes = 20'000'000;
constexpr size_t kMaxAnimatedTiles = 4'000'000;
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
    std::vector<std::tuple<size_t, uint8_t, int64_t>> appearPins;
    const auto seeded = [](double seed) {
        const double x = std::sin(seed * 12.9898 + seed * 78.233) * 43758.5453;
        return x - std::floor(x);
    };
    {
        const auto& aT = tl.animAppearType();
        const auto& aS = tl.animAppearStart();
        const auto& aE = tl.animAppearEnd();
        const auto& dT = tl.animDisappearType();
        const auto& dS = tl.animDisappearStart();
        const auto& dE = tl.animDisappearEnd();
        const size_t n = aT.size();
        // appear 动画要求"淡入之前这块砖根本不存在"，而下面那段通用的 t=0 基准帧
        // （见「补齐 t=0 的基准帧」）会把透明度补成 1 —— 两者冲突时以 appear 自身为准：
        // 参考实现每个 case 的第一句都是 `addKeyframe(entity, <通道>, 0, <初始状态>, null)`
        // （Fade → opacity 0、Grow → scale 0、Assemble → 位移随机量、Drop/Rise → ±8 与 scale 0），
        // 而且**不是** base 值。记下每个帧的 appear 类型，等基准帧补完再覆盖回初始状态。
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
            if (at) appearPins.emplace_back((size_t)(&fr - m_frames.data()), at, tile);
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

    // **补齐 t=0 的基准帧**（Re_ADOJAS `TimelineManager` 对每条通道都先
    // `addKeyframe(tile:i, 'scaleX', 0, base, null)` 再挂事件）。
    // 缺了它会有一个很隐蔽的错：若某条通道**只有一条未来的关键帧**（例如 disappear 的
    // Shrink 在 t=96 把 scale 打到 0），`Channel::sample(t<96)` 会返回那条帧的值 0 ——
    // 于是整块砖从**第一帧起就"scale=0"、被当成不可见**（MM2 开头整条轨道消失就是这么来的：
    // 6636 块砖都有 `MoveTrack`/`AnimateTrack` 帧，一半以上带未来的 scale=0）。
    // 基准值：位移/旋转 = 0，缩放/透明度 = 1（与 Re_ADOJAS 的 bases 一致）。
    for (TileKeyframes& f : m_frames) {
        auto seed = [](Channel& c, float base) {
            if (!c.empty() && c.keys().front().time > 0.0) c.addKeyframe(0.0, base);
        };
        seed(f.posX, 0.0f); seed(f.posY, 0.0f); seed(f.rot, 0.0f);
        seed(f.scaleX, 1.0f); seed(f.scaleY, 1.0f); seed(f.opacity, 1.0f);
    }

    // appear 动画的**起始状态**必须压过上面补的 base 基准帧 —— 参考实现每个 case 的第一句
    // 就是 `addKeyframe(entity, <通道>, 0, <初始状态>, null)`，它写的不是 base，而是"还没出现"。
    // 漏掉这一步的症状：`trackAnimation: Fade` + `beatsAhead: 8` 的谱面上，行星前方 8 拍
    // 以外的砖**全都可见**（0.000:1.000 那条基准帧把淡入窗口前的透明度钉成了 1），
    // 直到各自的 appearStart 才突然消失再淡回来。2026-10-31 实测 2.64e6 那张：t=0.5 时
    // tile 3~44 全是 alpha=1.0，而参考实现只该看见 ~9 块。
    for (const auto& p : appearPins) {
        TileKeyframes& f = m_frames[std::get<0>(p)];
        switch (std::get<1>(p)) {
            case 7:   // Fade
                f.opacity.addKeyframe(0.0, 0.0f);
                break;
            case 1:   // Extend
            case 4:   // Grow
            case 5:   // Grow_Spin
                f.scaleX.addKeyframe(0.0, 0.0f);
                f.scaleY.addKeyframe(0.0, 0.0f);
                break;
            case 2: case 3: {   // Assemble(_Far)：t=0 的随机位移与旋转
                const double range = (std::get<1>(p) == 3) ? 8.0 : 4.0;
                const double seed = (double)std::get<2>(p) * 7919.0;
                const double dx = seeded(seed) * range * 2 - range;
                const double dy = seeded(seed + 1) * range * 2 - range;
                const double dr = (seeded(seed + 2) * 150 - 75) * 3.14159265358979323846 / 180.0;
                f.posX.addKeyframe(0.0, (float)dx);
                f.posY.addKeyframe(0.0, (float)dy);
                f.rot.addKeyframe(0.0, (float)dr);
                break;
            }
            case 8: case 9:   // Drop / Rise：先藏在上下 8 格、缩放 0
                f.posY.addKeyframe(0.0, (std::get<1>(p) == 8) ? 8.0f : -8.0f);
                f.scaleX.addKeyframe(0.0, 0.0f);
                f.scaleY.addKeyframe(0.0, 0.0f);
                break;
            default: break;
        }
    }

    // 建帧完：缓存每块砖"区间之前/之后是否完全不可见"
    for (TileKeyframes& f : m_frames) {
        const auto visible = [](const TileKeyframes& g, double t) {
            const float op = g.opacity.sampleOr(t, 1.0f);
            const float sx = g.scaleX.sampleOr(t, 1.0f);
            const float sy = g.scaleY.sampleOr(t, 1.0f);
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
    // 诊断：`ADOCAO_ANIM_DUMP_TILE=<n>` 打印该砖各通道的关键帧（排查"砖为什么看不见"）
    if (const char* dt = std::getenv("ADOCAO_ANIM_DUMP_TILE")) {
        const int64_t want = std::atoll(dt);
        const uint32_t wi = frameIndexFor(want);
        if (wi != UINT32_MAX) {
            const TileKeyframes& f = m_frames[wi];
            auto dump = [&](const char* name, const Channel& c) {
                std::string line;
                char buf[64];
                for (const auto& k : c.keys()) {
                    std::snprintf(buf, sizeof(buf), "%.3f:%.3f ", k.time, k.value);
                    line += buf;
                }
                LOG_I("anim-dump tile=%lld %s keys=%zu [%s]", (long long)want, name, c.size(),
                      line.c_str());
            };
            dump("opacity", f.opacity);
            dump("scaleX", f.scaleX);
            dump("scaleY", f.scaleY);
            dump("posX", f.posX);
            dump("posY", f.posY);
            LOG_I("anim-dump tile=%lld range=[%.3f, %.3f] hiddenBefore=%d hiddenAfter=%d",
                  (long long)want, f.rangeStart, f.rangeEnd, (int)f.hiddenBefore, (int)f.hiddenAfter);
            if (const char* tt = std::getenv("ADOCAO_ANIM_DUMP_TIME")) {
                const double tq = std::atof(tt);
                TileAnimSample sm{};
                const bool has = sample(want, tq, sm);
                LOG_I("anim-dump tile=%lld @t=%.3f has=%d op=%.3f sx=%.3f sy=%.3f px=%.3f py=%.3f hidden=%d",
                      (long long)want, tq, (int)has, sm.opacity, sm.scaleX, sm.scaleY, sm.posX, sm.posY,
                      (int)hiddenAt(want, tq));
            }
        } else {
            LOG_I("anim-dump tile=%lld 没有动画帧", (long long)want);
        }
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
    // 倒计时/未开始（t < 0）：参考实现在**第一条关键帧之前**回落到 base（可视）。
    // 我们的关键帧都在 t >= 0，所以 t < 0 一律"还没进入动画"，按 base 处理 ——
    // 否则开场/暂停时整条轨道是黑的（2026-10-31 用户："暂停的时候什么都没有了"）。
    if (t < 0.0) return false;
    if (t < f.rangeStart) return f.hiddenBefore;
    if (t >= f.rangeEnd) return f.hiddenAfter;
    const float op = f.opacity.sampleOr(t, 1.0f);
    const float sx = f.scaleX.sampleOr(t, 1.0f);
    const float sy = f.scaleY.sampleOr(t, 1.0f);
    return op <= 0.002f || (std::fabs(sx) <= 0.002f && std::fabs(sy) <= 0.002f);
}

bool TileAnimation::sample(int64_t tile, double t, TileAnimSample& out) const {
    const uint32_t idx = frameIndexFor(tile);
    if (idx == UINT32_MAX) return false;
    const TileKeyframes& f = m_frames[idx];
    out = TileAnimSample{};
    // **首帧之前一律回落 base**（参考实现 `sample()` 在首帧之前返回 undefined，调用方用
    // base）。差别只在倒计时（t < 0）：用 `sample()` 会取到 appear 首帧的 0，
    // 于是"暂停/开场整条轨道不见"—— 2026-10-31 用户实测就是这句"暂停的时候什么都没有了"。
    out.posX = f.posX.sampleOr(t, 0.0f);
    out.posY = f.posY.sampleOr(t, 0.0f);
    out.rot = f.rot.sampleOr(t, 0.0f);
    out.scaleX = f.scaleX.sampleOr(t, 1.0f);
    out.scaleY = f.scaleY.sampleOr(t, 1.0f);
    out.opacity = f.opacity.sampleOr(t, 1.0f);
    out.animated = true;
    return true;
}

void TileAnimation::activeAt(double t, std::vector<int64_t>& out) const {
    out.clear();
    for (size_t i = 0; i < m_frames.size(); ++i)
        if (m_frames[i].activeAt(t)) out.push_back(m_index[i].tile);
}

}  // namespace adofai
