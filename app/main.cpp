#include "Application.hpp"
#include "LauncherWindow.hpp"
#include "core/util/Logger.hpp"
#include <cstring>

int main(int argc, char* argv[]) {
    bool debug = false;
    LauncherConfig cli;

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
             if (strcmp(argv[i], "--debug") == 0)          debug = true;
        else if (strcmp(argv[i], "--level") == 0     && i+1<argc) cli.levelPath = argv[++i];
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
                static const char* valid[] = {"Kick","KickHouse","KickChroma","KickRupture",
                    "Snare","SnareHouse","SnareVapor","Clap","ClapHit","ClapHitEcho",
                    "Hat","HatHouse","Chuck","Hammer","Shaker","ShakerLoud",
                    "Sidestick","Stick","ReverbClack","ReverbClap","Squareshot",
                    "FireTile","IceTile","PowerUp","PowerDown","VehiclePositive",
                    "VehicleNegative","Sizzle",nullptr};
                bool ok = false;
                for (int j = 0; valid[j]; j++) if (cli.forceHitsoundType == valid[j]) { ok = true; break; }
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
        // Hitsound loudness: tanh soft-limiter drive (1 = linear, no limiting).
        else if (strcmp(argv[i], "--hitsound-drive") == 0 && i+1<argc) cli.hitsoundDrive = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--export") == 0)                cli.exportHitsounds = true;
    }

    if (cli.exportHitsounds && !cli.levelPath.empty()) {
        return runApplicationFromCLI(cli, debug);
    }
    if (!cli.levelPath.empty()) {
        return runApplicationFromCLI(cli, debug);
    }
    return runApplication(debug);
}
