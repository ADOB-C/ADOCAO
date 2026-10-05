#pragma once

#include <glm/glm.hpp>
#include <vector>


namespace adofai {

class Timeline;

// Pure position solver: turns a Timeline + time into red/blue planet positions
// (or trail sample points). No GL/audio dependency.
class PositionSolver {
public:
    // How long a trail is and how finely it is sampled.
    //  - length: `duration` seconds (legacy) or `tiles` tiles of track back.
    //    A time window is unusable on charts whose BPM spans orders of magnitude:
    //    0.4s can be tens of thousands of tiles, so the trail sweeps across the
    //    whole map and lands on both sides of the planet.
    //  - rate: `fixedRate` Hz, optionally raised to follow the track covered per
    //    second (steps + arc of the tile's relative angle) so the polyline does
    //    not degenerate into one straight chord at extreme BPM.
    // Sample count is bounded by `maxPoints` in every mode.
    struct TrailSamplingConfig {
        bool  lengthInTiles  = false;
        float duration       = 0.4f;   // seconds (when !lengthInTiles)
        float tiles          = 8.0f;   // tiles back (when lengthInTiles)
        bool  speedAware     = false;  // raise the rate with track covered/second
        float samplesPerTile = 4.0f;
        float fixedRate      = 200.0f; // baseline rate (governor-adjusted)
        float rateMin        = 60.0f;
        float rateMax        = 4000.0f;
        float maxPoints      = 8192.0f;
    };
    struct TrailWindow {
        double startTime = 0.0;
        float  sampleRate = 200.0f;
    };
    static TrailWindow trailWindow(const Timeline& timeline, double t,
                                   const TrailSamplingConfig& cfg);

    static void positionAt(const Timeline& timeline, double t,
                           glm::dvec2& redOut, glm::dvec2& blueOut);
    static void positionAtTile(const Timeline& timeline, double t, int tileIdx,
                               glm::dvec2& redOut, glm::dvec2& blueOut);

    // Sample [startTime, endTime]. The head is bound to the planet position the
    // caller passes in, so the trail can never lead the planet along the track.
    static void sampleTrailRange(const Timeline& timeline, double startTime, double endTime,
                                 float sampleRate,
                                 const glm::dvec2& redHead, const glm::dvec2& blueHead,
                                 std::vector<glm::dvec2>& redOut,
                                 std::vector<glm::dvec2>& blueOut);

    // Convenience: window ending at endTime with the given duration.
    static void sampleTrail(const Timeline& timeline, double endTime,
                            float trailDuration, float sampleRate,
                            const glm::dvec2& redHead, const glm::dvec2& blueHead,
                            std::vector<glm::dvec2>& redOut,
                            std::vector<glm::dvec2>& blueOut);

    // Track covered per second at time t, in tiles, counting the arc swept by
    // the tile's relative angle as path length. This is the metric the optional
    // speed-aware sampling scales with: tiles/second alone under-samples a
    // slow tile that sweeps a large angle (180 deg / midspin), while a fixed
    // samples-per-second rate under-samples fast straight runs.
    static double tilePathSpeed(const Timeline& timeline, double t);
};

}  // namespace adofai
