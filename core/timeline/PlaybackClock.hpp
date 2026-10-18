#pragma once

#include <glm/glm.hpp>


namespace adofai {

class Timeline;

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

    void start(double wallClockSec);
    void startAt(double wallClockSec, float audioPosSec, float offsetSec);
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
