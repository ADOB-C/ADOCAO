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

    double baseX = 0.0, baseY = 0.0;    // angleData 前缀和（**不含** PositionTrack 偏移）
    double cumX = 0.0, cumY = 0.0;     // PositionTrack 继承量（floor < i0 的全部事件处理完）
    double lastReal = 0.0;             // 最后一个真 angleData 值
    bool haveLast = false;
    bool lastWasSentinel = false;

    const size_t nCk = (nData + stride) / stride;    // i0 = 0, stride, 2*stride, ... < nData
    out.reserve(nCk ? nCk : 1);

    for (size_t i = 0; i < nData; ++i) {
        if (i % stride == 0) {
            FloorCheckpoint ck;
            ck.floor = (uint64_t)i;
            ck.baseX = baseX; ck.baseY = baseY;
            ck.cumX = cumX;   ck.cumY = cumY;
            ck.flags = kCkBase | kCkCum;
            if (haveLast) { ck.lastAngleDeg = lastReal; ck.flags |= kCkLastAngle; }
            if (lastWasSentinel) ck.flags |= kCkLastSentinel;
            if (i < times.size()) { ck.timeSec = times[i]; ck.flags |= kCkTime; }
            if (i < bpms.size()) { ck.bpm = (double)bpms[i]; ck.flags |= kCkBpm; }
            out.push_back(ck);
        }

        // 事件：floor < i0 的部分在快照里；i0 那一层的事件在**窗口重算**里处理（§8.8 不变量 3），
        // 所以这里推进事件游标只用于维护 cum 的"下一个快照值"。
        while (oi < offs.size() && offs[oi].first == (int64_t)i) {
            const LevelData::TilePositionOffset& o = offs[oi].second;
            if (!o.justThisTile) { cumX += o.offsetX; cumY += o.offsetY; }
            ++oi;   // justThisTile 不进 cum（点加，不跨层）
        }

        const double a = level.angleData[i];
        if (a == 999.0) lastWasSentinel = true;
        else { lastReal = a; haveLast = true; lastWasSentinel = false; }

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
        putU32(out, c.flags);
        putU32(out, c.reserved);
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
        c.flags = rdU32(q + 72);
        c.reserved = rdU32(q + 76);
        // 结构性自洽：层号必须是 stride 的倍数且严格递增（坏数据不许被当成好数据）
        if (i == 0 && c.floor != 0) return false;
        if (i > 0 && c.floor <= out.back().floor) return false;
        if (c.floor % stride != 0) return false;
        if ((c.reserved & 0xFFFFFFFFu) != 0) return false;
        out.push_back(c);
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
