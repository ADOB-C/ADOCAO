#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <limits>


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
    std::vector<float>& outStartDist,
    std::vector<float>& outEndDist,
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

        outStartDist[i] = 1.0f;
        outEndDist[i]   = 1.0f;
    }
}

void Timeline::precalculateTiming() {
    const auto& tiles = m_level->tiles;
    const auto& angleData = m_level->angleData;
    // Export mode intentionally leaves LevelData::tiles empty; the timeline can
    // still be built from angleData (which is angleData.size()+1 tiles).
    int n = !tiles.empty() ? (int)tiles.size() : (int)angleData.size() + 1;
    if (n < 2) return;

    m_tileStartTimes.resize(n);
    m_tileDurations.resize(n);
    m_tileTotalAngles.resize(n);
    m_tileStartAngles.resize(n);
    m_tileBPM.resize(n);
    m_tileIsCW.resize(n);
    m_tileStartDist.resize(n);
    m_tileEndDist.resize(n);

    // Phase 0: Build sorted flat action index (O(m) space instead of O(n))
    std::vector<std::pair<int, size_t>> flatActions;
    for (size_t j = 0; j < m_level->actions.size(); j++) {
        auto& a = m_level->actions[j];
        if (a.floor >= 0 && a.floor < n) flatActions.push_back({a.floor, j});
    }
    std::stable_sort(flatActions.begin(), flatActions.end());

    // Phase 1 (sequential): Write directly to m_tileIsCW/m_tileBPM
    std::vector<float> preAngleDir(n - 1);
    std::vector<float> preExtraRot(n - 1, 0.0f);

    bool isCW = true;
    float currentBPM = m_level->settings.bpm;
    float angleDir = 180.0f;
    size_t actionCursor = 0;

    for (int i = 0; i < n - 1; i++) {
        preAngleDir[i] = angleDir;
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
        preExtraRot[i] = extraRotation;

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

    // Phase 2: Per-tile durations
    if (m_exportOnly) {
        for (int i = 0; i < n - 1; i++) {
            double rawAng = (i < (int)angleData.size()) ? angleData[i] : 180.0;
            double relAngle;
            if (rawAng == 999.0) { relAngle = 0.0; } else {
                double delta = std::fmod((double)preAngleDir[i] - rawAng, 360.0);
                if (delta < 0) delta += 360.0;
                if (!m_tileIsCW[i]) relAngle = (delta < 0.0001) ? 360.0 : 360.0 - delta;
                else relAngle = (delta < 0.0001) ? 360.0 : delta;
            }
            double rot = relAngle / 360.0 + (double)preExtraRot[i];
            m_tileDurations[i] = (float)(rot * 2.0 * (60.0 / m_tileBPM[i]));
            m_tileStartAngles[i] = 0.0f; m_tileTotalAngles[i] = 0.0f;
            m_tileStartDist[i] = 1.0f; m_tileEndDist[i] = 1.0f;
        }
        m_tileDurations[n - 1] = 0.1f;
    } else {
        constexpr int PARALLEL_THRESHOLD = 256;
        int workItems = n - 1;
        if (workItems >= PARALLEL_THRESHOLD) {
            unsigned int hw = std::thread::hardware_concurrency();
            if (hw == 0) hw = 2;
            unsigned int numThreads = std::min(hw, (unsigned)(workItems / 64));
            if (numThreads < 2) numThreads = 2;
            size_t chunk = ((size_t)workItems + numThreads - 1) / numThreads;
            std::vector<std::future<void>> futures;
            for (unsigned int t = 0; t < numThreads; t++) {
                size_t s = t * chunk;
                size_t e = std::min(s + chunk, (size_t)workItems);
                if (s >= e) break;
                futures.push_back(std::async(std::launch::async,
                    precalcTileRange, std::cref(*m_level),
                    std::cref(m_tileIsCW), std::cref(m_tileBPM),
                    std::cref(preAngleDir), std::cref(preExtraRot),
                    std::ref(m_tileStartAngles), std::ref(m_tileTotalAngles),
                    std::ref(m_tileDurations), std::ref(m_tileStartDist), std::ref(m_tileEndDist),
                    (int)s, (int)e, n));
            }
            for (auto& f : futures) f.wait();
        } else {
            precalcTileRange(*m_level, m_tileIsCW, m_tileBPM, preAngleDir, preExtraRot,
                             m_tileStartAngles, m_tileTotalAngles,
                             m_tileDurations, m_tileStartDist, m_tileEndDist,
                             0, workItems, n);
        }
    }

    // Phase 3: Prefix sum
    double totalTime = 0.0;
    for (int i = 0; i < n - 1; i++) {
        m_tileStartTimes[i] = totalTime;
        totalTime += m_tileDurations[i];
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
        int lastIdx = n - 1;
        m_tileDurations[lastIdx] = 0.0f;
        m_tileStartAngles[lastIdx] = (lastIdx > 0) ? m_tileStartAngles[lastIdx - 1] : 0.0f;

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
                m_tileStartDist[lastIdx] = d > 0.01f ? d : 1.0f;
            }
            m_tileEndDist[lastIdx] = m_tileStartDist[lastIdx];
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
                               !m_level->atStates.empty());
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
    }

    auto [minBPM, maxBPM] = std::minmax_element(m_tileBPM.begin(), m_tileBPM.end());
    LOG_D("Timeline: %d tiles, total duration %.2fs, bpm range %.0f-%.0f",
          n - 1, preShiftTotal, *minBPM, *maxBPM);
}

double Timeline::totalDuration() const {
    int n = (int)m_tileStartTimes.size();
    if (n < 2) return 0.0f;
    return m_tileStartTimes[n - 1] + 10.0;
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

std::vector<double> Timeline::getHitsoundTimestamps() const {
    std::vector<double> timestamps;
    int n = (int)m_tileStartTimes.size();
    if (n < 2) return timestamps;
    float offsetSec = m_level->settings.offset / 1000.0f;
    for (int i = 1; i < n; i++)
        timestamps.push_back(m_tileStartTimes[i] + offsetSec);
    return timestamps;
}

std::vector<HitsoundTimestampGroup> Timeline::getHitsoundTimestampGroups() const {
    std::vector<HitsoundTimestampGroup> groups;
    int n = (int)m_tileStartTimes.size();
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
        groups[groupMap[key]].timestamps.push_back(m_tileStartTimes[i] + offsetSec);
    }
    return groups;
}

int Timeline::findTileIndex(double t) const {
    int n = (int)m_tileStartTimes.size();
    if (n < 2) return 0;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_tileStartTimes[mid] <= t) lo = mid + 1;
        else hi = mid - 1;
    }
    int idx = hi;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return idx;
}

}  // namespace adofai
