#include "Application.hpp"
#include "LauncherWindow.hpp"
#include "LoadingWindow.hpp"
#include "FileMap.hpp"
#include "LevelLoader.hpp"
#include "GameWindow.hpp"
#include "audio/HitsoundManager.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/util/Logger.hpp"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdlib.h>
#endif

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif

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

static bool canAppendLog(const std::string& path) {
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return false;
    fclose(f);
    return true;
}

// Directory of the running executable (symlinks resolved on macOS).
static std::string executableDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string p(buf, n);
    const auto pos = p.find_last_of("\\/");
    return pos == std::string::npos ? std::string{} : p.substr(0, pos);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::string dir(buf.data());
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved)) dir = resolved;
    return dir;
#elif defined(__linux__)
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string dir(buf);
    const auto pos = dir.find_last_of('/');
    return pos == std::string::npos ? std::string{} : dir.substr(0, pos);
#else
    return {};
#endif
}

// Where ADOCAO.log goes: next to the executable first. That is one predictable
// location regardless of which directory you launched from — a CWD-relative log
// used to litter whatever directory you happened to be in, $HOME included.
// Inside a macOS .app the executable directory is Contents/MacOS, and writing
// there would invalidate the code signature (see scripts/make-app.sh), so that
// one case falls through to the per-user log directory, then the temp dir.
static const std::string& logPath() {
    static const std::string path = []() -> std::string {
        const std::string exeDir = executableDir();
        if (!exeDir.empty() && exeDir.find("/Contents/MacOS") == std::string::npos) {
            const std::string p = exeDir + "/ADOCAO.log";
            if (canAppendLog(p)) return p;
        }

        std::filesystem::path dir;
#ifdef __APPLE__
        if (const char* home = std::getenv("HOME"))
            dir = std::filesystem::path(home) / "Library" / "Logs" / "ADOCAO";
#elif defined(_WIN32)
        if (const char* lad = std::getenv("LOCALAPPDATA"))
            dir = std::filesystem::path(lad) / "ADOCAO" / "logs";
#else
        if (const char* xdg = std::getenv("XDG_STATE_HOME"))
            dir = std::filesystem::path(xdg) / "ADOCAO";
        else if (const char* home = std::getenv("HOME"))
            dir = std::filesystem::path(home) / ".local" / "state" / "ADOCAO";
#endif
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string p = (dir / "ADOCAO.log").string();
            if (canAppendLog(p)) return p;
        }

        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            const std::string p = (tmp / "ADOCAO.log").string();
            if (canAppendLog(p)) return p;
        }
        return "ADOCAO.log";   // last resort
    }();
    return path;
}

static void earlyLog(const char* msg) {
    // First call truncates old log, subsequent calls append
    FILE* f = fopen(logPath().c_str(), s_firstEarlyLog ? "w" : "a");
    s_firstEarlyLog = false;
    if (f) { fprintf(f, "[EARLY] %s\n", msg); fclose(f); }
#ifdef _WIN32
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#endif
}

int runApplication(bool debugConsole) {
    earlyLog("[ADOCAO] main() entered");

    Logger::instance().init(logPath(), debugConsole);
    LOG_I("ADOCAO starting...");
    LOG_I("Log file: %s", logPath().c_str());

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
        FileMap map;
        if (!map.open(cfg.levelPath) ||
            !lvl.loadFromBuffer(map.data(), map.size(), nullptr, true)) {
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

    Logger::instance().init(logPath(), debugConsole);

    LOG_I("ADOCAO starting (CLI mode)...");
    LOG_I("Log file: %s", logPath().c_str());

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
        FileMap map;
        if (!map.open(config.levelPath) ||
            !lvl.loadFromBuffer(map.data(), map.size(), nullptr, true)) {
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
