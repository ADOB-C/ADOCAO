#pragma once

#include <string>
#include <memory>
#include <functional>

class LevelData;
class Timeline;

// User selections from the launcher
struct LauncherConfig {
    std::string levelPath;
    std::string musicPath;
    std::string trackFillColor   = "DEBB7B";  // 6-char hex
    std::string trackStrokeColor = "6F5D3D";  // 6-char hex (DEBB7B * 0.5)
    std::string backgroundColor  = "000000";  // 6-char hex
    bool   autoStroke   = true;
    bool   enableHitsounds = true;
    std::string forceHitsoundType;  // force "None" hitsound to this type (empty = disabled)
    bool   autoPlay = false;       // auto-start playback after loading
    bool   legacyCulling = false;  // use legacy brute-force culling
    int    msaaSamples = 0;        // MSAA samples (0=off, 2, 4, 8)
    bool   exclusiveFullscreen = true; // exclusive fullscreen (vs borderless windowed)
    int  resolutionW = 1920;
    int  resolutionH = 1080;
    bool fullscreen = false;
    bool showTrail = true;
    float trailDuration = 0.4f;    // seconds of trail history
    float trailSampleRate = 200.0f; // samples per second (fixed, or the starting rate when adaptive)
    bool trailAdaptive = true;     // budget-driven adaptive trail sample rate
    float trailTargetFps = 120.0f; // adaptive: keep at least this FPS before raising the rate
    float trailRateMin = 60.0f;    // adaptive lower bound (samples per second)
    float trailRateMax = 4000.0f;  // adaptive upper bound (samples per second)
    bool  trailPerTile = false;    // optional: scale the sample rate with the local tile speed
    float trailSamplesPerTile = 4.0f; // per-tile mode: samples per tile (clamped to rateMin/Max)
    bool  trailLengthInTiles = false; // optional: trail length in tiles instead of seconds
    float trailTiles = 8.0f;       // trail length in tiles (when trailLengthInTiles)
    float hitsoundDrive = 1.0f;    // soft-knee drive; 1.0 == legacy level, no clipping
    bool exportHitsounds = false;
    std::string exportDir;         // hitsound export directory (defaults to level dir)
    bool cancelled = false;

    // Wizard (5.0.0): result of the "Next" preload step (parse + timeline).
    // Hitsound synthesis + audio are finished after Start in runLevelLoading.
    std::shared_ptr<LevelData> preloadedLevel;
    std::shared_ptr<Timeline> preloadedTimeline;
};

// Opens a centered ImGui launcher window. Returns config after user clicks Start or closes.
LauncherConfig showLauncher();
