#pragma once

#include <string>
#include <memory>
#include <functional>

namespace adofai { struct LevelData; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Timeline; }   // 库侧类型：前置声明也要在 adofai 里

// User selections from the launcher
struct LauncherConfig {
    std::string levelPath;
    std::string musicPath;
    // 逐砖颜色（ColorTrack / trackStyle）默认开启；只有显式 `--fill/--stroke` 才整体覆盖
    // （把整条轨道画成同一套颜色 —— 旧行为，保留给"只想看几何"的场合）。
    bool   trackColorOverride = false;
    std::string trackFillColor   = "DEBB7B";  // 6-char hex
    std::string trackStrokeColor = "6F5D3D";  // 6-char hex (DEBB7B * 0.5)
    std::string backgroundColor  = "000000";  // 6-char hex
    bool   autoStroke   = true;
    bool   enableHitsounds = true;
    std::string forceHitsoundType;  // force "None" hitsound to this type (empty = disabled)
    bool   autoPlay = false;       // auto-start playback after loading
    // 播放从这一时刻（关卡时间，秒）开始；>0 时等价于"选砖后按空格"（含音乐 seek）。
    // 诊断/自动化用（CLI `--start-at`）。
    float  startAt = 0.0f;
    int    msaaSamples = 0;        // MSAA samples (0=off, 2, 4, 8)
    // Windows 专用：全屏走“独占”而不是无边框窗口（`--exclusive`）。默认关闭 ——
    // 独占全屏不在 DWM 合成链上，WGC/OBS 抓不到；它换来的是更短的呈现路径
    // （无边框 GL 窗口要 blt 一份到 DWM 重定向表面，独占可以翻页）。要录屏就别开。
    bool   exclusiveFullscreen = false;
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
    // ④ 剔除用的粗块粒度（层/块；0 = 关闭索引、退回全扫）。
    // 更小 → 折叠/绕圈谱的块 AABB 更紧、剔除更快，但索引内存按比例增大
    // （实测 0.0098 B/砖 @4096；2³¹−1 外推 21 MB @4096 → 320 MB @256）。
    // 依据见 docs/rendering.md「④ 剔除」与 tests/cull_cost_probe.cpp --blocks。
    int cullBlockFloors = 4096;
    bool exportHitsounds = false;
    std::string exportDir;         // hitsound export directory (defaults to level dir)
    bool cancelled = false;

    // 开发：确定性抓帧（给"像素 diff"当验收工具）—— 跑到 `--capture-time` 这一个时刻、
    // 固定相机与缩放，渲染一帧、写 PNG，然后退出。这条路**不走** wall clock / 音频 /
    // 帧时间反馈，所以同一组参数两次跑出来应当逐字节相同（AGENTS.md 里那条验收要求）。
    std::string capturePath;       // 空 = 正常进主循环
    float  captureTime = 0.0f;     // 关卡时间（秒）
    int    captureTile = -1;       // ≥0 = 用该砖的起始时刻（谱面再长也不会漂；与 captureTime 二选一）
    float  captureZoom = 100.0f;   // 相机缩放（100 = 视野高 12 单位）
    // true = 抓帧也走**关卡相机**（settings.relativeTo/position + MoveCamera 时间线）。
    // 默认 false：保住既有 51 个像素基线的口径（它们的谱面没有运镜，且 The Moon 的
    // settings.relativeTo=Tile 会让整幅画面平移）。新加的运镜抓帧状态用这个开关 opt-in。
    bool   captureCamera = false;

    // Wizard (5.0.0): result of the "Next" preload step (parse + timeline).
    // Hitsound synthesis + audio are finished after Start in runLevelLoading.
    std::shared_ptr<adofai::LevelData> preloadedLevel;
    std::shared_ptr<adofai::Timeline> preloadedTimeline;
};

// Opens a centered ImGui launcher window. Returns config after user clicks Start or closes.
LauncherConfig showLauncher();
