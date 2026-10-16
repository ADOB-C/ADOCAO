#include "LevelLoader.hpp"

#include "core/util/Logger.hpp"

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



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

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
        if (isRawPcmType(cfg.forceHitsoundType)) {                 // audio-as-chart：直通，不混音
            std::vector<float> pcm; double rate = 0.0;
            if (result.timeline->buildRawPcm(pcm, rate))
                result.hitsounds.preSynthesizeRawPcm(pcm, rate);
            else
                LOG_W("raw-pcm：这张谱没有逐层音量/BPM 不恒定，回退 hitsound 混音");
        }
        if (!result.hitsounds.isSynthesized())
            result.hitsounds.preSynthesize(result.timeline->getHitsoundTimestampGroups(),
                                           result.timeline->totalDuration());
    }

    // Release data no longer needed (angleData, actions, position offsets)
    result.level->releaseMemory();

    // ③ 切片 3：丢掉 Timeline 的两支 TrackVis 密集 double（MYC 原版 103 MB）——
    // 只有"可重算（无 AT 事件）+ 粗索引单调"时才丢（`dropTrackVisArrays()` 自己判定），
    // 否则保留密集数组、行为与以前**完全一样**。
    if (result.timeline) {
        const bool dropStart = result.timeline->dropStartTimesArray();
        LOG_I("timeline: 起始时间密集数组 %s（省 8 B/层）", dropStart ? "已窗口化" : "保留");
        const bool dropped = result.timeline->dropTrackVisArrays();
        LOG_I("timeline: TrackVis 密集数组 %s（%s）", dropped ? "已窗口化" : "保留",
              dropped ? result.timeline->blocksRecomputableWhy() : "不可重算/粗索引不单调");
    }

    report(progress, 1.0f, "Ready!");
}
