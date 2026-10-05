#include "core/timeline/PlaybackClock.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/util/Logger.hpp"

#include <algorithm>


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
    const int n = (int)m_timeline->tileStartTimes().size();
    m_currentTileIndex = m_timeline->findTileIndex(t);
    m_frame.timeInLevel = t;
    m_frame.elapsedMs = m_elapsedTime;
    m_frame.tileIndex = m_currentTileIndex;

    PositionSolver::positionAt(*m_timeline, t, m_frame.redPosition, m_frame.bluePosition);

    if (!m_reportedEnd && n >= 2 && m_currentTileIndex >= n - 1) {
        m_reportedEnd = true;
        LOG_D("Planet reached end: tileTime=%.3fs", t);
    }
}

}  // namespace adofai
