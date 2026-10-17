#pragma once
#include "core/util/Scheduler.hpp"

// Wizard UI state shared across the per-page files in app/wizard/. One .cpp
// per page: each exports a self-contained drawXxxPage(State&, const Chrome&)
// that draws its own content + navigation and mutates State on transitions.

#include "app/LauncherWindow.hpp"   // LauncherConfig
#include "app/LoadingWindow.hpp"    // LoadingProgress
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace wizard {

struct Chrome;

// ---- Color conversion (wizard <-> config) ----
// The wizard edits colors as float RGB so ImGui's color widgets can drive them
// directly, while LauncherConfig / the CLI keep their "#RRGGBB"-style 6-char
// hex contract. These helpers are the only crossing point.
inline std::array<float, 3> rgbFromHex(const std::string& hex) {
    unsigned v = 0;
    if (hex.size() >= 6) std::sscanf(hex.c_str(), "%06x", &v);
    return { (float)((v >> 16) & 0xFFu) / 255.0f,
             (float)((v >>  8) & 0xFFu) / 255.0f,
             (float)( v        & 0xFFu) / 255.0f };
}

inline std::string hexFromRgb(const float rgb[3]) {
    auto byte = [](float c) {
        int v = (int)(c * 255.0f + 0.5f);
        return (unsigned)(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    char out[7];
    std::snprintf(out, sizeof(out), "%02X%02X%02X", byte(rgb[0]), byte(rgb[1]), byte(rgb[2]));
    return out;
}

// Automatic stroke has always been "fill * 0.5" (--no-auto-stroke turns it off).
// The halving is done in the byte domain exactly like the old hex code did, so
// odd channels truncate (DEBB7B -> 6F5D3D) instead of rounding up.
inline std::array<float, 3> deriveStroke(const float fill[3]) {
    auto half = [](float c) {
        int v = (int)(c * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (float)(v / 2) / 255.0f;
    };
    return { half(fill[0]), half(fill[1]), half(fill[2]) };
}

// 剔除粗块粒度（Graphics 页的选项；0 = 关闭索引、退回全扫）。
// 值与名字放这里，好让 PageGraphics（画）与 PageMusic（提交进 cfg）共用一份。
// 名字沿用本页 MSAA 那种短写法；内存代价见 docs/rendering.md「④ 剔除」：
// 2³¹−1 外推 256→320 MB、1k→80 MB、4k→21 MB（默认）、16k→5 MB。
// 实测最优：MYC（局部稀疏）4k；Singularity 这种**折返**谱 256（4k 也才 0.12 ms，帧预算 0.7%）。
inline constexpr std::array<const char*, 8> kCullBlockNames = {
    "Off", "256", "512", "1k", "2k", "4k", "8k", "16k"
};
inline constexpr std::array<int, 8> kCullBlockValues = {0, 256, 512, 1024, 2048, 4096, 8192, 16384};
inline constexpr int kCullBlockDefaultIdx = 5;   // 4k = 4096
static_assert(kCullBlockNames.size() == kCullBlockValues.size(), "剔除块粒度：名字与值必须一一对应");
static_assert(kCullBlockDefaultIdx < (int)kCullBlockValues.size(), "剔除块粒度：默认下标越界");
static_assert(kCullBlockValues[kCullBlockDefaultIdx] == 4096, "剔除块粒度：默认值应当是 4096");

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

    // Editor buffers (level/music paths). Colors are float RGB for
    // ImGui::ColorEdit3, seeded from the hex defaults in cfg so the two stay in
    // sync (and will pick up CLI-provided colors once the config is shared).
    char levelBuf[1024] = {};
    char musicBuf[1024] = {};
    std::array<float, 3> fillColor   = rgbFromHex(cfg.trackFillColor);
    std::array<float, 3> strokeColor = rgbFromHex(cfg.trackStrokeColor);
    std::array<float, 3> bgColor     = rgbFromHex(cfg.backgroundColor);

    // Page option state
    bool autoStroke = true;
    bool enableHitsounds = true;
    bool forceHS = false;
    int  forceHSIdx = 0;               // 0 = Kick
    bool preloadEnabled = true;        // Preload checkbox (welcome page)
    bool showTrail = true;
    float trailDuration = 0.4f;
    float trailSampleRate = 200.0f;
    bool  trailPerTile = false;        // optional speed-aware sampling
    float trailSamplesPerTile = 4.0f;
    bool  trailLengthInTiles = false;  // optional: length in tiles instead of seconds
    float trailTiles = 8.0f;
    bool fullscreen = false;
    int  msaaIdx = 0;                  // 0 = Off
    int  resoIdx = 2;                  // default: 1920x1080
    int  cullBlockIdx = kCullBlockDefaultIdx;   // 剔除粗块粒度（Graphics 页）

    std::string lastError;
    bool musicAutoTried = false;

    // Preload: background thread spawned by the Welcome page; the wizard loop
    // joins it and advances to Hitsounds when preloadFinished flips.
    LoadingProgress preloadProgress;
    bool preloadRunning = false;
    std::atomic<bool> preloadFinished{false};
    adofai::Scheduler::Handle preloadThread;   // ①/④：向导预载的句柄（原来裸线程 ✗）
};

// Shared option tables (used by page drawing and by the Start commit).
inline constexpr std::array<const char*, 4> kMsaaNames   = {"Off", "2x", "4x", "8x"};
inline constexpr std::array<int, 4>         kMsaaSamples = {0, 2, 4, 8};
inline constexpr std::array<const char*, 5> kResoNames   = {"960x540", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
inline constexpr std::array<int, 5>         kResoW       = {960, 1280, 1920, 2560, 3840};
inline constexpr std::array<int, 5>         kResoH       = {540, 720, 1080, 1440, 2160};
// 打拍音类型表在 app/HitsoundTypes.hpp（CLI 与向导共用一份，带编译期元素数守卫）


// ---- Page entry points (implemented in app/wizard/Page*.cpp) ----
void drawWelcomePage(State& st, const Chrome& ch);
void drawPreloadPage(State& st, const Chrome& ch);
void drawHitsoundsPage(State& st, const Chrome& ch);
void drawGraphicsPage(State& st, const Chrome& ch);
void drawVisualsPage(State& st, const Chrome& ch);
void drawMusicPage(State& st, const Chrome& ch);

} // namespace wizard
