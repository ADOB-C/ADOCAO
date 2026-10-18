#include "LevelLoader.hpp"

#include "core/util/Logger.hpp"
#include "core/util/Scheduler.hpp"
#include <cstdlib>   // getenv（ADOCAO_FLOOR_WINDOW）

#include "LauncherWindow.hpp"
#include "LoadingWindow.hpp"
#include "FileMap.hpp"
#include "core/level/LevelData.hpp"
#include "core/level/FloorWindow.hpp"   // kCheckpointStride（③ 切片 4 日志用）
#include "core/timeline/Timeline.hpp"
#include "core/camera/CameraTimeline.hpp"
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
    // ④ 后台车道：音频解码走 `Scheduler`（Background），不再自起 `std::async` ✗。
    // 任务体只碰 `result`（调用方持有、整个加载期都活着 ✓）→ 生命周期安全 ✓。
    Scheduler::Handle audioJob;
    if (!cfg.musicPath.empty()) {
        audioJob = Scheduler::instance().submit(Lane::Background, [&](Scheduler::CancelToken) {
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

    // 运镜时间线：必须在下面任何"丢数组"之前建好（它要逐层起始时间与砖位）。
    // 谱面没有 MoveCamera 时这是一次空构建（零分配），有的话也只是几十~几百条。
    result.cameraTimeline = std::make_shared<CameraTimeline>();
    result.cameraTimeline->build(*result.level, result.timeline->tileStartTimes(),
                                 result.timeline->tileBPMs());

    // RecolorTrack 的运行时索引（范围 + 触发时间）：必须在丢起始时间数组之前建。
    result.level->buildRecolorIndex(*result.timeline);
    result.level->buildFlashIndex(*result.timeline);
    result.level->buildMoveTrackIndex(*result.timeline);

    // PlaybackClock is created after final Timeline settings are known.
    result.playback = std::make_shared<PlaybackClock>();
    result.playback->attachTimeline(result.timeline.get());

    // Final settings (may have been changed in the wizard after the preload)
    result.timeline->setForceHitsoundType(cfg.forceHitsoundType);

    // Wait for background audio init to finish (if started)
    if (audioJob.valid()) {
        report(progress, 0.75f, "Finalizing audio...");
        audioJob.wait();
    }

    report(progress, 0.80f, "Synthesizing hitsounds...");
    if (cfg.enableHitsounds) {
        // 音色缓冲必须按**设备率**合成：AudioEngine 的回调是 1:1 拷贝（不做重采样）。
        // 设备拿不到 48k 时（本机实测 44100）不这么做，音色会比音乐快/慢 设备率/48000 倍。
        result.hitsounds.setOutputSampleRate(result.audio.deviceSampleRate());
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

    // ③ 切片 4：建"按块物化"缓存并**丢掉密集 `tiles`**（MYC 24 B/层 = 162 MB）。
    // 必须在 `releaseMemory()` **之前**（它按 `tilesWindowed()` 决定是否保留稀疏的
    // `tilePositionOffsets` —— provider 物化块要用它合成 PositionTrack），
    // 也必须在 Timeline 丢掉起始时间数组之前（checkpoint 要用它）。
    // ③ 切片 4：`tiles` 按块物化 **默认开**（2026-10-15 起）。
    // 关掉做 A/B：`ADOCAO_FLOOR_WINDOW=0`。
    // 历史上这里曾"默认关"过一次，原因有三处真 bug（`TileShape::keyForTile` 直接读
    // `lv.tiles[i].direction` 越界崩、`tileCount()` 丢完归零、`releaseMeshTemporaries`（已删）清
    // `tileBPMs`）+ 一处**先在 bug**（它无条件释放 `angleData`，而 `totalAngleAt()` 每帧要读它
    // → 行星沿错误的弧转）。四处都修好后，provider 开/关两种模式的像素门槛都 51/51 逐字节。
    const char* fw = std::getenv("ADOCAO_FLOOR_WINDOW");
    const bool windowTiles = !fw || fw[0] != '0';
    if (!windowTiles) {
        LOG_I("floor-window: 关闭（ADOCAO_FLOOR_WINDOW=0，A/B 用）→ 保留密集 tiles");
    } else if (result.level && result.timeline && result.level->hasScaleRadiusEvents()) {
        // 窗口重算没有 ScaleRadius 的半径偏移 → 丢密集 tiles 会让结尾位置整体漂移
        // （Won't You：40 条 ScaleRadius，t=206.3 时相机偏了 778 格、整段结尾黑屏）。
        LOG_I("floor-window: 谱面有 ScaleRadius，保留密集 tiles（窗口重算会丢半径偏移）");
    } else if (result.level && result.timeline) {
        std::string werr;
        if (result.level->buildFloorWindowCache(result.timeline->tileStartTimes(),
                                                result.timeline->tileBPMs(),
                                                result.timeline->tileIsCW(), &werr)) {
            const bool dropped = result.level->dropTilesArray();
            LOG_I("floor-window: 缓存已建（每 %u 层一个 checkpoint）；密集 tiles %s",
                  (unsigned)adofai::kCheckpointStride, dropped ? "已丢弃（按块物化）" : "保留");
        } else {
            LOG_I("floor-window: 未接（%s）→ 保留密集 tiles", werr.c_str());
        }
    }

    // Release data no longer needed (angleData, actions, position offsets)
    // ③ 切片 4：`angleData` → 位精确紧凑存储（运行期 `angleAt()` 读它）。
    // 必须在这之前完成：`computeCheckpoints`（上面建缓存时）与 `Timeline::build`（更早）都读原数组。
    // `TileMesh::build` 之后不需要原数组了（它走 `tileDirectionAt` 与 `keyForTile`，后者已改走 angleAt）。
    if (result.level) {
        std::string aerr;
        if (result.level->releaseAngleData(&aerr)) {
            LOG_I("angle-store: %lld 个取值 → %.2f B/层（%.1f MB → %.1f MB）",
                  (long long)result.level->angleStoreUnique(),
                  (double)result.level->angleStoreBytes() / (double)std::max<int64_t>(1, result.level->angleCount()),
                  (double)result.level->angleCount() * 8.0 / 1048576.0,
                  (double)result.level->angleStoreBytes() / 1048576.0);
        } else {
            LOG_I("angle-store: 未启用（%s）→ 保留 angleData", aerr.c_str());
        }
    }

    result.level->releaseMemory();

    // ③ 切片 3：丢掉 Timeline 的两支 TrackVis 密集 double（MYC 原版 103 MB）——
    // 只有"可重算（无 AT 事件）+ 粗索引单调"时才丢（`dropTrackVisArrays()` 自己判定），
    // 否则保留密集数组、行为与以前**完全一样**。
    if (result.timeline) {
        // ScaleRadius 的偏移是"按入口方向的前缀和"，窗口重算的 checkpoint 里没有它 →
        // 有 ScaleRadius 时保留密集数组（窗口化会丢半径偏移）。
        const bool hasRadius = result.level->hasScaleRadiusEvents();
        const bool dropStart = hasRadius ? false : result.timeline->dropStartTimesArray();
        LOG_I("timeline: 起始时间密集数组 %s（%s）", dropStart ? "已窗口化" : "保留",
              hasRadius ? "ScaleRadius 需要密集位置" : (dropStart ? "省 8 B/层" : "不可重算"));
        const bool dropped = result.timeline->dropTrackVisArrays();
        LOG_I("timeline: TrackVis 密集数组 %s（%s）", dropped ? "已窗口化" : "保留",
              dropped ? result.timeline->blocksRecomputableWhy() : "不可重算/粗索引不单调");
    }

    report(progress, 1.0f, "Ready!");
}
