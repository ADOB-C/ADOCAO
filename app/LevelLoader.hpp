#pragma once

// Loading is the app's glue between core (parse + timeline), audio (engine +
// hitsound synthesis) and the loading window. Heavy headers are included in
// LevelLoader.cpp only — this header forward-declares everything except the
// by-value audio members of LoadResult.

#include "audio/AudioEngine.hpp"
#include "audio/HitsoundManager.hpp"
#include <memory>

struct LauncherConfig;
struct LoadingProgress;
namespace adofai { struct LevelData; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Timeline; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class PlaybackClock; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class CameraTimeline; }   // 库侧类型：前置声明也要在 adofai 里

struct LoadResult {
    std::shared_ptr<adofai::LevelData> level;
    std::shared_ptr<adofai::Timeline> timeline;
    std::shared_ptr<adofai::PlaybackClock> playback;
    // 运镜时间线（MoveCamera 已展开/已排序）。必须在 `dropStartTimesArray` / `dropTilesArray`
    // / `releaseMemory()` **之前**构建 —— 它依赖逐层起始时间与砖位。建完只剩几十~几百条。
    std::shared_ptr<adofai::CameraTimeline> cameraTimeline;
    adofai::HitsoundManager hitsounds;
    adofai::AudioEngine audio;
};

// Wizard preload step (after "Next"): parse level + precalculate timing.
// Does NOT load audio or synthesize hitsounds (those happen after Start).
void runLevelPreload(const LauncherConfig& cfg, LoadingProgress& progress,
                     std::shared_ptr<adofai::LevelData>& outLevel,
                     std::shared_ptr<adofai::Timeline>& outTimeline);

// Full load used after the launcher. If cfg.preloadedLevel/preloadedTimeline
// are set (wizard preload done), reuses them and only loads audio + synthesizes
// hitsounds; otherwise does the whole load (CLI mode).
void runLevelLoading(const LauncherConfig& cfg, LoadingProgress& progress, LoadResult& result);
