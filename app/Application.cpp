#include "Application.hpp"
#include "LauncherWindow.hpp"
#include "LoadingWindow.hpp"
#include "LevelLoader.hpp"
#include "GameWindow.hpp"
#include "audio/HitsoundManager.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/util/Logger.hpp"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>

// Pin current thread to a performance core (big.LITTLE aware).
// Uses GetSystemCpuSetInformation via dynamic load for MinGW compat.
typedef BOOL (WINAPI *PGSCSI)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);

static void pinToBigCore() {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return;
    auto pfn = (PGSCSI)GetProcAddress(k, "GetSystemCpuSetInformation");
    if (!pfn) return;

    ULONG len = 0;
    pfn(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (len == 0) return;

    auto* sets = (SYSTEM_CPU_SET_INFORMATION*)malloc(len);
    if (!sets) return;
    if (!pfn(sets, len, &len, GetCurrentProcess(), 0)) {
        free(sets); return;
    }

    DWORD_PTR mask = 0;
    for (ULONG off = 0; off * sizeof(*sets) < len; ) {
        auto& s = sets[off];
        if (s.Type == 0 && s.CpuSet.EfficiencyClass == 1)  // perf core
            mask |= (DWORD_PTR)1 << s.CpuSet.Id;
        off += s.Size;
    }
    free(sets);

    if (mask)
        SetThreadAffinityMask(GetCurrentThread(), mask);
}

static void enableDPIAwareness() {
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        auto SetProcessDpiAwareness = (HRESULT(WINAPI*)(int))
            GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (SetProcessDpiAwareness) SetProcessDpiAwareness(2); // PerMonitor
        FreeLibrary(shcore);
    } else {
        HMODULE user32 = LoadLibraryA("user32.dll");
        if (user32) {
            auto SetProcessDPIAware = (BOOL(WINAPI*)())
                GetProcAddress(user32, "SetProcessDPIAware");
            if (SetProcessDPIAware) SetProcessDPIAware();
            FreeLibrary(user32);
        }
    }
}
#endif

static bool s_firstEarlyLog = true;

static void earlyLog(const char* msg) {
    // First call truncates old log, subsequent calls append
    FILE* f = fopen("ADOCAO.log", s_firstEarlyLog ? "w" : "a");
    s_firstEarlyLog = false;
    if (f) { fprintf(f, "[EARLY] %s\n", msg); fclose(f); }
#ifdef _WIN32
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#endif
}

