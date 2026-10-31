#pragma once

#include <glm/glm.hpp>


namespace adofai {

class Timeline;
class TileAnimation;

struct PlaybackFrame {
    double timeInLevel = 0.0;
    double elapsedMs = 0.0;
    int    tileIndex = 0;
    glm::dvec2 redPosition{0.0};
    glm::dvec2 bluePosition{0.0};
};

// Runtime playback state machine. Owns no GL objects; each mutation updates a
// pure data frame which the caller can apply to render-layer objects.
class PlaybackClock {
public:
    PlaybackClock() = default;

    void attachTimeline(const Timeline* timeline) { m_timeline = timeline; }
    // 轨道动画（MoveTrack/AnimateTrack）。给了之后行星就按"粘性轨道"跟随被搬走的砖
    // （见 PositionSolver 与 docs/level-loading.md「跟随 MoveTrack 动画」）。
    // 不持有它：生命周期由渲染侧（LevelScene）管。
    void setTileAnimation(const TileAnimation* anim) { m_tileAnim = anim; }

    void start(double wallClockSec);
    void startAt(double wallClockSec, float audioPosSec, float offsetSec);
    // 直接用**关卡时间**定位（double 计算，避开 audioPos 的 float 精度：
    // 6.8e7 砖的谱面上 float 只有 ~1e-4 s，按 audioPos 定位会漂到隔壁砖）。
    void startAtLevelTime(double levelTimeSec);
    void stop();
    void update(float deltaMs);
    void updateWallClock(double wallClockSec);
    void syncToAudio(float audioPosSec, float offsetSec);

    bool isPlaying() const { return m_isPlaying; }
    float elapsedTimeMs() const { return (float)m_elapsedTime; }
    double timeInLevel() const;
    int currentTileIndex() const { return m_currentTileIndex; }
    float preRoll() const;
    float audioStartOffset() const;
    const PlaybackFrame& frame() const { return m_frame; }

private:
    const TileAnimation* m_tileAnim = nullptr;
    const Timeline* m_timeline = nullptr;
    bool m_isPlaying = false;
    double m_elapsedTime = 0.0;
    double m_startWallClock = 0.0;
    int m_currentTileIndex = 0;
    bool m_reportedEnd = false;
    PlaybackFrame m_frame;

    void updateFrame();
};

}  // namespace adofai
