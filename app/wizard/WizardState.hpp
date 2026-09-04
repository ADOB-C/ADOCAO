#pragma once

// Wizard UI state shared across the per-page files in app/wizard/. One .cpp
// per page: each exports a self-contained drawXxxPage(State&, const Chrome&)
// that draws its own content + navigation and mutates State on transitions.

#include "app/LauncherWindow.hpp"   // LauncherConfig
#include "app/LoadingWindow.hpp"    // LoadingProgress
#include <array>
#include <atomic>
#include <string>
#include <thread>

namespace wizard {

struct Chrome;

// Wizard page ids, in display order.
enum class Page : int { Welcome = 0, Preload, Hitsounds, Graphics, Visuals, Music };

// All cross-page UI edit state. User selections are staged in the editor
// buffers below and committed into cfg when leaving the wizard (Start on the
// Music page / Export on the Welcome page). preloadedLevel/preloadedTimeline
// are filled by the preload thread and carried out via cfg.
struct State {
    Page page = Page::Welcome;
    LauncherConfig cfg;
    bool done = false;                 // Start/Export pressed (or window closed)

    // Editor buffers (level/music paths, hex colors)
    char levelBuf[1024] = {};
    char musicBuf[1024] = {};
    char fillBuf[8] = "DEBB7B";
    char strokeBuf[8] = "6F5D3D";
    char bgBuf[8] = "000000";

    // Page option state
    bool autoStroke = true;
    bool enableHitsounds = true;
    bool forceHS = false;
    int  forceHSIdx = 0;               // 0 = Kick
    bool preloadEnabled = true;        // Preload checkbox (welcome page)
    bool showTrail = true;
    float trailDuration = 0.4f;
    float trailSampleRate = 200.0f;
    bool fullscreen = false;
    bool legacyCulling = false;
    int  msaaIdx = 0;                  // 0 = Off
    int  resoIdx = 2;                  // default: 1920x1080

    std::string lastError;
    bool musicAutoTried = false;

    // Preload: background thread spawned by the Welcome page; the wizard loop
    // joins it and advances to Hitsounds when preloadFinished flips.
    LoadingProgress preloadProgress;
    bool preloadRunning = false;
    std::atomic<bool> preloadFinished{false};
    std::thread preloadThread;
};

// Shared option tables (used by page drawing and by the Start commit).
inline constexpr std::array<const char*, 4> kMsaaNames   = {"Off", "2x", "4x", "8x"};
inline constexpr std::array<int, 4>         kMsaaSamples = {0, 2, 4, 8};
inline constexpr std::array<const char*, 5> kResoNames   = {"960x540", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
inline constexpr std::array<int, 5>         kResoW       = {960, 1280, 1920, 2560, 3840};
inline constexpr std::array<int, 5>         kResoH       = {540, 720, 1080, 1440, 2160};
inline constexpr std::array<const char*, 28> kHitsoundTypes = {
    "Kick","KickHouse","KickChroma","KickRupture",
    "Snare","SnareHouse","SnareVapor","Clap","ClapHit","ClapHitEcho",
    "Hat","HatHouse","Chuck","Hammer","Shaker","ShakerLoud",
    "Sidestick","Stick","ReverbClack","ReverbClap","Squareshot",
    "FireTile","IceTile","PowerUp","PowerDown","VehiclePositive",
    "VehicleNegative","Sizzle"
};

// ---- Page entry points (implemented in app/wizard/Page*.cpp) ----
void drawWelcomePage(State& st, const Chrome& ch);
void drawPreloadPage(State& st, const Chrome& ch);
void drawHitsoundsPage(State& st, const Chrome& ch);
void drawGraphicsPage(State& st, const Chrome& ch);
void drawVisualsPage(State& st, const Chrome& ch);
void drawMusicPage(State& st, const Chrome& ch);

} // namespace wizard
