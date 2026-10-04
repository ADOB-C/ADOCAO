#include "app/HitsoundTypes.hpp"
#include "core/level/LevelPath.hpp"
#include "Application.hpp"
#include "app/MapExport.hpp"
#include "LauncherWindow.hpp"
#include "core/util/Logger.hpp"
#include <cstring>

int main(int argc, char* argv[]) {
    bool debug = false;
    std::string mapOut, mapSize, mapBg, mapTiles, mapNativeAll, mapStitchDir, mapStitchOut;
    bool mapNative = false;
    int mapBlock = 4096, mapThreads = 0, stitchScale = 1;
    LauncherConfig cli;

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
             if (strcmp(argv[i], "--debug") == 0)          debug = true;
        else if (strcmp(argv[i], "--level") == 0     && i+1<argc) cli.levelPath = resolveLevelPath(argv[++i]);
        else if (strcmp(argv[i], "--music") == 0     && i+1<argc) cli.musicPath = argv[++i];
        else if (strcmp(argv[i], "--width") == 0     && i+1<argc) cli.resolutionW = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0    && i+1<argc) cli.resolutionH = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fullscreen") == 0)           cli.fullscreen = true;
        else if (strcmp(argv[i], "--fill") == 0       && i+1<argc) cli.trackFillColor = argv[++i];
        else if (strcmp(argv[i], "--stroke") == 0     && i+1<argc) cli.trackStrokeColor = argv[++i];
        else if (strcmp(argv[i], "--bg") == 0         && i+1<argc) cli.backgroundColor = argv[++i];
        else if (strcmp(argv[i], "--no-auto-stroke") == 0)        cli.autoStroke = false;
        else if (strcmp(argv[i], "--no-hitsound") == 0)           cli.enableHitsounds = false;
        else if (strcmp(argv[i], "--force-hitsound") == 0) {
            if (i+1 < argc && argv[i+1][0] != '-') {
                cli.forceHitsoundType = argv[++i];
                // Validate type
                // 类型表与向导共用（app/HitsoundTypes.hpp），避免两份清单漂移

                bool ok = false;
                for (const char* t : kHitsoundTypes) if (cli.forceHitsoundType == t) { ok = true; break; }
                if (!ok) { LOG_W("Unknown hitsound type '%s', defaulting to Kick", cli.forceHitsoundType.c_str()); cli.forceHitsoundType = "Kick"; }
            } else {
                cli.forceHitsoundType = "Kick";
            }
        }
        else if (strcmp(argv[i], "--auto-play") == 0)           cli.autoPlay = true;
        else if (strcmp(argv[i], "--legacy-culling") == 0)   cli.legacyCulling = true;
        else if (strcmp(argv[i], "--msaa") == 0 && i+1<argc)   cli.msaaSamples = atoi(argv[++i]);
        else if (strcmp(argv[i], "--exclusive") == 0)         cli.exclusiveFullscreen = true;
        else if (strcmp(argv[i], "--no-exclusive") == 0)   cli.exclusiveFullscreen = false;
        else if (strcmp(argv[i], "--no-trail") == 0)              cli.showTrail = false;
        else if (strcmp(argv[i], "--trail-duration") == 0 && i+1<argc) cli.trailDuration = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--trail-sample-rate") == 0 && i+1<argc) { cli.trailSampleRate = (float)atof(argv[++i]); cli.trailAdaptive = false; } // explicit = fixed
        else if (strcmp(argv[i], "--trail-target-fps") == 0 && i+1<argc) { cli.trailTargetFps = (float)atof(argv[++i]); cli.trailAdaptive = true; }
        else if (strcmp(argv[i], "--trail-rate-min") == 0 && i+1<argc) cli.trailRateMin = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--trail-rate-max") == 0 && i+1<argc) cli.trailRateMax = (float)atof(argv[++i]);
        // Optional: scale the sample rate with the track covered per second
        // (steps + arc of the tile's relative angle) instead of a fixed Hz rate.
        else if (strcmp(argv[i], "--trail-samples-per-tile") == 0 && i+1<argc) { cli.trailSamplesPerTile = (float)atof(argv[++i]); cli.trailPerTile = true; }
        // Optional: measure the trail in tiles instead of seconds (a 0.4s window
        // is tens of thousands of tiles on charts that speed up by 1000x).
        else if (strcmp(argv[i], "--trail-tiles") == 0 && i+1<argc) { cli.trailTiles = (float)atof(argv[++i]); cli.trailLengthInTiles = true; }
        else if (strcmp(argv[i], "--export") == 0)                cli.exportHitsounds = true;
        // 地图全景（真无头）：--map out.png [--map-size WxH] [--map-bg HEX|transparent]
        else if (strcmp(argv[i], "--map") == 0 && i+1<argc)       mapOut = argv[++i];
        else if (strcmp(argv[i], "--map-size") == 0 && i+1<argc)  mapSize = argv[++i];
        else if (strcmp(argv[i], "--map-bg") == 0 && i+1<argc)    mapBg = argv[++i];
        else if (strcmp(argv[i], "--map-tiles") == 0 && i+1<argc) mapTiles = argv[++i];
        else if (strcmp(argv[i], "--map-native") == 0)            mapNative = true;
        else if (strcmp(argv[i], "--map-native-all") == 0 && i+1<argc) mapNativeAll = argv[++i];
        else if (strcmp(argv[i], "--map-stitch") == 0 && i+1<argc) { mapStitchDir = argv[++i];
            if (i+1<argc && argv[i+1][0] != '-') mapStitchOut = argv[++i]; }
        else if (strcmp(argv[i], "--stitch-scale") == 0 && i+1<argc) stitchScale = atoi(argv[++i]);
        else if (strcmp(argv[i], "--map-block") == 0 && i+1<argc) mapBlock = atoi(argv[++i]);
        else if (strcmp(argv[i], "--map-threads") == 0 && i+1<argc) mapThreads = atoi(argv[++i]);
    }

    if (!mapStitchDir.empty()) {
        if (mapStitchOut.empty()) { std::fprintf(stderr, "--map-stitch 需要 <目录> <输出.png>\n"); return 1; }
        return stitchLevelMapTiles(mapStitchDir, mapStitchOut, stitchScale);
    }
    if (!mapNativeAll.empty()) {
        if (cli.levelPath.empty()) { std::fprintf(stderr, "--map-native-all 需要 --level\n"); return 1; }
        return exportLevelMapNativeAll(cli.levelPath, mapNativeAll, mapTiles, mapBlock, mapThreads);
    }
    if (!mapOut.empty()) {
        if (cli.levelPath.empty()) { std::fprintf(stderr, "--map 需要 --level\n"); return 1; }
        return exportLevelMap(cli.levelPath, mapOut, mapSize, mapBg, mapTiles, mapNative);   // 不初始化 GL，直接出图
    }

    if (cli.exportHitsounds && !cli.levelPath.empty()) {
        return runApplicationFromCLI(cli, debug);
    }
    if (!cli.levelPath.empty()) {
        return runApplicationFromCLI(cli, debug);
    }
    return runApplication(debug);
}
