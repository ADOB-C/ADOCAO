#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <limits>
#include "core/util/CountLimits.hpp"
#include "core/util/Env.hpp"
#include <cstdio>


namespace adofai {

void Timeline::build(const LevelData& level, bool exportOnly) {
    m_level = &level;
    m_exportOnly = exportOnly;
    precalculateTiming();

    // Pre-roll = **Re_ADOJAS `getTimeOrigin()`**：把"墙钟/音乐时间"换成"关卡时间"。
    //   separateCountdownTime 关：origin = offset/1000
    //   开：origin = countdownTicks * 60/bpm + offset/1000 − tileStartTimes[1]
    // （tileStartTimes 已在构建时归一化到 tile1 = 0，所以最后一项恒为 0。）
    // **这是 2026-10-21 的老坑**：只按 max(offset, 2 beats) 算 preRoll 时，
    // separateCountdownTime 打开的谱（如 Plum - R，offset 1360ms、4 ticks、180 BPM）
    // 关卡时间会**晚 1.333 s**，与 Re_ADOJAS 的播放/动画时刻整体错位。
    {
        const float secPerBeat = 60.0f / level.settings.bpm;
        const float offsetSec = level.settings.offset / 1000.0f;
        if (level.settings.separateCountdownTime) {
            // 实测锚点：Plum - R 在 Re_ADOJAS 里 elapsed=6200ms 时当前砖 = 14，
            // 而 tile 14 的 chart time = 3.500s → origin = 6200/1000 − 3.500 = 2.700 ≈ 2.693
            // = countdown(4×0.3333) + offset(1.36) − tileStartTimes[1](归一化后为 0)。
            // 换句话说：`getTimeOrigin` 用的是**归一化后**的 tileStartTimes[1]，即 0。
            m_preRoll = level.settings.countdownTicks * secPerBeat + offsetSec - startTimeAt(1);
        } else {
            m_preRoll = offsetSec;
        }
        if (m_preRoll < 0.0f) m_preRoll = 0.0f;
        m_audioStartOffset = m_preRoll - offsetSec;
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
// AnimateTrack 的动画类型 id（appear / disappear 共用一张表；名字来自 ADOFAI 编辑器）
//   1 Extend  2 Assemble  3 Assemble_Far  4 Grow  5 Grow_Spin  6 Fade(appear)
//   7 Fade    8 Drop      9 Rise
//   10 Scatter 11 Scatter_Far 12 Retract 13 Shrink 14 Shrink_Spin
static uint8_t animId(const std::string& s) {
    if (s == "Extend") return 1;
    if (s == "Assemble") return 2;
    if (s == "Assemble_Far" || s == "AssembleFar") return 3;
    if (s == "Grow") return 4;
    if (s == "Grow_Spin" || s == "GrowSpin") return 5;
    if (s == "Fade") return 7;
    if (s == "Drop") return 8;
    if (s == "Rise") return 9;
    if (s == "Scatter") return 10;
    if (s == "Scatter_Far" || s == "ScatterFar") return 11;
    if (s == "Retract") return 12;
    if (s == "Shrink") return 13;
    if (s == "Shrink_Spin" || s == "ShrinkSpin") return 14;
    if (s == "None" || s.empty()) return 0;
    return 0;   // 未知类型按 None（与 Re_ADOJAS 的 switch default 一致）
}
static double easeOutSine(double u) { return std::sin(u * 3.14159265358979323846 * 0.5); }
static double easeOutQuad(double u) { return 1.0 - (1.0 - u) * (1.0 - u); }
// Re_ADOJAS 的 seededRandom：x = sin(seed*12.9898 + seed*78.233) * 43758.5453，取小数部分
static double seededRandom(double seed) {
    const double x = std::sin(seed * 12.9898 + seed * 78.233) * 43758.5453;
    return x - std::floor(x);
}

bool Timeline::animAt(int64_t i, double time, AnimSample& out) const {
    out = AnimSample{};
    if (i < 0 || (size_t)i >= m_animAppearType.size()) return false;
    const size_t k = (size_t)i;
    bool any = false;
    const uint8_t at = m_animAppearType[k], dt = m_animDisappearType[k];
    if (at != 0 && time < m_animAppearEnd[k]) {
        const double t0 = m_animAppearStart[k], t1 = m_animAppearEnd[k];
        const double u = (t1 > t0) ? std::clamp((time - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
        switch (at) {
            case 1: {   // Extend：从上一砖的位置 + 0 缩放长出来
                const double q = easeOutSine(u);
                out.scaleX = out.scaleY = (float)q;
                any = true; break;
            }
            case 2: case 3: {   // Assemble / _Far：随机偏移 + 旋转，SineOut 归位（用增量表示）
                const double range = (at == 3) ? 8.0 : 4.0;
                const double seed = (double)i * 7919.0;
                const double dx = seededRandom(seed) * range * 2 - range;
                const double dy = seededRandom(seed + 1) * range * 2 - range;
                const double dr = (seededRandom(seed + 2) * 150 - 75) * 3.14159265358979323846 / 180.0;
                const double q = 1.0 - easeOutSine(u);
                out.posX = (float)(dx * q); out.posY = (float)(dy * q); out.rot = (float)(dr * q);
                any = true; break;
            }
            case 4: {   // Grow：缩放 0→1（Quad.easeOut）
                const float s = (float)easeOutQuad(u);
                out.scaleX = out.scaleY = s; any = true; break;
            }
            case 5: {   // Grow_Spin：缩放 0→1 + 旋转 −π→0
                const float s = (float)easeOutQuad(u);
                out.scaleX = out.scaleY = s;
                out.rot = (float)(-3.14159265358979323846 * (1.0 - easeOutSine(u)));
                any = true; break;
            }
            case 7: {   // Fade：透明度 0→1（Linear）
                out.alpha = (float)u; any = true; break;
            }
            case 8: case 9: {   // Drop / Rise：位移 ±8 → 0（Linear）+ 前 1/8 时长缩放 0→1
                const double dir = (at == 8) ? 1.0 : -1.0;
                out.posY = (float)(dir * 8.0 * (1.0 - u));
                const float s = (float)std::min(1.0, easeOutQuad(std::min(1.0, u * 8.0)));
                out.scaleX = out.scaleY = s;
                any = true; break;
            }
            default: break;
        }
    }
    if (dt != 0 && time > m_animDisappearStart[k]) {
        const double t0 = m_animDisappearStart[k], t1 = m_animDisappearEnd[k];
        const double u = (t1 > t0) ? std::clamp((time - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
        const double q = easeOutSine(u);
        switch (dt) {
            case 7: out.alpha *= (float)(1.0 - u); any = true; break;                    // Fade
            case 10: case 11: {   // Scatter / _Far：随机飞散
                const double range = (dt == 11) ? 8.0 : 4.0;
                const double seed = (double)i * 3571.0 + 1000.0;
                const double dx = seededRandom(seed) * range * 2 - range;
                const double dy = seededRandom(seed + 1) * range * 2 - range;
                const double dr = (seededRandom(seed + 2) * 150 - 75) * 3.14159265358979323846 / 180.0;
                out.posX += (float)(dx * q); out.posY += (float)(dy * q); out.rot += (float)(dr * q);
                any = true; break;
            }
            case 12: out.scaleX *= (float)(1.0 - q); out.scaleY *= (float)(1.0 - q); any = true; break;  // Retract（位移略）
            case 13: out.scaleX *= (float)(1.0 - q); out.scaleY *= (float)(1.0 - q); any = true; break;  // Shrink
            case 14: out.scaleX *= (float)(1.0 - q); out.scaleY *= (float)(1.0 - q);
                     out.rot += (float)(-3.14159265358979323846 * q); any = true; break;   // Shrink_Spin
            default: break;
        }
    }
    return any;
}

float Timeline::durationAt(int64_t i) const {
    return durationFromTotalAngle(i, totalAngleAt(i));
}

float Timeline::durationFromTotalAngle(int64_t i, float totalAngle) const {
    const int64_t n = (int64_t)m_tileBPM.size();
    if (i >= n - 1) return m_exportOnly ? 0.1f : 0.0f;
    const float rotationAmount = std::abs(totalAngle) / (2.0f * 3.14159265f);
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
    //
    // 绝大多数谱的 action 本来就按 floor 升序写出（实测：Won't You 638 万层 / 578.9 万个
    // in-range 动作 **0 个逆序**，The Moon 31.4 万、Singularity 4.6 万也全序；只有 14 个动作的
    // angles360 有 2 个逆序）。对这些谱 `std::stable_sort` 排的是一段**已经有序**的序列，却仍要
    // O(m log m) 次比较 + 归并缓冲 —— 6.4M 层上实测约 100~140 ms，占 `Timeline::build` 的大头。
    //
    // 这里在**建表时就**记下"是否已序"（零额外遍历：数据本来就在缓存里），已序就跳过排序。
    // 对已序输入，稳定排序的输出与原序列逐位相同，所以结果不变（`floorSorted` 为假时才走原路径）。
    std::vector<std::pair<int, size_t>> flatActions;
    bool floorSorted = true;
    for (size_t j = 0; j < m_level->actions.size(); j++) {
        auto& a = m_level->actions[j];
        if (a.floor >= 0 && a.floor < n) {
            if (!flatActions.empty() && a.floor < flatActions.back().first) floorSorted = false;
            flatActions.push_back({a.floor, j});
        }
    }
    if (!floorSorted) std::stable_sort(flatActions.begin(), flatActions.end());

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
    // 注意：这是**平移前**的赋值 → 恒为 0（等价于"t<0 的倒计时段"阈值）。
    // 平移后 startTimeAt(0) 是负的一砖时长；PositionSolver 用 firstStartTime() 区分
    // "倒计时绕砖 0 转"与"正常沿砖走"，保持 0 是当前行为。
    m_firstStartTime = 0.0;

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
        // **逐条对齐 Re_ADOJAS `TimelineManager.buildAnimateTrackKeyframes`**
        // （src/lib/Player/TimelineManager.ts:1046-1116）：
        //   * appear：事件写了 trackAnimation 才覆盖；`beatsAhead` 缺失沿用上一条
        //   * disappear：事件写了 trackDisappearAnimation 才覆盖；**缺失按 None**（不继承）
        //   * flag2 = **最近一条** AT 事件是否写 trackAnimation（不是"曾经有过"）
        //   * num5/num6 **只在 AT 所在层**推进
        //   * scaledBeats = beats × (tileBPM[floor] / refBPM)，再乘 60/refBPM 得秒
        //   * beatsAhead ≤ 0 时除 Fade 外**跳过 appear**（Assemble/Grow 会闪回）
        m_animAppearType.assign(n, 0);
        m_animDisappearType.assign(n, 0);
        m_animAppearStart.assign(n, 0.0);
        m_animAppearEnd.assign(n, 0.0);
        m_animDisappearStart.assign(n, 0.0);
        m_animDisappearEnd.assign(n, 0.0);
        // 与 Re_ADOJAS 的 GLOBAL_ANIM_TILE_LIMIT 一致：没有逐条 AT、只有全局动画的超大谱
        // 直接跳过动画（否则要按层分配 6 个数组）。
        const bool skipGlobalAnim = (m_level->atStates.empty() && n > 100000);
        std::string curDA = m_level->settings.trackDisappearAnimation;
        std::string curAA = m_level->settings.trackAnimation;
        float curBB = m_level->settings.beatsBehind, curBA = m_level->settings.beatsAhead;
        float num5 = m_tileBPM[0], num6 = m_tileBPM[0];
        bool flag2 = false;
        const double pitch = (m_level->settings.pitch > 0.0f) ? std::max(0.1, (double)m_level->settings.pitch / 100.0) : 1.0;
        uint8_t appearId = animId(curAA), disappearId = animId(curDA);
        for (int i = 0; i < n - 1; i++) {
            bool hadAT = false, floorFlag2 = false;
            auto it = m_level->atStates.find(i);
            if (it != m_level->atStates.end()) {
                auto& st = it->second;
                if (st.hasAA) { curAA = st.aa; appearId = animId(curAA); if (st.ba >= 0.0f) curBA = st.ba; }
                if (st.hasDA) { curDA = st.da; disappearId = animId(curDA); if (st.bb >= 0.0f) curBB = st.bb; }
                floorFlag2 = st.hasAA;
                hadAT = true;
            }
            if (hadAT) { num5 = num6; num6 = m_tileBPM[i]; flag2 = floorFlag2; }
            const float refBPM = flag2 ? num6 : num5;
            const float speed = m_tileBPM[i];
            const float ratio = (refBPM > 0.0f) ? speed / refBPM : 1.0f;
            const float sBA = curBA * ratio, sBB = curBB * ratio;
            const double secPerBeat = 60.0 / (double)(speed > 0.0f ? speed : 100.0f);
            const int64_t pi = (int64_t)i;
            if (disappearId != 0) {
                const double nextEntry = m_tileStartTimes[(size_t)i + 1];
                const double start = nextEntry + (double)sBB * secPerBeat;
                const double dur = std::min((secPerBeat * 0.5) / pitch, 0.5);
                m_tileDisappearTimes[(size_t)i] = start;
                if (!skipGlobalAnim) {
                    m_animDisappearType[(size_t)i] = disappearId;
                    m_animDisappearStart[(size_t)i] = start;
                    m_animDisappearEnd[(size_t)i] = start + dur;
                }
            }
            if (appearId != 0 && (sBA > 0.0f || appearId == 7 /* Fade */)) {
                const bool dropRise = (appearId == 8 || appearId == 9);
                const double tilesLead = (double)sBA * (dropRise ? 2.0 : 1.0);
                const double start = std::max(m_tileStartTimes[(size_t)pi] - tilesLead * secPerBeat, 0.0);
                const double dur = dropRise ? (secPerBeat * (double)sBA) / pitch
                                            : std::min((secPerBeat * 0.5) / pitch, 0.5);
                m_tileAppearTimes[(size_t)pi] = start;
                if (!skipGlobalAnim) {
                    m_animAppearType[(size_t)pi] = appearId;
                    m_animAppearStart[(size_t)pi] = start;
                    m_animAppearEnd[(size_t)pi] = start + dur;
                }
            }
        }
        // 调试：把若干层的 appear/disappear 窗口打出来（对齐 Re_ADOJAS 的逐条核对用）
        if (env::flag("ADOCAO_TRACKVIS_DUMP")) {
            LOG_I("trackvis: startTimes[0..3] = %.4f %.4f %.4f %.4f",
                  m_tileStartTimes[0], m_tileStartTimes[1], m_tileStartTimes[2], m_tileStartTimes[3]);
            // 值 "2" 时额外把整张网格写成 CSV，供 Re_ADOJAS 的 headless harness 用同一套网格
            if (const char* v = std::getenv("ADOCAO_TRACKVIS_DUMP"); v && v[0] == '2') {
                if (FILE* f = std::fopen("build/trackvis_grid.csv", "w")) {
                    std::fprintf(f, "floor,startTime,bpm,posX,posY\n");
                    for (int64_t i = 0; i < (int64_t)n; ++i) {
                        const auto p = m_level->tilePositionAt(i);
                        std::fprintf(f, "%lld,%.17g,%.9g,%.17g,%.17g\n", (long long)i,
                                     m_tileStartTimes[(size_t)i], (double)m_tileBPM[(size_t)i],
                                     (double)p[0], (double)p[1]);
                    }
                    std::fclose(f);
                    LOG_I("trackvis: 网格已写 build/trackvis_grid.csv（%lld 层）", (long long)n);
                }
            }
            int shown = 0;
            for (int i = 0; i < n - 1 && shown < 24; ++i) {
                const bool isAT = m_level->atStates.count(i) != 0;
                const bool hasA = m_animAppearType[(size_t)i] != 0;
                const bool hasD = m_animDisappearType[(size_t)i] != 0;
                if (!isAT && !hasA && !hasD && shown > 0) continue;
                LOG_I("trackvis[%d]%s appear=%u [%.3f,%.3f] disappear=%u [%.3f,%.3f]",
                      i, isAT ? " (AT)" : "",
                      (unsigned)m_animAppearType[(size_t)i], m_animAppearStart[(size_t)i],
                      m_animAppearEnd[(size_t)i],
                      (unsigned)m_animDisappearType[(size_t)i], m_animDisappearStart[(size_t)i],
                      m_animDisappearEnd[(size_t)i]);
                ++shown;
            }
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
        // num5 的推进规则与 Phase 5 逐字相同：i>0 时 num5=bpm[i-1]；i==0 用初值
        float num5;
        if (i == 0) { num5 = m_visNum5; }
        else { num5 = (i - 1 == 0) ? bpm0 : m_tileBPM[(size_t)(i - 1)]; }
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
    // **有消失动画时返回"动画结束"**，不是动画开始：可见性判定是 `disappear <= t → 隐藏`，
    // 用开始时刻会把整段 Scatter/Shrink/Fade*（0.3~0.5 s）直接跳掉 —— 表现就是
    // "该散开的砖根本看不见"（2026-10-21 R=Plum - R 结尾 Scatter_Far）。
    if ((size_t)i < m_animDisappearType.size() && m_animDisappearType[(size_t)i] != 0 &&
        (size_t)i < m_animDisappearEnd.size() && m_animDisappearEnd[(size_t)i] > 0.0) {
        return m_animDisappearEnd[(size_t)i];
    }
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
    // ⚠️ 必须走 `angleCount()`，不能用 `angleData.size()`：后者在 `releaseAngleData()`
    // 之后是**空**的，于是这条夹取被整段跳过 → 末尾那块合成砖被算成真砖（实测 729 → 730，
    // 多排一次打拍音；这个 bug 只在"压缩了角度"的路径上出现，明文加载看不出来）。
    const int64_t ac = m_level ? m_level->angleCount() : 0;
    if (ac > 0 && ac < n) n = (int)ac;
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
    // 每砖 hitsound 覆盖/音量在 `releaseMemory()` 里被释放之后，这里会**静默**退化成
    // "全部用 settings 的默认类型/音量" —— 那看起来完全正常，却是一份错表（ADOCAO-E 拿它
    // 做音频预览会听到错的音色）。所以宁可明确失败：日志 + 空表，调用方必须自己判断。
    if (m_level && m_level->hitsoundCachesReleased) {
        LOG_E("getHitsoundTimestampGroups：每砖 hitsound 覆盖/音量已被 releaseMemory() 释放，"
              "结果不可信 —— 请在释放之前取，或重新加载关卡");
        return groups;
    }
    int n = realTileCount();
    if (n < 2) return groups;

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
    // 块内二分：找"最后一个 startTimeAt(i) <= t"，与原来那次线性扫描**同值**。
    // 前提是块内 startTimes 单调不减（duration 恒 ≥ 0）—— 上面的锚点二分本来就依赖同一前提。
    // 实测 4 张谱（angles360 / The Moon / Singularity / Won't You 638 万层）违例数全为 0；
    // 边界（t 恰好等于某层起始时间、±1 ULP）与"块内一个都不 ≤ t"都保持原语义（后者仍返回 0）。
    //
    // 为什么改：原来最多扫 4096 次，每次都要过 `windowFor()` 的块查找（1 槽线程缓存 + 边界检查），
    // 实测 8.7 µs/次 —— 占了窗口化谱拖尾整次调用的 67~76%。二分后 ~12 次访问器，同一块内
    // 窗口缓存命中，应当降到亚微秒（集成时用 findidx 探针 + 像素门槛复核）。
    int64_t idx = -1;
    int64_t lo2 = from, hi2 = to - 1;
    while (lo2 <= hi2) {
        const int64_t m = lo2 + (hi2 - lo2) / 2;
        if (startTimeAt(m) <= t) { idx = m; lo2 = m + 1; } else hi2 = m - 1;
    }
    if (idx < 0) idx = 0;
    return (int)idx;
}

}  // namespace adofai
