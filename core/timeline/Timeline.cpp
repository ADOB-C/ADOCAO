#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <limits>
#include "core/util/CountLimits.hpp"


namespace adofai {

void Timeline::build(const LevelData& level, bool exportOnly) {
    m_level = &level;
    m_exportOnly = exportOnly;
    precalculateTiming();

    // Pre-roll: total time before tile 1 (>=360° / 2 beats on tile 0)
    {
        float secPerBeat = 60.0f / level.settings.bpm;
        float offsetSec = level.settings.offset / 1000.0f;
        float min2Beats = 2.0f * secPerBeat;  // 360° at initial BPM
        m_preRoll = std::max(offsetSec, min2Beats);
        m_audioStartOffset = m_preRoll - offsetSec;
    }
}

// Phase 2 worker: compute per-tile geometry for tiles [start, end).
// Each tile is independent — reads pre-computed state arrays and const level data.
static void precalcTileRange(
    const LevelData& level,
    const std::vector<bool>&  tileIsCW,
    const std::vector<float>& tileBPM,
    const std::vector<float>& preAngleDir,
    const std::vector<float>& preExtraRot,
    std::vector<float>& outStartAngles,
    std::vector<float>& outTotalAngles,
    std::vector<float>& outDurations,
    int start, int end, int n)
{
    const auto& tiles = level.tiles;
    const auto& angleData = level.angleData;

    for (int i = start; i < end; i++) {
        bool isCW = tileIsCW[i];
        float currentBPM = tileBPM[i];

        // startAngle = direction INTO tile i (from tile i-1).
        // Unit-step tiles: incoming = tiles[i-1].direction + 180° (reverse).
        float startAngle;
        if (i == 0) {
            startAngle = (level.settings.rotation + 180.0f) * 3.14159265f / 180.0f;
        } else {
            startAngle = std::fmod(tiles[i - 1].direction + 180.0f, 360.0f) * 3.14159265f / 180.0f;
        }
        outStartAngles[i] = startAngle;

        double rawAngleData = (i < (int)angleData.size()) ? angleData[i] : 180.0;
        double relAngle;
        if (rawAngleData == 999.0) {
            relAngle = 0.0;
        } else {
            double delta = std::fmod((double)preAngleDir[i] - rawAngleData, 360.0);
            if (delta < 0) delta += 360.0;
            if (!isCW) {
                relAngle = 360.0 - delta;
                if (relAngle >= 360.0) relAngle -= 360.0;
            } else {
                relAngle = delta;
            }
            if (delta < 0.0001) relAngle = 360.0;
        }
        float totalAngle = (float)relAngle * 3.14159265f / 180.0f;
        if (isCW) totalAngle = -totalAngle;

        if (isCW) totalAngle -= preExtraRot[i] * 2.0f * 3.14159265f;
        else      totalAngle += preExtraRot[i] * 2.0f * 3.14159265f;

        outTotalAngles[i] = totalAngle;

        float rotationAmount = std::abs(totalAngle) / (2.0f * 3.14159265f);
        float duration = rotationAmount * 2.0f * (60.0f / currentBPM);
        outDurations[i] = duration;
    }
}

// A/B 参考实现：Phase 1 的状态递推 + Phase 2 的逐层公式，**逐字**照抄一遍。
// 与生产路径的唯一差别是"这段代码在另一个函数里" —— 这正是要被测量的变量。
void Timeline::recomputeForTest(std::vector<float>& outStartAngles,
                                std::vector<float>& outTotalAngles,
                                std::vector<float>& outDurations) const {
    const int n = (int)m_tileBPM.size();
    outStartAngles.assign(n, 0.0f);
    outTotalAngles.assign(n, 0.0f);
    outDurations.assign(n, 0.0f);
    if (n < 2) return;
    const auto& tiles = m_level->tiles;
    const auto& angleData = m_level->angleData;

    // Phase 1 的两个量：preAngleDir（入层方向）与 preExtraRot（该层 Pause 的累加）
    std::vector<float> preAngleDir(n - 1), preExtraRot(n - 1, 0.0f);
    {
        bool isCW = true;
        float angleDir = 180.0f;
        size_t cursor = 0;
        std::vector<std::pair<int, size_t>> flat;
        for (size_t j = 0; j < m_level->actions.size(); j++) {
            const auto& a = m_level->actions[j];
            if (a.floor >= 0 && a.floor < n) flat.push_back({a.floor, j});
        }
        std::stable_sort(flat.begin(), flat.end());
        for (int i = 0; i < n - 1; i++) {
            preAngleDir[i] = angleDir;
            float extraRotation = 0.0f;
            while (cursor < flat.size() && flat[cursor].first == i) {
                const auto& a = m_level->actions[flat[cursor].second];
                switch (a.type) {
                case LevelData::FastAction::Twirl:    isCW = !isCW; break;
                case LevelData::FastAction::SetSpeed: break;   // BPM 不参与角度公式
                case LevelData::FastAction::Pause:    extraRotation += a.val1 / 2.0f; break;
                default: break;
                }
                cursor++;
            }
            preExtraRot[i] = extraRotation;
            const double rawAngleData = (i < (int)angleData.size()) ? angleData[i] : 180.0;
            if (rawAngleData == 999.0) {
                int minus = 1;
                while (i - minus >= 0 && angleData[i - minus] == 999.0) minus++;
                const double realAngle = (i - minus >= 0) ? angleData[i - minus] : 0.0;
                angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
            } else {
                angleDir = (float)std::fmod(rawAngleData + 180.0, 360.0);
            }
            if (angleDir < 0) angleDir += 360.0f;
        }
    }

    // Phase 2：逐层独立
    for (int i = 0; i < n - 1; i++) {
        const bool isCW = m_tileIsCW[i];
        const float currentBPM = m_tileBPM[i];
        float startAngle;
        if (i == 0) startAngle = (m_level->settings.rotation + 180.0f) * 3.14159265f / 180.0f;
        else startAngle = std::fmod(tiles[i - 1].direction + 180.0f, 360.0f) * 3.14159265f / 180.0f;
        outStartAngles[i] = startAngle;
        const double rawAngleData = (i < (int)angleData.size()) ? angleData[i] : 180.0;
        double relAngle;
        if (rawAngleData == 999.0) {
            relAngle = 0.0;
        } else {
            double delta = std::fmod((double)preAngleDir[i] - rawAngleData, 360.0);
            if (delta < 0) delta += 360.0;
            if (!isCW) { relAngle = 360.0 - delta; if (relAngle >= 360.0) relAngle -= 360.0; }
            else       { relAngle = delta; }
            if (delta < 0.0001) relAngle = 360.0;
        }
        float totalAngle = (float)relAngle * 3.14159265f / 180.0f;
        if (isCW) totalAngle = -totalAngle;
        if (isCW) totalAngle -= preExtraRot[i] * 2.0f * 3.14159265f;
        else      totalAngle += preExtraRot[i] * 2.0f * 3.14159265f;
        outTotalAngles[i] = totalAngle;
        const float rotationAmount = std::abs(totalAngle) / (2.0f * 3.14159265f);
        outDurations[i] = rotationAmount * 2.0f * (60.0f / currentBPM);
    }
    outDurations[n - 1] = 0.0f;
    outStartAngles[n - 1] = outStartAngles[n - 2];
}

float Timeline::angleDirBefore(int64_t i) const {
    // ③ 切片 4：走 `angleAt()`（`angleData` 可能已被换成紧凑存储）
    if (i <= 0) return 180.0f;
    const int64_t k = i - 1;
    const int64_t nAd = m_level->angleCount();
    const double rawAngleData = (k < nAd) ? m_level->angleAt(k) : 180.0;
    float angleDir;
    if (rawAngleData == 999.0) {
        int minus = 1;
        while (k - minus >= 0 && m_level->angleAt(k - minus) == 999.0) minus++;
        const double realAngle = (k - minus >= 0) ? m_level->angleAt(k - minus) : 0.0;
        angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
    } else {
        angleDir = (float)std::fmod(rawAngleData + 180.0, 360.0);
    }
    if (angleDir < 0) angleDir += 360.0f;
    return angleDir;
}
float Timeline::extraRotAt(int64_t i) const {
    auto it = std::lower_bound(m_pauseRot.begin(), m_pauseRot.end(), i,
                               [](const std::pair<int64_t, float>& e, int64_t v) { return e.first < v; });
    return (it != m_pauseRot.end() && it->first == i) ? it->second : 0.0f;
}
float Timeline::startAngleAt(int64_t i) const {
    if (m_level->tileCount() == 0) return 0.0f;
    const int64_t n = (int64_t)m_tileBPM.size();
    if (i <= 0) return (m_level->settings.rotation + 180.0f) * 3.14159265f / 180.0f;
    if (i >= n - 1) return startAngleAt(n - 2);
    return std::fmod(m_level->tileDirectionAt(i - 1) + 180.0f, 360.0f) * 3.14159265f / 180.0f;
}
float Timeline::totalAngleAt(int64_t i) const {
    if (m_level->tileCount() == 0) return 0.0f;
    const int64_t nAll = (int64_t)m_tileBPM.size();
    if (i >= nAll - 1) return m_lastTotalAngle;
    const bool isCW = m_tileIsCW[(size_t)i];
    const double rawAngleData = (i < m_level->angleCount()) ? m_level->angleAt(i) : 180.0;
    double relAngle;
    if (rawAngleData == 999.0) {
        relAngle = 0.0;
    } else {
        double delta = std::fmod((double)angleDirBefore(i) - rawAngleData, 360.0);
        if (delta < 0) delta += 360.0;
        if (!isCW) { relAngle = 360.0 - delta; if (relAngle >= 360.0) relAngle -= 360.0; }
        else       { relAngle = delta; }
        if (delta < 0.0001) relAngle = 360.0;
    }
    float totalAngle = (float)relAngle * 3.14159265f / 180.0f;
    if (isCW) totalAngle = -totalAngle;
    const float rot = extraRotAt(i);
    if (isCW) totalAngle -= rot * 2.0f * 3.14159265f;
    else      totalAngle += rot * 2.0f * 3.14159265f;
    return totalAngle;
}
float Timeline::durationAt(int64_t i) const {
    const int64_t n = (int64_t)m_tileBPM.size();
    if (i >= n - 1) return m_exportOnly ? 0.1f : 0.0f;
    const float rotationAmount = std::abs(totalAngleAt(i)) / (2.0f * 3.14159265f);
    return rotationAmount * 2.0f * (60.0f / m_tileBPM[(size_t)i]);
}
void Timeline::precalculateTiming() {
    const auto& tiles = m_level->tiles;
    const auto& angleData = m_level->angleData;
    // Export mode intentionally leaves LevelData::tiles empty; the timeline can
    // still be built from angleData (which is angleData.size()+1 tiles).
    // #0：int64 算，收窄前显式判（原来 `(int)angleData.size() + 1` 在 2^31−1 时溢出为负）。
    const int64_t nFull = countFromTilesOrAngles((int64_t)tiles.size(), (int64_t)angleData.size());
    if (!fitsInt32(nFull)) {
        LOG_E("Timeline::precalculateTiming 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32",
              (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    if (n < 2) return;

    m_tileStartTimes.resize(n);
    m_tileBPM.resize(n);
    m_tileIsCW.resize(n);
    // 每层距离只在最后一层可能是真值，其余恒 1.0f —— 所以只记层数 + 两个标量（见 Timeline.hpp）。
    // 原先是两条 vector<float>：8 B/层，2^31 层就是 17 GB。
    m_distCount = (size_t)n;

    // Phase 0: Build sorted flat action index (O(m) space instead of O(n))
    std::vector<std::pair<int, size_t>> flatActions;
    for (size_t j = 0; j < m_level->actions.size(); j++) {
        auto& a = m_level->actions[j];
        if (a.floor >= 0 && a.floor < n) flatActions.push_back({a.floor, j});
    }
    std::stable_sort(flatActions.begin(), flatActions.end());

    // Phase 1 (sequential): Write directly to m_tileIsCW/m_tileBPM

    bool isCW = true;
    float currentBPM = m_level->settings.bpm;
    float angleDir = 180.0f;
    size_t actionCursor = 0;

    for (int i = 0; i < n - 1; i++) {
        float extraRotation = 0.0f;

        while (actionCursor < flatActions.size() && flatActions[actionCursor].first == i) {
            auto& a = m_level->actions[flatActions[actionCursor].second];
            switch (a.type) {
            case LevelData::FastAction::Twirl: isCW = !isCW; break;
            case LevelData::FastAction::SetSpeed:
                if (a.flag) currentBPM *= a.val1; else currentBPM = a.val1; break;
            case LevelData::FastAction::Pause: extraRotation += a.val1 / 2.0f; break;
            default: break;
            }
            actionCursor++;
        }

        m_tileIsCW[i] = isCW;
        m_tileBPM[i] = currentBPM;
        if (extraRotation != 0.0f) m_pauseRot.push_back({(int64_t)i, extraRotation});

        double rawAngleData = (i < (int)angleData.size()) ? angleData[i] : 180.0;
        if (rawAngleData == 999.0) {
            // ADOFAI-JS _parseAngle: backtrack to last non-999 angle,
            // then angleDir = normalize(realAngle + (minus-1)*180)
            int minus = 1;
            while (i - minus >= 0 && angleData[i - minus] == 999.0) {
                minus++;
            }
            double realAngle = (i - minus >= 0) ? angleData[i - minus] : 0.0;
            angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
            if (angleDir < 0) angleDir += 360.0f;
        } else {
            angleDir = (float)std::fmod(rawAngleData + 180.0, 360.0);
            if (angleDir < 0) angleDir += 360.0f;
        }
    }
    m_tileIsCW[n - 1] = isCW;
    m_tileBPM[n - 1] = currentBPM;

    // Phase 3: Prefix sum
    double totalTime = 0.0;

    for (int i = 0; i < n - 1; i++) {
        m_tileStartTimes[i] = totalTime;
        totalTime += (double)durationAt(i);   // 时长不存了：后面按块重算用同一个 durationAt() 现算
    }
    if (n > 0) m_tileStartTimes[n - 1] = totalTime;

    double preShiftTotal = totalTime;
    if (n > 1) {
        double shift = m_tileStartTimes[1];
        for (int i = 0; i < n; i++) m_tileStartTimes[i] -= shift;
    }

    // Phase 4: Last tile — use pre-built index instead of scanning all actions.
    // Export mode has no tile geometry, so this phase is only needed for normal
    // rendering/timeline data.
    if (!m_exportOnly) {
        const int lastIdx = n - 1;
        m_lastTotalAngle = totalAngleAt(lastIdx);

        while (actionCursor < flatActions.size() && flatActions[actionCursor].first == lastIdx) {
            auto& a = m_level->actions[flatActions[actionCursor].second];
            switch (a.type) {
            case LevelData::FastAction::Twirl: m_tileIsCW[lastIdx] = !m_tileIsCW[lastIdx]; break;
            case LevelData::FastAction::SetSpeed:
                if (a.flag) m_tileBPM[lastIdx] *= a.val1; else m_tileBPM[lastIdx] = a.val1; break;
            default: break;
            }
            actionCursor++;
        }

        {
            const auto& p = tiles[lastIdx].position;
            if (lastIdx > 0) {
                const auto& pp = tiles[lastIdx - 1].position;
                float dx = (float)(p[0] - pp[0]), dy = (float)(p[1] - pp[1]);
                float d = std::sqrt(dx * dx + dy * dy);
                m_lastStartDist = d > 0.01f ? d : 1.0f;
            }
            m_lastEndDist = m_lastStartDist;
        }
    }

    // Phase 5: Per-tile visibility windows (trackDisappearAnimation)
    // Uses C# ApplyEventsToFloors num5/num6/flag2 speed ratio system
    //
    // 只有"这张谱真的用到"才分配这两个数组（16 B/层，1e9 层就是 16 GB）。判据与消费方
    // app/LevelScene.cpp 的 m_tileVisEnabled **逐字相同**，而那边在 `if (!m_tileVisEnabled) return;`
    // 之后才读它们 —— 所以这是行为等价的省略：判据为假时旧代码填的是全 ±inf、消费方判定 disabled；
    // 现在留空，判定同样是 disabled。判据一旦改动，两处必须一起改。
    const bool needsTrackVis = (m_level->settings.trackDisappearAnimation != "None" ||
                               m_level->settings.trackAnimation != "None" ||
                               m_level->hasAtStates);
    if (needsTrackVis)
    {
        m_tileDisappearTimes.assign(n, std::numeric_limits<double>::infinity());
        m_tileAppearTimes.assign(n, -std::numeric_limits<double>::infinity());
        std::string curDA = m_level->settings.trackDisappearAnimation;
        std::string curAA = m_level->settings.trackAnimation;
        float curBB = m_level->settings.beatsBehind, curBA = m_level->settings.beatsAhead;
        float num5 = m_tileBPM[0], num6 = m_tileBPM[0];
        bool flag2 = false;
        for (int i = 0; i < n - 1; i++) {
            // C# ApplyEventsToFloors: num5/num6 advance EVERY tile (i>0), independent of events
            if (i > 0) { num5 = num6; num6 = m_tileBPM[i]; }
            auto it = m_level->atStates.find(i);
            if (it != m_level->atStates.end()) {
                auto& st = it->second;
                if (!st.da.empty()) { curDA = st.da; curBB = st.bb; }
                if (st.hasAA) { curAA = st.aa; curBA = st.ba; }
                // C# semantics: flag2 latches true once any floor has angleData, never resets
                if (st.hasAA) flag2 = true;
            }
            float refBPM = flag2 ? num6 : num5;
            // beatsBehind * 60 / refBPM  (tile BPM cancels out in reference formula)
            if (curDA != "None")
                m_tileDisappearTimes[i] = m_tileStartTimes[i + 1] + (double)(curBB * 60.0f / refBPM);
            if (curAA != "None")
                m_tileAppearTimes[i] = m_tileStartTimes[i] - (double)(curBA * 60.0f / refBPM);
        }
        // Debug: count tiles with finite disappear times
        int dc = 0; for (int i = 0; i < n; i++) if (m_tileDisappearTimes[i] < 1e100) dc++;
        LOG_D("TrackVis Phase5: %d/%d tiles have disappearTime, curDA=%s curBB=%.1f curBA=%.1f refBPM=%.1f",
              dc, n, curDA.c_str(), curBB, curBA, flag2 ? num6 : num5);
        // ③ 切片 2：记下恒定态（无 AnimateTrack 事件时整段不变），供按块重算
        m_visDA = curDA; m_visAA = curAA; m_visBB = curBB; m_visBA = curBA;
        m_visNum5 = (n > 0) ? m_tileBPM[0] : 0.0f;

    }
    m_needsTrackVis = needsTrackVis;
    // 可重算判据：不需要 TrackVis 时只看时长；需要 TrackVis 时还要求块内没有 AT 事件
    // （FastAction 只留了 hasTrackAnimation 的 bool，没留 trackAnimation 的值 → 有 AT 就不严格）。
    if (!needsTrackVis) { m_blocksRecomputable = true; m_blocksWhy = "不需要 TrackVis，只需时长前缀"; }
    else if (!m_level->hasAtStates) { m_blocksRecomputable = true; m_blocksWhy = "TrackVis 状态恒定（无 AnimateTrack）"; }
    else { m_blocksRecomputable = false; m_blocksWhy = "有 AnimateTrack：FastAction 没留 trackAnimation 值"; }

    // ③ 切片 1：记录锚点（每 kAnchorStride 层一个）。**只记录，不改消费方** ——
    // 本切片的验收是"锚点值 == 全量数组在该层逐位相同"（tests/timeline_anchor_test.cpp）。
    m_anchors.clear();
    if (n > 0) {
        const bool hasTV = !m_tileDisappearTimes.empty();
        for (int64_t i = 0; i < (int64_t)n; i += kAnchorStride) {
            Anchor a;
            a.floor = i;
            a.startTime = m_tileStartTimes[(size_t)i];
            a.bpm = m_tileBPM[(size_t)i];
            a.cw = m_tileIsCW[(size_t)i];
            if (hasTV) {
                a.hasTrackVis = true;
                a.disappearTime = m_tileDisappearTimes[(size_t)i];
                a.appearTime = m_tileAppearTimes[(size_t)i];
            }
            m_anchors.push_back(a);
        }
        LOG_D("Timeline: %zu 个锚点（每 %lld 层）", m_anchors.size(), (long long)kAnchorStride);

        // ③ 切片 3：粗索引（每锚点一份 TrackVis 时间，26 KB 换掉 103 MB）+ 单调性判据。
        // **必须在锚点记完之后建**（它在 Phase 5 之后，早建会遍历空数组）。
        m_coarseDA.clear(); m_coarseAA.clear();
        m_visCoarseMonotone = false;
        if (m_needsTrackVis) {
            m_coarseDA.reserve(m_anchors.size()); m_coarseAA.reserve(m_anchors.size());
            for (const Anchor& a : m_anchors) {
                const size_t k = (size_t)a.floor;
                m_coarseDA.push_back(k < m_tileDisappearTimes.size() ? m_tileDisappearTimes[k] : 0.0);
                m_coarseAA.push_back(k < m_tileAppearTimes.size() ? m_tileAppearTimes[k] : 0.0);
            }
            // 消费方的二分（原实现）就假设单调；只有粗索引也单调才敢窗口化
            m_visCoarseMonotone = true;
            for (size_t k = 1; k < m_coarseDA.size(); k++) {
                if (!(m_coarseDA[k] >= m_coarseDA[k - 1]) || !(m_coarseAA[k] >= m_coarseAA[k - 1])) {
                    m_visCoarseMonotone = false;
                    break;
                }
            }
        }
    }

    auto [minBPM, maxBPM] = std::minmax_element(m_tileBPM.begin(), m_tileBPM.end());
    LOG_D("Timeline: %d tiles, total duration %.2fs, bpm range %.0f-%.0f",
          n - 1, preShiftTotal, *minBPM, *maxBPM);
}

bool Timeline::recomputeBlock(int64_t i0, int64_t i1, BlockResult& out) const {
    const int64_t n = tileCount();   // 丢过密集数组也要成立（这正是窗口化的用途）
    if (i0 < 0 || i1 > n || i0 >= i1) return false;
    const int64_t stride = kAnchorStride;
    const int64_t b = i0 / stride;
    if (b >= (int64_t)m_anchors.size() || m_anchors[(size_t)b].floor != i0) return false;
    if ((int64_t)m_tileBPM.size() != n) return false;   // 前置：BPM/角度/cw 都在（时长现算）
    // 起始时间只依赖"时长 + 锚点"，任何谱都可算；TrackVis 两支才需要额外条件（见下）

    // 1) 起始时间：从锚点的**已存值**起步，按层序把时长（转 double）累加 —— 与原前缀同一顺序、
    //    同一操作，所以逐位相同。为 TrackVis 那一遍多算一层（disappear[i] 要用 start[i+1]）。
    const int64_t last = std::min(i1 + 1, n);
    out.startTimes.assign((size_t)(last - i0), 0.0);
    double t = m_anchors[(size_t)b].startTime;
    for (int64_t i = i0; i < last; i++) {
        out.startTimes[(size_t)(i - i0)] = t;
        if (i < n - 1) t += (double)durationAt(i);   // 现算：与 precalculateTiming 同一函数、同一顺序
    }
    // TrackVis 两支：只有"需要 + 可重算"时才给；否则留空（消费方走 trackVisWindowed() 判定）
    if (!m_needsTrackVis || !m_blocksRecomputable) {
        out.disappearTimes.clear(); out.appearTimes.clear(); return true;
    }

    // 2) TrackVis 两支：复刻 Phase 5 的循环（i ∈ [0, n-1) 才写；refBPM 在无 AT 时取 num5）
    out.disappearTimes.assign((size_t)(i1 - i0), std::numeric_limits<double>::infinity());
    out.appearTimes.assign((size_t)(i1 - i0), -std::numeric_limits<double>::infinity());
    const bool writeDA = (m_visDA != "None");
    const bool writeAA = (m_visAA != "None");
    const float bpm0 = (n > 0) ? m_tileBPM[0] : 0.0f;
    for (int64_t i = i0; i < i1 && i < n - 1; i++) {
        // num5/num6 的推进规则与 Phase 5 逐字相同：i>0 时 num5=num6、num6=bpm[i]；i==0 用初值
        float num5, num6;
        if (i == 0) { num5 = m_visNum5; num6 = m_visNum5; }
        else { num5 = (i - 1 == 0) ? bpm0 : m_tileBPM[(size_t)(i - 1)]; num6 = m_tileBPM[(size_t)i]; }
        const float refBPM = num5;   // flag2 == false（无 AT）
        const double st  = out.startTimes[(size_t)(i - i0)];
        const double st1 = out.startTimes[(size_t)(i - i0 + 1)];
        if (writeDA) out.disappearTimes[(size_t)(i - i0)] = st1 + (double)(m_visBB * 60.0f / refBPM);
        if (writeAA) out.appearTimes[(size_t)(i - i0)]    = st  - (double)(m_visBA * 60.0f / refBPM);
    }
    return true;
}

// ③ 切片 3：物化 i 所在的块（单块缓存）。返回该块，出错则 nullptr
const Timeline::BlockResult* Timeline::windowFor(int64_t i) const {
    if (i < 0 || i >= tileCount()) return nullptr;
    const int64_t b = i / kAnchorStride;
    if (m_winBlock == b && !m_winCache.startTimes.empty()) return &m_winCache;
    const int64_t i0 = b * kAnchorStride;
    const int64_t i1 = std::min(i0 + kAnchorStride, tileCount());
    BlockResult r;
    if (!recomputeBlock(i0, i1, r)) return nullptr;
    m_winCache = std::move(r);
    m_winBlock = b;
    return &m_winCache;
}

double Timeline::startTimeAt(int64_t i) const {
    if (!m_startWindowed) {
        return (i >= 0 && (size_t)i < m_tileStartTimes.size()) ? m_tileStartTimes[(size_t)i] : 0.0;
    }
    const BlockResult* w = windowFor(i);
    if (!w) return 0.0;
    const size_t k = (size_t)(i - (i / kAnchorStride) * kAnchorStride);
    return k < w->startTimes.size() ? w->startTimes[k] : 0.0;
}

bool Timeline::dropStartTimesArray() {
    const size_t n = m_tileBPM.size();
    if (n == 0) return false;
    if (m_anchors.empty() || m_tileBPM.size() != n) return false;     // 前置：锚点 + BPM 齐备
    std::vector<double>().swap(m_tileStartTimes);
    m_startWindowed = true;
    m_winBlock = -1;         // 缓存里的 startTimes 仍有效，但保持一致语义
    return true;
}

double Timeline::disappearTimeAt(int64_t i) const {
    if (!m_visWindowed) {
        return (i >= 0 && (size_t)i < m_tileDisappearTimes.size()) ? m_tileDisappearTimes[(size_t)i]
                                                                   : std::numeric_limits<double>::infinity();
    }
    const BlockResult* w = windowFor(i);
    if (!w) return std::numeric_limits<double>::infinity();
    const size_t k = (size_t)(i - (i / kAnchorStride) * kAnchorStride);
    return k < w->disappearTimes.size() ? w->disappearTimes[k] : std::numeric_limits<double>::infinity();
}

double Timeline::appearTimeAt(int64_t i) const {
    if (!m_visWindowed) {
        return (i >= 0 && (size_t)i < m_tileAppearTimes.size()) ? m_tileAppearTimes[(size_t)i]
                                                                : -std::numeric_limits<double>::infinity();
    }
    const BlockResult* w = windowFor(i);
    if (!w) return -std::numeric_limits<double>::infinity();
    const size_t k = (size_t)(i - (i / kAnchorStride) * kAnchorStride);
    return k < w->appearTimes.size() ? w->appearTimes[k] : -std::numeric_limits<double>::infinity();
}

bool Timeline::dropTrackVisArrays() {
    if (!m_needsTrackVis) return true;                 // 本来就没有
    if (!m_blocksRecomputable) return false;           // 有 AT：留着密集数组（行为不变）
    if (m_coarseDA.empty()) return false;              // 防御：粗索引必须已经建好
    if (!m_visCoarseMonotone) return false;            // 粗索引不单调 → 二分的等价性没保证
    std::vector<double>().swap(m_tileDisappearTimes);
    std::vector<double>().swap(m_tileAppearTimes);
    m_visWindowed = true;
    m_winBlock = -1;
    return true;
}

double Timeline::totalDuration() const {
    const int64_t n = tileCount();
    if (n < 2) return 0.0f;
    return startTimeAt(n - 1) + 10.0;
}

bool Timeline::buildRawPcm(std::vector<float>& out, double& sampleRateOut) const {
    if (!m_level) return false;
    const LevelData& lv = *m_level;
    const size_t n = lv.tiles.size();
    if (n < 2) return false;
    // 逐层音量必须齐（audio-as-chart 是"每个采样一个覆盖"），否则这不是那种谱
    if (lv.tileHitsoundVolumes.size() < n) return false;
    // 1 层 = 1 采样 要求时长严格 60/bpm，所以 BPM 必须恒定（容 0.1%）
    const double bpm = m_tileBPM.size() > 1 ? (double)m_tileBPM[1] : (double)lv.settings.bpm;
    if (bpm <= 1.0) return false;
    for (size_t i = 1; i < m_tileBPM.size(); i++)
        if (std::fabs((double)m_tileBPM[i] - bpm) > bpm * 1e-3) return false;
    const double rate = bpm / 60.0;
    if (rate < 1000.0 || rate > 400000.0) return false;

    out.resize(n);
    size_t atFullScale = 0;
    for (size_t i = 0; i < n; i++) {
        const float v = lv.hasHitsoundVolume((int)i) ? lv.tileHitsoundVolumes[i]
                                                     : lv.settings.hitsoundVolume;
        float smp = v * 655.36f / 32768.0f;            // 编码 int16/655.36 的逆映射
        if (smp > 1.0f) { smp = 1.0f; atFullScale++; }
        else if (smp < -1.0f) { smp = -1.0f; atFullScale++; }
        out[i] = smp;
    }
    if (atFullScale * 20 > n)                           // >5% 顶满 = 数据可疑，记一笔便于排查
        LOG_W("raw-pcm: %zu/%zu 个采样顶着满刻度（多半是缺失覆盖落到了默认音量 %.2f）",
              atFullScale, n, (double)lv.settings.hitsoundVolume);
    sampleRateOut = rate;
    return true;
}

// 真实层数 = 谱面里的砖数。`m_tileStartTimes` 的**最后一格是合成砖**
// （`calculateTilePositions()` 追加的"无限旋转参考"，见 AGENTS 里那条 extra 砖的说明），
// 它不是谱面里的砖 —— 打拍音不能给它排一击（曾经末尾会多响一下，Kick 和弦那种
// "一击不多一击不少"的谱上直接量得出来）。
int Timeline::realTileCount() const {
    int n = (int)tileCount();
    if (m_level && !m_level->angleData.empty() && (int)m_level->angleData.size() < n)
        n = (int)m_level->angleData.size();
    return n;
}

std::vector<double> Timeline::getHitsoundTimestamps() const {
    std::vector<double> timestamps;
    int n = realTileCount();
    if (n < 2) return timestamps;
    float offsetSec = m_level->settings.offset / 1000.0f;
    for (int i = 1; i < n; i++)
        timestamps.push_back(startTimeAt(i) + offsetSec);
    return timestamps;
}

std::vector<HitsoundTimestampGroup> Timeline::getHitsoundTimestampGroups() const {
    std::vector<HitsoundTimestampGroup> groups;
    int n = realTileCount();
    if (n < 2) return groups;

    float countdown = (float)m_level->settings.countdownTicks * (60.0f / m_level->settings.bpm);
    float offsetSec = m_level->settings.offset / 1000.0f;
    float defaultVol = m_level->settings.hitsoundVolume;
    const std::string& defaultType = m_level->settings.hitsound;

    std::map<std::pair<std::string, float>, size_t> groupMap;

    for (int i = 1; i < n; i++) {
        auto hsIt = m_level->tileHitsounds.find(i);
        std::string type = (hsIt != m_level->tileHitsounds.end()) ? hsIt->second : defaultType;
        if (!m_forceHitsoundType.empty()) type = m_forceHitsoundType;
        float vol = m_level->hasHitsoundVolume(i) ? m_level->tileHitsoundVolumes[(size_t)i] : defaultVol;

        auto key = std::make_pair(type, vol);
        auto it = groupMap.find(key);
        if (it == groupMap.end()) {
            groupMap[key] = groups.size();
            groups.push_back({type, vol, {}});
        }
        groups[groupMap[key]].timestamps.push_back(startTimeAt(i) + offsetSec);
    }
    return groups;
}

int Timeline::findTileIndex(double t) const {
    const int64_t n = tileCount();
    if (n < 2) return 0;
    if (!m_startWindowed) {
        // 未窗口化：保持原实现（逐位同结果）
        int lo = 0, hi = (int)n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (m_tileStartTimes[(size_t)mid] <= t) lo = mid + 1;
            else hi = mid - 1;
        }
        int idx = hi;
        if (idx < 0) idx = 0;
        if (idx >= (int)n) idx = (int)n - 1;
        return idx;
    }
    // 窗口化：先在锚点上二分（startTime 单调不减：duration 恒 ≥ 0），再在那一块内取
    // "最后一个 startTime <= t"的层（与原实现的结果相同；块内扫描对非单调也更稳）。
    int64_t lo = 0, hi = (int64_t)m_anchors.size() - 1, aEnd = -1;
    while (lo <= hi) {
        const int64_t m = (lo + hi) / 2;
        if (m_anchors[(size_t)m].startTime <= t) { aEnd = m; lo = m + 1; } else hi = m - 1;
    }
    if (aEnd < 0) return 0;
    const int64_t from = aEnd * kAnchorStride;
    const int64_t to = std::min(from + kAnchorStride, n);
    int64_t idx = -1;
    for (int64_t i = from; i < to; ++i)
        if (startTimeAt(i) <= t) idx = i;
    if (idx < 0) idx = 0;
    return (int)idx;
}

}  // namespace adofai
