#include "LevelLoader.hpp"

#include "LauncherWindow.hpp"
#include "LoadingWindow.hpp"
#include "FileMap.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include <cstring>
#include <thread>
#include <future>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

static void report(LoadingProgress& p, float pct, const char* text) {
    p.stage++;
    p.percent.store(pct);
    {
        std::lock_guard<std::mutex> lock(p.textMutex);
        strncpy(p.stageText, text, sizeof(p.stageText) - 1);
        p.stageText[sizeof(p.stageText) - 1] = '\0';
    }
}

void runLevelPreload(const LauncherConfig& cfg, LoadingProgress& progress,
                     std::shared_ptr<LevelData>& outLevel,
                     std::shared_ptr<Timeline>& outTimeline) {
    // Phase 1: parse level
    outLevel = std::make_shared<LevelData>();
    auto onParseProgress = [&](float pct, const char* stage) {
        report(progress, 0.05f + pct * 0.60f, stage);
    };
    FileMap map;
    if (!map.open(cfg.levelPath) ||
        !outLevel->loadFromBuffer(map.data(), map.size(), onParseProgress)) {
        report(progress, 0.0f, "Error: Failed to parse level");
        outLevel.reset();
        return;
    }

    // Phase 2: precalculate timing (tile positions / timeline)
    report(progress, 0.70f, "Precalculating timeline...");
    outTimeline = std::make_shared<Timeline>();
    outTimeline->build(*outLevel, false);
    report(progress, 1.0f, "Preload complete");
}

void runLevelLoading(const LauncherConfig& cfg, LoadingProgress& progress, LoadResult& result) {
    report(progress, 0.02f, "Starting audio engine...");

    // Start audio init + music loading in background immediately (parallel with level parsing)
    std::future<void> audioFuture;
    if (!cfg.musicPath.empty()) {
        audioFuture = std::async(std::launch::async, [&]() {
            result.audio.init();
            result.audio.loadMusic(cfg.musicPath);
        });
    } else {
        result.audio.init();
    }

    if (!cfg.preloadedLevel || !cfg.preloadedTimeline) {
        // Full path (CLI mode): parse level + precalculate timing
        report(progress, 0.05f, "Parsing level...");
        result.level = std::make_shared<LevelData>();
        auto onParseProgress = [&](float pct, const char* stage) {
            report(progress, 0.05f + pct * 0.40f, stage);
        };
        FileMap map;
        if (!map.open(cfg.levelPath) ||
            !result.level->loadFromBuffer(map.data(), map.size(), onParseProgress)) {
            report(progress, 0.0f, "Error: Failed to parse level");
            result.level.reset();
            return;
        }
        report(progress, 0.45f, "Precalculating timeline...");
        result.timeline = std::make_shared<Timeline>();
        result.timeline->build(*result.level, false);
    } else {
        // Wizard preload already parsed level + computed timeline
        result.level    = cfg.preloadedLevel;
        result.timeline = cfg.preloadedTimeline;
        report(progress, 0.50f, "Applying final settings...");
    }

    // PlaybackClock is created after final Timeline settings are known.
    result.playback = std::make_shared<PlaybackClock>();
    result.playback->attachTimeline(result.timeline.get());

    // Final settings (may have been changed in the wizard after the preload)
    result.timeline->setForceHitsoundType(cfg.forceHitsoundType);

    // Wait for background audio init to finish (if started)
    if (audioFuture.valid()) {
        report(progress, 0.75f, "Finalizing audio...");
        audioFuture.get();
    }

    report(progress, 0.80f, "Synthesizing hitsounds...");
    if (cfg.enableHitsounds) {
        result.hitsounds.init();
        result.hitsounds.preSynthesize(result.timeline->getHitsoundTimestampGroups(),
                                       result.timeline->totalDuration());
    }

    // Release data no longer needed (angleData, actions, position offsets)
    result.level->releaseMemory();

    report(progress, 1.0f, "Ready!");
}
