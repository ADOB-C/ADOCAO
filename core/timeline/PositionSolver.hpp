#pragma once

#include <glm/glm.hpp>
#include <vector>

class Timeline;

// Pure position solver: turns a Timeline + time into red/blue planet positions
// (or trail sample points). No GL/audio dependency.
class PositionSolver {
public:
    static void positionAt(const Timeline& timeline, double t,
                           glm::dvec2& redOut, glm::dvec2& blueOut);
    static void positionAtTile(const Timeline& timeline, double t, int tileIdx,
                               glm::dvec2& redOut, glm::dvec2& blueOut);

    static void sampleTrail(const Timeline& timeline, double endTime,
                            float trailDuration, float sampleRate,
                            const glm::dvec2& redHead, const glm::dvec2& blueHead,
                            std::vector<glm::dvec2>& redOut,
                            std::vector<glm::dvec2>& blueOut);

    // Track covered per second at time t, in tiles, counting the arc swept by
    // the tile's relative angle as path length. This is the metric the optional
    // per-tile trail sampling scales with: tiles/second alone under-samples a
    // slow tile that sweeps a large angle (180 deg / midspin), while a fixed
    // samples-per-second rate under-samples fast straight runs.
    static double tilePathSpeed(const Timeline& timeline, double t);
};
