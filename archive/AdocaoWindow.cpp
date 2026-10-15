#include "archive/AdocaoWindow.hpp"

#include <algorithm>
#include <cmath>

namespace adofai {
namespace adocao {
namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr float  kPiF = 3.14159265f;

// `Timeline::totalAngleAt` + `durationAt` 的**生产路径**公式（逐字照抄，包括浮点类型与
// 常量 3.14159265f；`precalcTileRange` 是另一条 A/B 参考实现，别照抄那一份）。
// 第 i 层的 duration 由 tileIsCW / tileBPM / preAngleDir / preExtraRot 决定。
float rotationDuration(double rawAngleData, float preAngleDir, bool isCW, float currentBPM,
                       float extraRotation) {
    double relAngle;
    if (rawAngleData == 999.0) {
        relAngle = 0.0;
    } else {
        double delta = std::fmod((double)preAngleDir - rawAngleData, 360.0);
        if (delta < 0) delta += 360.0;
        if (!isCW) {
            relAngle = 360.0 - delta;
            if (relAngle >= 360.0) relAngle -= 360.0;
        } else {
            relAngle = delta;
        }
        if (delta < 0.0001) relAngle = 360.0;
    }
    float totalAngle = (float)relAngle * kPiF / 180.0f;
    if (isCW) totalAngle = -totalAngle;
    if (isCW) totalAngle -= extraRotation * 2.0f * kPiF;
    else      totalAngle += extraRotation * 2.0f * kPiF;
    const float rotationAmount = std::abs(totalAngle) / (2.0f * kPiF);
    return rotationAmount * 2.0f * (60.0f / currentBPM);
}

}  // namespace

float checkpointPrevDirection(const FloorCheckpoint& ck) {
    // `calculateTilePositions`：999 → floats[i] = floats[i-1] + 180（沿用上一个"有效"方向）。
    // 所以 i0-1 的有效方向 = 最后一个真角度 + 180 × 紧跟其后的 999 个数。
    return (float)(ck.lastAngleDeg + 180.0 * (double)ck.sentinelRun);
}

bool computeFloorWindow(const LevelData::Settings& settings, const FloorWindowInput& in,
                        FloorWindow& out, std::string& err) {
    out = FloorWindow{};
    err.clear();
    if (!in.angles || !in.events) { err = "computeFloorWindow 缺输入（angles/events）"; return false; }
    const FloorCheckpoint& ck = in.checkpoint;
    const uint32_t need = kCkBase | kCkCum | kCkBpm | kCkTime | kCkCw | kCkAngleDir;
    if ((ck.flags & need) != need) {
        err = "checkpoint 缺分量（窗口续算需要的 base/cum/bpm/time/cw/angleDir 不齐）";
        return false;
    }
    const size_t n = in.angles->size();
    if (n == 0) return true;
    const uint64_t i0 = ck.floor;
    if (n > kCheckpointStride) { err = "一个窗口不能超过一个 stride（分块边界就是窗口边界）"; return false; }

    out.first = i0;
    out.tiles.assign(n, LevelData::Tile{});
    out.tileBPMs.assign(n, 0.0f);
    out.tileStartTimes.assign(n, 0.0);

    // 事件按 floor 稳定排序（同层保持文件顺序 —— 与 processActions/Timeline 的迭代顺序一致）。
    // PositionTrack 是"同层最后一个胜出"（`tilePositionOffsets[floor] = {...}` 覆盖），
    // 而 Twirl/SetSpeed/Pause 是逐个生效 —— 所以下面按类型分开处理，别混成"同层累加"。
    std::vector<const LevelData::FastAction*> ev;
    ev.reserve(in.events->size());
    for (const LevelData::FastAction& a : *in.events) {
        if (a.floor < 0) continue;
        const uint64_t f = (uint64_t)a.floor;
        if (f >= i0 && f < i0 + n) ev.push_back(&a);
    }
    std::stable_sort(ev.begin(), ev.end(),
                     [](const LevelData::FastAction* a, const LevelData::FastAction* b) {
                         return a->floor < b->floor;
                     });

    // ---- 起点状态（全部来自快照；位置**从快照重新生成**，绝不原地累加）----
    double baseX = ck.baseX, baseY = ck.baseY;
    double cumX = ck.cumX, cumY = ck.cumY;
    bool isCW = ck.cw != 0;
    float currentBPM = (float)ck.bpm;
    float angleDir = (float)ck.angleDirDeg;      // "进第 i0 层"的方向（ADOFAI-JS 语义）
    double totalTime = ck.timeSec;

    // 上一层（i0-1）的**有效**方向：999 连跑时由 lastAngleDeg + sentinelRun 推回来
    float prevDir = (ck.flags & kCkLastAngle) ? checkpointPrevDirection(ck) : 0.0f;

    size_t ei = 0;
    for (size_t k = 0; k < n; ++k) {
        const uint64_t i = i0 + k;
        const double raw = (*in.angles)[k];

        // ---- ① 本层事件（floor == i）：位置 / 转向 / BPM / Pause ----
        double px = 0.0, py = 0.0;               // 点加（justThisTile，只作用于本层）
        float extraRotation = 0.0f;              // Pause：val1/2
        // Twirl/SetSpeed/Pause 逐个生效；**PositionTrack 同层最后一个胜出**
        // （processActions 里是 `tilePositionOffsets[floor] = {…}`，同层会互相覆盖）。
        const LevelData::FastAction* lastPos = nullptr;
        while (ei < ev.size() && (uint64_t)ev[ei]->floor == i) {
            const LevelData::FastAction& a = *ev[ei];
            switch (a.type) {
            case LevelData::FastAction::Twirl:   isCW = !isCW; break;
            case LevelData::FastAction::SetSpeed:
                if (a.flag) currentBPM *= a.val1; else currentBPM = a.val1;
                break;
            case LevelData::FastAction::Pause:   extraRotation += a.val1 / 2.0f; break;
            case LevelData::FastAction::PositionTrack: lastPos = &a; break;
            default: break;
            }
            ++ei;
        }
        if (lastPos) {
            if (lastPos->flag) { px += lastPos->val1; py += lastPos->val2; }   // 点加：只本层
            else { cumX += lastPos->val1; cumY += lastPos->val2; }            // 继承：本层及之后
        }

        // ---- ② 有效方向（999 → 上一层 + 180）----
        const float dir = (raw == 999.0) ? (prevDir + 180.0f) : (float)raw;

        // ---- ③ 位置：base（从快照续走）+ cum + 点加 ----
        // 结合顺序必须与 `applyPositionTrackOffsets` 的 `position += cumX + px` 完全一致。
        out.tiles[k].position = {baseX + (cumX + px), baseY + (cumY + py)};
        out.tiles[k].direction = dir;

        // ---- ④ BPM / 时间（Timeline Phase 1 的顺序：本层事件处理完之后再取状态）----
        out.tileBPMs[k] = currentBPM;
        out.tileStartTimes[k] = totalTime;
        totalTime += (double)rotationDuration(raw, angleDir, isCW, currentBPM, extraRotation);

        // ---- ⑤ 推进：angleDir（下一层的"角度方向"）、prevDir、base ----
        if (raw == 999.0) {
            // 与 Phase 1 的收尾更新同序：往回找最后一个非 999，angleDir = real + (minus-1)*180
            // 这里 k 是窗口内的下标，往回找只可能落在窗口内（窗口起点由 prevDir 兜住）。
            int minus = 1;
            while ((int64_t)k - minus >= 0 && (*in.angles)[k - minus] == 999.0) ++minus;
            if ((int64_t)k - minus >= 0) {
                const double realAngle = (*in.angles)[k - minus];
                angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
            } else {
                angleDir = (float)std::fmod((double)ck.lastAngleDeg + (double)ck.sentinelRun * 180.0
                                            + (minus - 1) * 180.0, 360.0);
            }
        } else {
            angleDir = (float)std::fmod(raw + 180.0, 360.0);
        }
        if (angleDir < 0) angleDir += 360.0f;

        prevDir = dir;
        const double rad = (double)dir * kPiD / 180.0;
        baseX += std::cos(rad);
        baseY += std::sin(rad);
    }
    (void)settings;
    return true;
}

}  // namespace adocao
}  // namespace adofai