int runApplication(bool debugConsole) {
    earlyLog("[ADOCAO] main() entered");

    // Determine log path next to executable
    std::string logPath = "ADOCAO.log";
    Logger::instance().init(logPath, debugConsole);

    LOG_I("ADOCAO starting...");

#ifdef _WIN32
    earlyLog("[ADOCAO] DPI awareness...");
    enableDPIAwareness();
    earlyLog("[ADOCAO] CPU pin...");
    pinToBigCore();
#endif

    earlyLog("[ADOCAO] GLFW init...");
    if (!glfwInit()) {
        LOG_E("Failed to initialize GLFW");
#ifdef _WIN32
        MessageBoxA(nullptr, "Failed to initialize OpenGL.\nPlease update your graphics driver.",
                    "ADOCAO - Error", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }

    // Stage 1: Launcher
    LauncherConfig cfg = showLauncher();
    if (debugConsole) cfg.enableHitsounds = false;
    if (cfg.cancelled || cfg.levelPath.empty()) {
        LOG_I("Launcher cancelled, exiting.");
        glfwTerminate();
        return 0;
    }
    LOG_D("Launcher: level=%s, music=%s, resolution=%dx%d, fullscreen=%d",
          cfg.levelPath.c_str(), cfg.musicPath.c_str(),
          cfg.resolutionW, cfg.resolutionH, cfg.fullscreen);

    // Export hitsounds to WAV and exit (no game window)
    if (cfg.exportHitsounds) {
        LevelData lvl;
        if (!lvl.loadFromFile(cfg.levelPath, nullptr, true)) {
            LOG_E("Failed to load level for export");
            glfwTerminate();
            return 1;
        }
        Timeline timeline;
        timeline.build(lvl, true);
        HitsoundManager hm;
        hm.init();
        if (!hm.preSynthesize(timeline.getHitsoundTimestampGroups(), timeline.totalDuration())) {
            LOG_E("Export: pre-synthesis failed");
            glfwTerminate();
            return 1;
        }
        // Output: <exportDir>/<levelname>_hitsounds.wav (default: level dir)
        std::string outPath;
        if (!cfg.exportDir.empty()) {
            std::string fname = cfg.levelPath.substr(cfg.levelPath.find_last_of("/\\") + 1);
            auto dot = fname.rfind('.');
            if (dot != std::string::npos) fname = fname.substr(0, dot);
            outPath = cfg.exportDir + "/" + fname + "_hitsounds.wav";
        } else {
            outPath = cfg.levelPath;
            auto dot = outPath.rfind('.');
            if (dot != std::string::npos) outPath = outPath.substr(0, dot);
            outPath += "_hitsounds.wav";
        }
        hm.writeWav(outPath);
        LOG_I("Exported hitsounds to %s", outPath.c_str());
        glfwTerminate();
        return 0;
    }

    // Stage 2: Loading
    LoadResult loadResult;
    showLoadingWindow([&](LoadingProgress& progress) {
        runLevelLoading(cfg, progress, loadResult);
    });

    if (!loadResult.level) {
        LOG_E("Failed to load level");
        glfwTerminate();
        return 1;
    }

    LOG_I("Level loaded: %zu tiles, BPM=%.1f", loadResult.level->tiles.size(), loadResult.level->settings.bpm);

    // Stage 3: Game
    showGameWindow(cfg, loadResult);

    LOG_I("Game window closed, exiting.");
    glfwTerminate();
    return 0;
}

int runApplicationFromCLI(const LauncherConfig& cfg, bool debugConsole) {
    earlyLog("[ADOCAO] CLI mode entered");

    std::string logPath = "ADOCAO.log";
    Logger::instance().init(logPath, debugConsole);

    LOG_I("ADOCAO starting (CLI mode)...");

#ifdef _WIN32
    earlyLog("[ADOCAO] DPI awareness...");
    enableDPIAwareness();
    earlyLog("[ADOCAO] CPU pin...");
    pinToBigCore();
#endif

    if (!glfwInit()) {
        LOG_E("Failed to initialize GLFW");
#ifdef _WIN32
        MessageBoxA(nullptr, "Failed to initialize OpenGL.\nPlease update your graphics driver.",
                    "ADOCAO - Error", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }

    LauncherConfig config = cfg;
    if (debugConsole) config.enableHitsounds = false;

    LOG_D("CLI: level=%s, music=%s, resolution=%dx%d, fullscreen=%d",
          config.levelPath.c_str(), config.musicPath.c_str(),
          config.resolutionW, config.resolutionH, config.fullscreen);

    if (config.exportHitsounds) {
        LevelData lvl;
        if (!lvl.loadFromFile(config.levelPath, nullptr, true)) {
            LOG_E("Failed to load level for export");
            glfwTerminate();
            return 1;
        }
        Timeline timeline;
        timeline.build(lvl, true);
        HitsoundManager hm;
        hm.init();
        auto groups = timeline.getHitsoundTimestampGroups();
        if (!hm.preSynthesize(groups, timeline.totalDuration())) {
            LOG_E("Export: pre-synthesis failed");
            glfwTerminate();
            return 1;
        }
        std::string outPath;
        if (!config.exportDir.empty()) {
            std::string fname = config.levelPath.substr(config.levelPath.find_last_of("/\\") + 1);
            auto dot = fname.rfind('.');
            if (dot != std::string::npos) fname = fname.substr(0, dot);
            outPath = config.exportDir + "/" + fname + "_hitsounds.wav";
        } else {
            outPath = config.levelPath;
            auto dot = outPath.rfind('.');
            if (dot != std::string::npos) outPath = outPath.substr(0, dot);
            outPath += "_hitsounds.wav";
        }
        hm.writeWav(outPath);
        LOG_I("Exported hitsounds to %s", outPath.c_str());
        glfwTerminate();
        return 0;
    }

    LoadResult loadResult;
    showLoadingWindow([&](LoadingProgress& progress) {
        runLevelLoading(config, progress, loadResult);
    });

    if (!loadResult.level) {
        LOG_E("Failed to load level");
        glfwTerminate();
        return 1;
    }

    LOG_I("Level loaded: %zu tiles, BPM=%.1f", loadResult.level->tiles.size(), loadResult.level->settings.bpm);

    showGameWindow(config, loadResult);

    LOG_I("Game window closed, exiting.");
    glfwTerminate();
    return 0;
}
