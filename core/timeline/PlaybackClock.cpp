#include "core/timeline/PlaybackClock.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"

#include <algorithm>
#include "core/util/CountLimits.hpp"


namespace adofai {

void PlaybackClock::start(double wallClockSec) {
    if (!m_timeline || m_isPlaying) return;
    m_isPlaying = true;
    m_elapsedTime = 0.0;
    m_startWallClock = wallClockSec;
    m_currentTileIndex = 0;
    m_reportedEnd = false;

    LOG_D("Playback started at t=%.3fs, countdown=%.1f beats", wallClockSec,
          m_timeline->level()->settings.countdownTicks * (60.0f / m_timeline->level()->settings.bpm));
    updateFrame();
    LOG_D("Playback started");
}

void PlaybackClock::startAt(double wallClockSec, float audioPosSec, float offsetSec) {
    (void)offsetSec;
    if (!m_timeline || m_isPlaying) return;
    m_isPlaying = true;
    m_elapsedTime = ((double)audioPosSec + (double)m_timeline->audioStartOffset()) * 1000.0;
    if (m_elapsedTime < 0.0) m_elapsedTime = 0.0;
    m_startWallClock = wallClockSec - (m_elapsedTime / 1000.0);
    m_currentTileIndex = m_timeline->findTileIndex(timeInLevel());
    m_reportedEnd = false;

    updateFrame();
    LOG_D("Playback started mid-level at tile %d, time=%.3fs", m_currentTileIndex, timeInLevel());
}

void PlaybackClock::stop() {
    if (!m_timeline) return;
    m_isPlaying = false;
    m_elapsedTime = 0.0;
    LOG_D("Playback stopped");
}

double PlaybackClock::timeInLevel() const {
    if (!m_timeline) return 0.0;
    return m_elapsedTime / 1000.0 - (double)m_timeline->preRoll();
}

float PlaybackClock::preRoll() const {
    return m_timeline ? m_timeline->preRoll() : 0.0f;
}

float PlaybackClock::audioStartOffset() const {
    return m_timeline ? m_timeline->audioStartOffset() : 0.0f;
}

void PlaybackClock::update(float deltaMs) {
    if (!m_timeline || !m_isPlaying) return;
    m_elapsedTime += deltaMs;
    updateFrame();
}

void PlaybackClock::updateWallClock(double wallClockSec) {
    if (!m_timeline || !m_isPlaying) return;
    m_elapsedTime = (wallClockSec - m_startWallClock) * 1000.0;
    if (m_elapsedTime < 0.0) m_elapsedTime = 0.0;
    updateFrame();
}

void PlaybackClock::syncToAudio(float audioPosSec, float offsetSec) {
    (void)offsetSec;
    if (!m_timeline || !m_isPlaying) return;
    m_elapsedTime = ((double)audioPosSec + (double)m_timeline->audioStartOffset()) * 1000.0;
    if (m_elapsedTime < 0.0) m_elapsedTime = 0.0;
    updateFrame();
}

void PlaybackClock::updateFrame() {
    if (!m_timeline) return;

    const double t = timeInLevel();
    // #0：int64 算，收窄前显式判
    const int64_t nFull = m_timeline->tileCount();
    if (!fitsInt32(nFull)) {
        LOG_E("PlaybackClock 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    // 结尾定格：到"末尾合成砖"的起点（=最后一格走完）就停住，行星停在最后一格，
    // 不再沿合成砖继续前进（旧行为会让球跑到轨道外面，看起来"最后一格没画出来"）。
    double endTime = 0.0;
    if (n >= 2) {
        endTime = m_timeline->startTimeAt(n - 1);
        if (t >= endTime) {
            // 只定格**球**，不定格**时间**：结尾的 Flash（2025 是 768 拍白色淡出）
            // 和 MoveCamera（512 拍推出）都排在最后一层，按时间推进才播得出来。
            // 时间继续走到音乐结束（syncToAudio 让 t 跟着音频位置），球留在最后一格中心。
            m_frame.timeInLevel = t;
            m_frame.elapsedMs = m_elapsedTime;
            m_currentTileIndex = n - 2;
            m_frame.tileIndex = m_currentTileIndex;
            const auto& p = m_timeline->level()->tilePositionAt(n - 2);
            double dirDeg = m_timeline->level()->tileDirectionAt(n - 2);
            double rad = dirDeg * 3.14159265358979323846 / 180.0;
            // 停在最后一格的**中心**（格子起点 + 半格）
            m_frame.redPosition = glm::dvec2(p[0] + std::cos(rad) * 0.5, p[1] + std::sin(rad) * 0.5);
            m_frame.bluePosition = m_frame.redPosition;
            return;
        }
    }
    m_currentTileIndex = m_timeline->findTileIndex(t);
    m_frame.timeInLevel = t;
    m_frame.elapsedMs = m_elapsedTime;
    m_frame.tileIndex = m_currentTileIndex;

    PositionSolver::positionAt(*m_timeline, t, m_frame.redPosition, m_frame.bluePosition);

    if (!m_reportedEnd && n >= 2 && m_currentTileIndex >= n - 2) {
        m_reportedEnd = true;
        LOG_D("Planet reached end: tileTime=%.3fs", t);
    }
}

}  // namespace adofai
