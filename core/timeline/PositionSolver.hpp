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
        float fixedRate      = 800.0f; // baseline rate (governor-adjusted)
        float rateMin        = 60.0f;
        float rateMax        = 4000.0f;
        float maxPoints      = 8192.0f;
        // true = 这个 rate 是**自适应governor**给出的（受 rateMin/rateMax 夹）；
        // false = 用户显式选的固定率（Launcher/CLI），只受 maxPoints 约束 ——
        // 否则"把采样率调到 64000"会被 rateMax 静默夹回 4000，设置等于没生效。
        bool  adaptive       = false;
        // **Sagitta 判据**（像素级"不欠采样"）：允许的折线偏离（px，0 = 关闭）与当前
        // 一砖占多少像素。开启时按 `s = sqrt(8·R·p/pxPerTile)`（砖/样本）反推采样率，
        // R 是该砖的行星轨道半径（砖）。这套判据跟 zoom 走：拉近看曲线才需要更密。
        float sagittaPx      = 0.0f;
        float pixelsPerTile  = 0.0f;
    };
    struct TrailWindow {
        double startTime = 0.0;
        float  sampleRate = 800.0f;
    };
    static TrailWindow trailWindow(const Timeline& timeline, double t,
                                   const TrailSamplingConfig& cfg);

    static void positionAt(const Timeline& timeline, double t,
                           glm::dvec2& redOut, glm::dvec2& blueOut);
    static void positionAtTile(const Timeline& timeline, double t, int tileIdx,
                               glm::dvec2& redOut, glm::dvec2& blueOut);
    // ── MultiPlanet（N 颗星）─────────────────────────────────────────────
    // 参考实现 Re_ADOJAS `Player.applyMultiPlanetLayout`（官方 scrPlanet.UpdatePlanet 稳态）：
    // 枢轴星在砖心，移动星在半径 r 处，其余星落在正 N 边形上（边长 = r）。
    // N<=2 时逐位退化成 positionAtTile 的结果（保证既有像素门槛不变）。
    struct PlanetPos { uint8_t id; glm::dvec2 pos; };
    static void planetsAt(const Timeline& timeline, double t, std::vector<PlanetPos>& out);

    // Sample [startTime, endTime]. The head is bound to the planet position the
    // caller passes in, so the trail can never lead the planet along the track.
    // 关卡结束后也照常采样：positionAtTile 在末尾之外走的是"绕枢轴星的轨道角"
    // （Infinite Rotation），不是外推直线。
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
    // 行星在 t 时刻绕的半径（砖）：max(tileStartDist, tileEndDist)。sagitta 判据用它当曲率半径。
    static double curvatureRadius(const Timeline& timeline, double t);
};

}  // namespace adofai
