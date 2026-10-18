#include "core/camera/CameraTimeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace adofai {

namespace {

// entryBeat：复刻原版 scrLevelMaker.CalculateFloorEntryTimes（Re_ADOJAS `computeEntryBeats`）。
//   eb[0] = -1（**哨兵**，不是笔误：原版就是这么写的，base=0 时会真的多出 -1 拍）；
//   eb[j] = Σ_{f<j} 该砖拍数（j = 1..n-2）；最后一砖保持默认 0。
// 只在真的存在 Floor 模式重复时才计算（O(n) 单趟；这里拿到的是**密集** tileStartTimes，
// 因为时间线在 `dropStartTimesArray()` 之前构建 —— 巨谱上也只是几百 ms，且只在用到时才付）。
std::vector<double> computeEntryBeats(const std::vector<double>& tileStartTimes,
                                      const std::vector<float>& tileBPMs) {
    const size_t n = tileStartTimes.size();
    std::vector<double> eb(n, 0.0);
    if (n <= 1) return eb;
    eb[0] = -1.0;
    double beats = 0.0;
    for (size_t j = 1; j + 1 < n; j++) {
        eb[j] = beats;
        const double t0 = tileStartTimes[j];
        const double t1 = tileStartTimes[j + 1];
        const double bpm = (j < tileBPMs.size() && tileBPMs[j] > 0.0f) ? (double)tileBPMs[j] : 100.0;
        beats += ((t1 - t0) * bpm) / 60.0;
    }
    return eb;
}

inline double entryBeatOf(const std::vector<double>& eb, int64_t i) {
    return (i >= 0 && (size_t)i < eb.size()) ? eb[(size_t)i] : 0.0;
}

// `eventTag` / `tag` 是**空格分隔的多个 tag**：按空格拆开，逐个查表、命中第一个就停
// （Re_ADOJAS 的 `eventTag.split(' ').filter(Boolean)` + `for … break`）。
inline bool tagMatches(const std::string& eventTag, const std::string& tableTag) {
    size_t p = 0;
    while (p <= eventTag.size()) {
        size_t q = eventTag.find(' ', p);
        if (q == std::string::npos) q = eventTag.size();
        if (q > p && eventTag.compare(p, q - p, tableTag) == 0) return true;
        p = q + 1;
    }
    return false;
}

struct RepeatInfo {
    float  repetitions = 0.0f;
    float  interval = 1.0f;
    float  gapLength = 1.0f;
    bool   isBeat = true;
    bool   executeOnCurrentFloor = false;
};

}  // namespace

