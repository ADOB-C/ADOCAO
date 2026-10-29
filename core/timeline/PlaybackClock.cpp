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

void PlaybackClock::startAtLevelTime(double levelTimeSec) {
    if (!m_timeline || m_isPlaying) return;
    m_isPlaying = true;
    m_elapsedTime = (levelTimeSec + (double)m_timeline->preRoll()) * 1000.0;
    if (m_elapsedTime < 0.0) m_elapsedTime = 0.0;
    m_startWallClock = 0.0;
    m_currentTileIndex = m_timeline->findTileIndex(timeInLevel());
    m_reportedEnd = false;
    updateFrame();
    LOG_D("Playback started at level time %.6fs (tile %d)", levelTimeSec, m_currentTileIndex);
}

void PlaybackClock::stop() {
    if (!m_timeline) return;
    m_isPlaying = false;
    m_elapsedTime = 0.0;
    // **必须重算帧**：以前只把 elapsed 归零，`m_frame` 还停在被停住的那一刻
    // （行星/相机/砖的可见性都来自这一帧）→ 用户看到"退出播放没有回到初始状态"。
    m_currentTileIndex = 0;
    m_reportedEnd = false;
    updateFrame();
    LOG_D("Playback stopped（回到 t=0）");
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
            // 结尾 = 官方的 **Infinite Rotation**（Re_ADOJAS `Player.ts` 的
            // "past the last tile" 分支）：枢轴星停在最后一格**中心**，移动星继续
            // 以半径 1 绕它转，角速度 = 该格的 bpm。别再手写"两颗球都放到格心"——
            // 那会让红蓝完全重合（2026-10-18 用户问的"为什么重叠"就是它）。
            // 时间继续走：结尾的 Flash（2025 是 768 拍白色淡出）和 512 拍 MoveCamera
            // 都排在最后一层，时间不走就永远不播。
            m_frame.timeInLevel = t;
            m_frame.elapsedMs = m_elapsedTime;
            m_currentTileIndex = n - 2;
            m_frame.tileIndex = m_currentTileIndex;
            PositionSolver::positionAt(*m_timeline, t, m_frame.redPosition, m_frame.bluePosition);
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
