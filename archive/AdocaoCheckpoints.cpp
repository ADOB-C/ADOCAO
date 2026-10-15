#include "archive/AdocaoCheckpoints.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"

namespace adofai {
namespace adocao {
namespace {

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putF64(std::vector<uint8_t>& b, double d) {
    uint64_t x = 0;
    std::memcpy(&x, &d, 8);          // 位模式原样：checkpoint 必须逐位可逆（§8.8 的 f64 要求）
    putU64(b, x);
}
uint32_t rdU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
double rdF64(const uint8_t* p) {
    const uint64_t x = rdU64(p);
    double d = 0.0;
    std::memcpy(&d, &x, 8);
    return d;
}
inline bool have(size_t n, size_t off, size_t k) { return off <= n && k <= n - off; }

constexpr double kPi = 3.14159265358979323846;

// 第 i 层的"步进方向"（度）——与 calculateTilePositions 用的是同一个量：tiles[i].direction
// 就是那边的 floats[i]（哨兵 999 已经在前一层 +180 处理过）。
inline double stepAngleDeg(const LevelData& level, size_t i) {
    return (double)level.tiles[i].direction;
}

}  // namespace

bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        std::vector<FloorCheckpoint>& out, std::string& err) {
    out.clear();
    err.clear();
    if (stride == 0) { err = "checkpoint stride 不能为 0"; return false; }

    const size_t nData = level.angleData.size();     // 真层数；tiles 还会多一块 extra 砖
    if (level.tiles.size() < nData) {
        err = "LevelData 还没有算位置（tiles < angleData）——checkpoint 必须在 finishLoad 之后算";
        return false;
    }

    // 与 applyPositionTrackOffsets 同一套语义：非 justThisTile 的偏移**累进**（本层及之后都吃到），
    // justThisTile 是**点加**（只作用于本层）。这里按层号升序推进，所以只需要按 floor 排序的事件。
    std::vector<std::pair<int64_t, LevelData::TilePositionOffset>> offs(
        level.tilePositionOffsets.begin(), level.tilePositionOffsets.end());
    std::sort(offs.begin(), offs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t oi = 0;

    // 时间/BPM 只有 Timeline 才是权威实现（SetSpeed 继承 + duration 前缀和）——**不在这里重抄一份**
    // 状态机（重抄必然与生产路径漂移）。代价：要在 pack 期建一次 Timeline（约 32 B/层）。
    std::vector<double> times;
    std::vector<float> bpms;
    {
        Timeline tl;
        tl.build(level, /*exportOnly=*/true);
        times = tl.tileStartTimes();
        bpms = tl.tileBPMs();
    }

    // Timeline Phase 1 的状态（与 `precalculateTiming` 逐字同序：**先处理本层事件、再取状态**）：
    //   * isCW / currentBPM 在"处理完 floor < i0 的事件"之后的值进快照；
    //   * angleDir 是"进第 i0 层的方向"，由 angleData[i0-1] 按 ADOFAI-JS 的 999 规则推出来。
    // 于是窗口的循环必须**自己再处理一遍 floor == i0 的事件**（§8.8 不变量 3），位置那条也同理。
    std::vector<std::pair<int64_t, const LevelData::FastAction*>> acts;
    acts.reserve(level.actions.size());
    for (const LevelData::FastAction& a : level.actions)
        if (a.floor >= 0) acts.push_back({(int64_t)a.floor, &a});
    std::stable_sort(acts.begin(), acts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t ai = 0;
    // ⚠️ 类型必须与 `Timeline::precalculateTiming` **完全一致**：那边 currentBPM/angleDir 是
    // **float**（`currentBPM *= a.val1` 每步都舍入回 float）。这里用 double 累加会在多次
    // SetSpeed 之后与生产路径差出 ULP —— 而"从快照重算 = 全谱一次算"要求逐位相同。
    bool isCW = true;
    float currentBPM = level.settings.bpm;
    float angleDir = 180.0f;

    double baseX = 0.0, baseY = 0.0;    // angleData 前缀和（**不含** PositionTrack 偏移）
    double cumX = 0.0, cumY = 0.0;     // PositionTrack 继承量（floor < i0 的全部事件处理完）
    double lastReal = 0.0;             // 最后一个真 angleData 值
    bool haveLast = false;
    bool lastWasSentinel = false;
    size_t sentinelRun = 0;            // 紧跟当前位置之前连续 999 的个数

    const size_t nCk = (nData + stride) / stride;    // i0 = 0, stride, 2*stride, ... < nData
    out.reserve(nCk ? nCk : 1);

    for (size_t i = 0; i < nData; ++i) {
        if (i % stride == 0) {
            FloorCheckpoint ck;
            ck.floor = (uint64_t)i;
            ck.baseX = baseX; ck.baseY = baseY;
            ck.cumX = cumX;   ck.cumY = cumY;
            ck.flags = kCkBase | kCkCum;
            ck.cw = isCW ? 1 : 0;
            ck.flags |= kCkCw;
            ck.bpm = (double)currentBPM;
            ck.flags |= kCkBpm;
            ck.angleDirDeg = (double)angleDir;      // 存的就是那个 float 值（别用 double 中间量）
            ck.flags |= kCkAngleDir;
            if (haveLast) { ck.lastAngleDeg = lastReal; ck.flags |= kCkLastAngle; }
            if (lastWasSentinel) ck.flags |= kCkLastSentinel;
            ck.sentinelRun = (uint32_t)sentinelRun;
            ck.flags |= kCkSentinelRun;
            if (i < times.size()) { ck.timeSec = times[i]; ck.flags |= kCkTime; }
            out.push_back(ck);
        }

        // 事件：floor < i0 的部分在快照里；i0 那一层的事件在**窗口重算**里处理（§8.8 不变量 3），
        // 所以这里推进事件游标只用于维护 cum 的"下一个快照值"。
        while (oi < offs.size() && offs[oi].first == (int64_t)i) {
            const LevelData::TilePositionOffset& o = offs[oi].second;
            if (!o.justThisTile) { cumX += o.offsetX; cumY += o.offsetY; }
            ++oi;   // justThisTile 不进 cum（点加，不跨层）
        }
        // Timeline Phase 1 的同序递推（Twirl / SetSpeed；Pause 是逐层的，不进快照）
        while (ai < acts.size() && acts[ai].first == (int64_t)i) {
            const LevelData::FastAction& a = *acts[ai].second;
            switch (a.type) {
            case LevelData::FastAction::Twirl: isCW = !isCW; break;
            case LevelData::FastAction::SetSpeed:
                if (a.flag) currentBPM *= a.val1; else currentBPM = a.val1;
                break;
            default: break;
            }
            ++ai;
        }
        // 交叉自检（放在这里而不是快照处）：Timeline 的 `m_tileBPM[i]` 是**处理完第 i 层事件之后**
        // 的值，而快照存的是"处理之前"（窗口要自己再处理一遍，§8.8 不变量 3）。两者都要对得上，
        // 所以这里比的是"本层事件处理完之后"的 currentBPM —— 它能抓住任何实现漂移（2026-10-15
        // 就是这条先抓出了 SetSpeed 的 float/double 类型不一致）。
        if (i < bpms.size() && bpms[i] != currentBPM) {
            err = "checkpoint 的 bpm 与 Timeline 不一致（实现漂移）@floor " + std::to_string(i);
            return false;
        }

        const double a = level.angleData[i];
        if (a == 999.0) { lastWasSentinel = true; ++sentinelRun; }
        else { lastReal = a; haveLast = true; lastWasSentinel = false; sentinelRun = 0; }
        // angleDir = "进下一层的方向"（ADOFAI-JS 语义，与 Phase 1 的收尾更新逐字同序）
        if (a == 999.0) {
            int minus = 1;
            while ((int64_t)i - minus >= 0 && level.angleData[i - minus] == 999.0) ++minus;
            const double realAngle = ((int64_t)i - minus >= 0) ? level.angleData[i - minus] : 0.0;
            angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
        } else {
            angleDir = (float)std::fmod(a + 180.0, 360.0);
        }
        if (angleDir < 0) angleDir += 360.0f;

        const double rad = stepAngleDeg(level, i) * kPi / 180.0;
        baseX += std::cos(rad);
        baseY += std::sin(rad);
    }
    return true;
}

bool encodeCheckpoints(const std::vector<FloorCheckpoint>& ck, uint32_t stride,
                       std::vector<uint8_t>& out) {
    out.clear();
    putU32(out, stride);
    putU32(out, 0);
    putU64(out, ck.size());
    out.reserve(out.size() + ck.size() * kCheckpointBytes);
    for (const FloorCheckpoint& c : ck) {
        putU64(out, c.floor);
        putF64(out, c.baseX); putF64(out, c.baseY);
        putF64(out, c.cumX);  putF64(out, c.cumY);
        putF64(out, c.timeSec);
        putF64(out, c.bpm);
        putF64(out, c.radiusScale);
        putF64(out, c.lastAngleDeg);
        putF64(out, c.angleDirDeg);
        putU32(out, c.flags);
        putU32(out, c.sentinelRun);
        out.push_back(c.cw);
        for (int i = 0; i < 7; ++i) out.push_back(0);
    }
    return true;
}

bool decodeCheckpoints(const uint8_t* p, size_t n, uint32_t& stride,
                       std::vector<FloorCheckpoint>& out) {
    out.clear();
    if (!p || !have(n, 0, 16)) return false;
    stride = rdU32(p);
    if (stride == 0) return false;
    const uint64_t count = rdU64(p + 8);
    if (!have(n, 16, (size_t)count * kCheckpointBytes)) return false;
    if (count > (uint64_t)1 << 40) return false;
    out.reserve((size_t)count);
    size_t off = 16;
    for (uint64_t i = 0; i < count; ++i, off += kCheckpointBytes) {
        const uint8_t* q = p + off;
        FloorCheckpoint c;
        c.floor = rdU64(q);
        c.baseX = rdF64(q + 8);  c.baseY = rdF64(q + 16);
        c.cumX = rdF64(q + 24);  c.cumY = rdF64(q + 32);
        c.timeSec = rdF64(q + 40);
        c.bpm = rdF64(q + 48);
        c.radiusScale = rdF64(q + 56);
        c.lastAngleDeg = rdF64(q + 64);
        c.angleDirDeg = rdF64(q + 72);
        c.flags = rdU32(q + 80);
        c.sentinelRun = rdU32(q + 84);
        c.cw = q[88];
        if (c.cw > 1) return false;
        for (int i = 89; i < 96; ++i) if (q[i]) return false;   // pad 必须为 0（结构自洽）
        // 结构性自洽：层号必须是 stride 的倍数且严格递增（坏数据不许被当成好数据）
        if (i == 0 && c.floor != 0) return false;
        if (i > 0 && c.floor <= out.back().floor) return false;
        if (c.floor % stride != 0) return false;
        out.push_back(c);
    }
    return true;
}

bool encodeActionIndex(const std::vector<ActionIndexEntry>& entries, uint32_t stride,
                       std::vector<uint8_t>& out) {
    out.clear();
    putU32(out, stride);
    putU32(out, 1u);                      // flags bit0 = 32 B 条目（含稀疏列 used 起点）
    putU64(out, entries.size());
    out.reserve(out.size() + entries.size() * 32);
    for (const ActionIndexEntry& e : entries) {
        putU64(out, e.firstAction);
        putU64(out, e.usedStrId);
        putU64(out, e.usedVal1);
        putU64(out, e.usedVal2);
    }
    return true;
}

bool decodeActionIndex(const uint8_t* p, size_t n, uint32_t& stride,
                       std::vector<ActionIndexEntry>& out) {
    out.clear();
    if (!p || !have(n, 0, 16)) return false;
    stride = rdU32(p);
    if (stride == 0) return false;
    const uint32_t flags = rdU32(p + 4);
    if (flags & ~1u) return false;         // 未知位 → 明确失败（不猜）
    const uint64_t count = rdU64(p + 8);
    if (count > (uint64_t)1 << 40) return false;
    const size_t entry = (flags & 1u) ? 32u : 8u;
    if (!have(n, 16, (size_t)count * entry)) return false;
    out.reserve((size_t)count);
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* q = p + 16 + (size_t)i * entry;
        ActionIndexEntry e;
        e.firstAction = rdU64(q);
        if (entry == 32) {
            e.usedStrId = rdU64(q + 8);
            e.usedVal1 = rdU64(q + 16);
            e.usedVal2 = rdU64(q + 24);
        }
        if (i > 0) {
            // 必须非递减（坏数据不许蒙混）；稀疏列的 used 起点同理
            const ActionIndexEntry& b = out.back();
            if (e.firstAction < b.firstAction) return false;
            if (e.usedStrId < b.usedStrId || e.usedVal1 < b.usedVal1 || e.usedVal2 < b.usedVal2)
                return false;
        }
        out.push_back(e);
    }
    return true;
}

size_t verifyCheckpointResume(const LevelData& level, const FloorCheckpoint& ck,
                              size_t i0, size_t i1, size_t* firstBad) {
    const size_t nData = level.angleData.size();
    if (firstBad) *firstBad = 0;
    if (i0 >= level.tiles.size() || i0 != (size_t)ck.floor) return (size_t)-1;
    if (i1 > nData) i1 = nData;                    // extra 砖不走"前缀和步进"那条公式
    if (i1 <= i0) return 0;

    // 事件游标：只用于 [i0, i1) 这一段（**包含 floor == i0**，§8.8 不变量 3）
    std::vector<std::pair<int64_t, LevelData::TilePositionOffset>> offs(
        level.tilePositionOffsets.begin(), level.tilePositionOffsets.end());
    std::sort(offs.begin(), offs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t oi = 0;
    while (oi < offs.size() && offs[oi].first < (int64_t)i0) ++oi;

    double baseX = ck.baseX, baseY = ck.baseY;      // **从快照重新生成**，绝不原地累加（不变量 2）
    double cumX = ck.cumX, cumY = ck.cumY;
    size_t bad = 0;
    for (size_t i = i0; i < i1; ++i) {
        double px = 0.0, py = 0.0;                  // 本层的点加（justThisTile，同层多个事件累加）
        while (oi < offs.size() && offs[oi].first == (int64_t)i) {
            const LevelData::TilePositionOffset& o = offs[oi].second;
            if (o.justThisTile) { px += o.offsetX; py += o.offsetY; }
            else { cumX += o.offsetX; cumY += o.offsetY; }
            ++oi;
        }
        const double wx = baseX + cumX + px;
        const double wy = baseY + cumY + py;
        if (std::memcmp(&wx, &level.tiles[i].position[0], 8) != 0 ||
            std::memcmp(&wy, &level.tiles[i].position[1], 8) != 0) {
            if (bad == 0 && firstBad) *firstBad = i;
            ++bad;
        }
        const double rad = stepAngleDeg(level, i) * kPi / 180.0;
        baseX += std::cos(rad);
        baseY += std::sin(rad);
    }
    return bad;
}

}  // namespace adocao
}  // namespace adofai