void CameraTimeline::build(const LevelData& level,
                           const std::vector<double>& tileStartTimes,
                           const std::vector<float>& tileBPMs) {
    m_keys.clear();
    m_droppedInactive = 0;
    if (level.cameraEvents.empty()) return;

    // ── RepeatEvents 表：floor → 该层上的 [tag, info]（同 tag 后写的覆盖先写的，与 Re_ADOJAS 一致）
    struct TagRow { std::string tag; RepeatInfo info; };
    std::unordered_map<int32_t, std::vector<TagRow>> repeatTable;
    for (const LevelData::RepeatEvent& rp : level.repeatEvents) {
        RepeatInfo info;
        info.isBeat = (rp.flags & 1) != 0;
        info.executeOnCurrentFloor = (rp.flags & 2) != 0;
        info.repetitions = rp.repetitions;
        info.interval = rp.interval;
        info.gapLength = rp.gapLength;
        if (rp.tag.empty()) continue;            // 没 tag 的 RepeatEvents 匹配不到任何事件
        auto& rows = repeatTable[rp.floor];
        bool replaced = false;
        for (TagRow& row : rows) {
            if (row.tag == rp.tag) { row.info = info; replaced = true; break; }   // 后写覆盖
        }
        if (!replaced) rows.push_back(TagRow{rp.tag, info});
    }

    // Floor 模式的角偏移要 entryBeat —— 只有真用得上才算（O(n)）。
    bool needEntryBeat = false;
    for (const LevelData::CameraEvent& ev : level.cameraEvents) {
        auto it = repeatTable.find(ev.floor);
        if (it == repeatTable.end()) continue;
        for (const TagRow& row : it->second) {
            const bool beatMode = row.info.isBeat && row.info.interval > 0.0f;   // Re_ADOJAS 的 interval 归一
            if (tagMatches(ev.tag, row.tag) && !beatMode && !row.info.executeOnCurrentFloor) {
                needEntryBeat = true;
                break;
            }
        }
        if (needEntryBeat) break;
    }
    std::vector<double> entryBeat = needEntryBeat ? computeEntryBeats(tileStartTimes, tileBPMs)
                                                  : std::vector<double>{};

    const size_t totalTiles = level.tiles.size();
    const size_t nTimes = tileStartTimes.size();
    auto timeAt = [&](int64_t floor, double angleOffsetDeg) -> double {
        const double start = (floor >= 0 && (size_t)floor < nTimes) ? tileStartTimes[(size_t)floor] : 0.0;
        const double bpm = (floor >= 0 && (size_t)floor < tileBPMs.size() && tileBPMs[(size_t)floor] > 0.0f)
                               ? (double)tileBPMs[(size_t)floor] : 100.0;
        return start + (angleOffsetDeg / 180.0) * (60.0 / bpm);
    };
    auto bpmAt = [&](int64_t floor) -> float {
        return (floor >= 0 && (size_t)floor < tileBPMs.size()) ? tileBPMs[(size_t)floor] : 100.0f;
    };

    m_keys.reserve(level.cameraEvents.size());
    for (const LevelData::CameraEvent& ev : level.cameraEvents) {
        if (ev.flags & LevelData::kCamInactive) { m_droppedInactive++; continue; }
        if (ev.flags & LevelData::kCamEditorOnly) { m_droppedInactive++; continue; }
        if (ev.floor < 0) { m_droppedInactive++; continue; }

        // 找重复表：只在**事件宿主砖**上找（Re_ADOJAS 用 action.floor，不是展开后的 floor）
        const RepeatInfo* info = nullptr;
        auto it = repeatTable.find(ev.floor);
        if (it != repeatTable.end()) {
            for (const TagRow& row : it->second) {
                if (tagMatches(ev.tag, row.tag)) { info = &row.info; break; }
            }
        }

        if (!info) {
            CameraKey k;
            k.time = timeAt(ev.floor, ev.angleOffset);
            k.anchorX = level.tilePositionAt(ev.floor)[0];
            k.anchorY = level.tilePositionAt(ev.floor)[1];
            k.bpm = bpmAt(ev.floor);
            k.ev = ev;
            m_keys.push_back(std::move(k));
            continue;
        }

        // Re_ADOJAS 用 `interval > 0` 判 Beat 模式（不是看 repeatType）—— Floor 模式 interval 归一成 -1。
        const bool isBeatMode = info->isBeat && info->interval > 0.0f;
        const int64_t baseFloor = ev.floor;
        for (double rep = 0.0; rep <= (double)info->repetitions; rep += 1.0) {
            int64_t targetFloor = isBeatMode ? baseFloor : baseFloor + (int64_t)(rep * (double)info->gapLength);
            if (targetFloor >= (int64_t)totalTiles) break;
            if (targetFloor < 0) continue;

            int64_t epFloor;
            double epAngleOffset;
            if (isBeatMode) {
                epFloor = baseFloor;
                epAngleOffset = (double)ev.angleOffset + (double)info->interval * rep * 180.0;
            } else if (info->executeOnCurrentFloor) {
                epFloor = targetFloor;
                epAngleOffset = (double)ev.angleOffset;
            } else {
                epFloor = baseFloor;
                epAngleOffset = (double)ev.angleOffset +
                                (entryBeatOf(entryBeat, targetFloor) - entryBeatOf(entryBeat, baseFloor)) * 180.0;
            }

            CameraKey k;
            k.time = timeAt(epFloor, epAngleOffset);
            k.anchorX = level.tilePositionAt(epFloor)[0];
            k.anchorY = level.tilePositionAt(epFloor)[1];
            k.bpm = bpmAt(epFloor);
            k.ev = ev;
            k.ev.floor = (int32_t)epFloor;          // 与 Player 的 `{...event, floor}` 一致
            k.ev.angleOffset = (float)epAngleOffset;
            m_keys.push_back(std::move(k));
        }
    }

    // 排序键与 Re_ADOJAS 完全相同：|Δt| < 0.1 ms 视为同刻 → 按 id；id 相同 → 文件顺序。
    std::stable_sort(m_keys.begin(), m_keys.end(), [](const CameraKey& a, const CameraKey& b) {
        const double dt = a.time - b.time;
        if (std::fabs(dt) < 1e-4) {
            if (a.ev.id != b.ev.id) return a.ev.id < b.ev.id;
            return a.ev.order < b.ev.order;
        }
        return dt < 0.0;
    });
}

}  // namespace adofai
