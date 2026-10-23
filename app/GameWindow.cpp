#include "GameWindow.hpp"
#ifdef __APPLE__
#include "MacBorderless.hpp"
#endif

#include "LauncherWindow.hpp"
#include "LevelLoader.hpp"
#include "render/LevelScene.hpp"
#include "render/SceneConfig.hpp"
#include "core/util/Profile.hpp"
#include "ProcMem.hpp"   // 常驻口径（浸泡统计用）
#include "glad/gl_core.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/util/Easing.hpp"
#include <algorithm>
#include <array>
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

// 实现（STB_IMAGE_WRITE_IMPLEMENTATION）在 app/MapExport.cpp —— 同一个二进制里只留一份。
#include "stb_image_write.h"



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

// Start playback from the given floor (used for Space-from-selected-tile).
static void jumpToTile(Timeline& timeline, PlaybackClock& clock, AudioEngine& audio, HitsoundManager& hs,
                        const LevelData& level, int floor) {
    if (floor < 0 || floor >= (int)level.tileCount()) return;
    double targetTime = timeline.startTimeAt(floor);
    float offsetSec = level.settings.offset / 1000.0f;
    float audioPos = (float)(targetTime + offsetSec);
    if (audioPos < 0) audioPos = 0;

    const bool hasMusic = audio.hasMusic();
    const float musicDur = hasMusic ? audio.duration() : 0.0f;
    if (hasMusic && audioPos < musicDur) {
        // Start in the middle of the song: seek the music there and let it drive
        // the clock (audio-visual sync).
        clock.startAt(glfwGetTime(), audioPos, offsetSec);
        hs.resetAt(audioPos);
        audio.seek(audioPos);
        audio.play();
    } else {
        // No music, or the target tile lies past the end of the song (charts may
        // have content beyond the audio). Play the level silently on the wall
        // clock. Seeking past EOF must be avoided: miniaudio silently restarts
        // from 0 while the clock would claim a huge time, so the song would play
        // from the beginning against a level that thinks it is hours in.
        clock.startAt(glfwGetTime(), audioPos, offsetSec);
        hs.resetAt(audioPos);
        if (!hasMusic && audio.isPlaying()) audio.pause();
    }
}

} // namespace

GameWindow::~GameWindow() {
#ifdef __APPLE__
    // 兜底：正常析构时把窗口级状态（level）还原；run() 收尾已做过就不重复。
    if (!m_nativeCleanupDone && m_isFullscreen) macosBorderlessExit(m_window);
#endif
}

bool GameWindow::init(const LauncherConfig& cfg, LoadResult& result) {
    m_cfg = &cfg;
    m_level = result.level.get();
    m_timeline = result.timeline.get();
    m_cameraTimeline = result.cameraTimeline.get();
    m_playback = result.playback.get();
    m_hitsoundMgr = &result.hitsounds;
    m_audioEngine = &result.audio;
    m_targetAspect = (float)cfg.resolutionW / (float)cfg.resolutionH;
    LOG_D("GameWindow::init: %lld tiles, fullscreen=%d", (long long)m_level->tileCount(), cfg.fullscreen);

    // Create window
    m_isFullscreen = cfg.fullscreen;

    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;

    // GLFW window sizes are logical points; on Retina the framebuffer is
    // content-scale × window, and glfwGetVideoMode() reports physical pixels.
    // Convert requested resolutions to logical points so the framebuffer
    // matches the requested physical resolution exactly. No-op on Windows/
    // Linux where content scale is 1.0.
    float csx = 1.0f, csy = 1.0f;
    if (primary) {
        glfwGetMonitorContentScale(primary, &csx, &csy);
        if (csx <= 0.0f) csx = 1.0f;
        if (csy <= 0.0f) csy = 1.0f;
    }
    // **交互窗口按"点"**（用户说的 800×600 就该是屏幕上 800×600），
    // 抓帧（--capture）仍按**物理像素**（像素门槛的基线尺寸不能随显示器变）。
    // 之前两者都按物理像素：Retina 上 `--width 800` 会得到一个 400×300 的小窗口。
    const bool captureRun = !cfg.capturePath.empty();
    int winW = captureRun ? (int)(cfg.resolutionW / csx) : cfg.resolutionW;
    int winH = captureRun ? (int)(cfg.resolutionH / csy) : cfg.resolutionH;
    if (winW < 1) winW = 1;
    if (winH < 1) winH = 1;
    // **别把 video mode 再除一次 content scale**：macOS 的 `glfwGetVideoMode()` 报的就是
    // **逻辑点**（与窗口/坐标系同单位），不是物理像素 —— 实测本机 mode=1470x956、scale=2.0，
    // 除完只剩 735x478，于是"无边框铺满"只铺了屏幕左上四分之一（2026-10-19 用户截图）。
    // Windows/Linux 的 scale 恒为 1，这条对它们没有影响。
    int screenW = mode ? mode->width : winW; if (screenW < 1) screenW = 1;
    int screenH = mode ? mode->height : winH; if (screenH < 1) screenH = 1;
    int fsW = mode ? screenW : winW;
    int fsH = mode ? screenH : winH;
    m_windowedW = winW;
    m_windowedH = winH;
    // 窗口模式的落点先算好：全屏启动时 Alt+Enter 退回窗口也要落回屏幕中间，
    // 不能是 (0,0)（默认值）。
    if (primary && !captureRun && !cfg.fullscreen) {
        // 窗口比工作区大就按比例缩小（点为单位），否则会被系统截掉一部分。
        int wx, wy, ww, wh;
        glfwGetMonitorWorkarea(primary, &wx, &wy, &ww, &wh);
        if (ww > 0 && wh > 0 && (winW > ww || winH > wh)) {
            const double k = std::min((double)ww / winW, (double)wh / winH);
            winW = std::max(1, (int)(winW * k));
            winH = std::max(1, (int)(winH * k));
            LOG_I("window: 请求的窗口大于工作区 %dx%d 点 → 缩到 %dx%d 点", ww, wh, winW, winH);
        }
    }
    if (primary) {
        int wx, wy, ww, wh;
        glfwGetMonitorWorkarea(primary, &wx, &wy, &ww, &wh);
        m_windowedX = wx + (ww - winW) / 2;
        m_windowedY = wy + (wh - winH) / 2;
    }

    if (cfg.msaaSamples > 0) glfwWindowHint(GLFW_SAMPLES, cfg.msaaSamples);
    // 与 osu!/SDL 对齐：窗口**创建时就是 RESIZABLE** —— SDL 里只有 RESIZABLE 的窗口才被允许
    // 进 Space（`NSWindowCollectionBehaviorFullScreenPrimary`，否则 FullScreenNone）。
    // 只有全屏（Borderless）需要 RESIZABLE：macOS 只允许可缩放的窗口进 Space。
    // 窗口态不可缩放（大小由 --width/--height 决定，启动后不改）。
    glfwWindowHint(GLFW_RESIZABLE, cfg.fullscreen ? GLFW_TRUE : GLFW_FALSE);
    // 抓帧（--capture）是开发/验收用的脚本化运行：**别弹窗、别抢焦点**。
    // GL 仍然需要窗口（GLFW 的隐藏窗口照样有有效 drawable），所以三个 hint 一起给：
    // 不显示、创建时不聚焦、显示时也不聚焦 —— 否则 macOS 上建窗口会激活 App，
    // 把用户正在用的窗口顶掉（这就是"抢焦点"）。
    if (!cfg.capturePath.empty()) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }

    // 砖块几何的 VS 要用 `fma()`（GLSL 4.00+）来逐字镜像参考实现的融合结构，所以首选 4.1
    // （macOS 的上限）。**拿不到就退到 3.3 core** —— 那时几何走 CPU 回退路（见 TileMeshCpu）：
    // 3.3 编译不了那份 VS，但参考几何在 CPU 上是逐位精确的，画面照样对。
    // `ADOCAO_GL_VERSION=3.3` 可强制（验收用：直接验"3.3 上下文 + CPU 几何"这条降级链）。
    auto createWindow = [&](int major, int minor) -> GLFWwindow* {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        GLFWwindow* w = nullptr;
        if (cfg.fullscreen) {
#ifdef _WIN32
            // 全屏 = **无边框窗口**（osu! 的 Borderless 语义），不用 glfwSetWindowMonitor(monitor)。
            // 它是普通顶层窗口，DWM 照常合成，WGC / DXGI 复制 / PresentMon 都能用，
            // 这就是"能录制的全屏"。（不设 GLFW_FLOATING：置顶窗口在 Alt+Tab 后会
            // 一直盖住别人；任务栏由 shell 的"前台窗口铺满显示器"判定隐藏。）
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            w = glfwCreateWindow(fsW, fsH, "ADOCAO", nullptr, nullptr);
            if (w) {
                int mx = 0, my = 0;
                if (primary) glfwGetMonitorPos(primary, &mx, &my);
                glfwSetWindowPos(w, mx, my);
            }
#else
            // macOS/Linux 全屏 = osu! 的 **Borderless**（源码 `SDL_WINDOW_FULLSCREEN_DESKTOP`）：
            // 铺满屏幕的普通无边框窗口，**不改显示模式、不进 Space、不改系统 UI**。
            // （2026-10-19：曾照 osu! 的 Fullscreen 去切显示模式 / 上 kiosk presentation options，
            //   结果把用户的机器搞死过 —— kiosk 会禁掉 Mission Control/⌘Tab，app 一卡住人就被困住；
            //   切模式会把桌面从 2x 掉成 1x。**这两条都不再做了**。）
            // 窗口级只做：无边框 + 铺满 + 抬到菜单栏之上（进程退出即消失）。
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            w = glfwCreateWindow(fsW, fsH, "ADOCAO", nullptr, nullptr);
            if (w) {
                int mx = 0, my = 0;
                if (primary) glfwGetMonitorPos(primary, &mx, &my);
                glfwSetWindowPos(w, mx, my);
            }
#endif
        } else {
            glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
            w = glfwCreateWindow(winW, winH, "ADOCAO", nullptr, nullptr);
            if (w && primary) glfwSetWindowPos(w, m_windowedX, m_windowedY);
        }
        return w;
    };

    int wantMajor = 4, wantMinor = 1;
    if (const char* env = std::getenv("ADOCAO_GL_VERSION")) {   // 测试钩子：强制某个版本
        int a = 0, b = 0;
        if (std::sscanf(env, "%d.%d", &a, &b) == 2 && a >= 3) { wantMajor = a; wantMinor = b; }
    }
    m_window = createWindow(wantMajor, wantMinor);
    if (!m_window && !(wantMajor == 3 && wantMinor == 3)) {
        LOG_W("OpenGL %d.%d 上下文拿不到，退回 3.3 core（砖块几何将走 CPU 回退）", wantMajor, wantMinor);
        m_window = createWindow(3, 3);
    }
    if (!m_window) { LOG_E("Failed to create game window"); return false; }
#ifdef __APPLE__
    // 窗口态只留关闭按钮；全屏 = 进自己的 Space（等窗口上屏后由主循环第一帧触发）。
    if (!cfg.fullscreen) macosWindowedChrome(m_window);
    else m_pendingFullscreenEnter = true;
#endif

    glfwMakeContextCurrent(m_window);
    {
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(m_window, &ww, &wh);
        glfwGetFramebufferSize(m_window, &fw, &fh);
        int mx = 0, my = 0, mw = 0, mh = 0;
        if (primary) glfwGetMonitorWorkarea(primary, &mx, &my, &mw, &mh);
        LOG_I("window: %dx%d 点, framebuffer %dx%d, 工作区 %dx%d 点, content scale %.2f, fullscreen=%d mode=%d",
              ww, wh, fw, fh, mw, mh, csx, (int)cfg.fullscreen, cfg.windowMode);
    }
    // 录屏/对比时建议 ADOCAO_VSYNC=1：无 vsync 的 GL 窗口在 macOS + OBS 窗口捕获下
    // 会抓到“两帧合成”的撕裂帧（表现为轨道重影/黑帧）。
    const bool vsync = [] {
        return env::flag("ADOCAO_VSYNC");
    }();
    glfwSwapInterval(vsync ? 1 : 0);
    // 用户拿"只有 59.4 fps"来问的时候，第一件事是看这行：vsync 是唯一会**真的**把
    // app 锁在刷新率上的开关（默认关）。抓屏工具自己的刷新率上限是另一回事
    // （WGC/DXGI 复制只看得见合成器每周期提交的那一帧），别混为一谈。
    // 只报 vsync 状态；帧率上限由 run() 里的 ADOCAO_UNCAPPED 决定，在那边报
    // （这里还没解析它，写"默认 320 fps 节流"会在 UNCAPPED 打开时自相矛盾 —— 真踩过）。
    LOG_I("swap interval: %d — %s", vsync ? 1 : 0,
          vsync ? "vsync on，帧率上限 = 显示器刷新率" : "vsync off，不锁帧");
    if (cfg.fullscreen && mode) {
        LOG_I("fullscreen: %dx%d @ %d Hz（%s）", mode->width, mode->height, mode->refreshRate,
#ifdef _WIN32
              "无边框窗口，DWM 合成 → WGC/OBS 可抓"
#else
              "Borderless：铺满屏幕的无边框窗口（不切显示模式、不进 Space、不动系统 UI）"
#endif
        );
    }
    if (!loadGLCore()) { LOG_E("Failed to load OpenGL functions"); glfwDestroyWindow(m_window); return false; }
    if (cfg.msaaSamples > 0) glEnable(GL_MULTISAMPLE);
    {   // 实际拿到的采样数可能被驱动/显示器降级（GLFW_SAMPLES 只是 hint）→ 记一行，别让它静默变了
        GLint got = 0;
        glGetIntegerv(GL_SAMPLES, &got);
        LOG_I("MSAA: 请求 %dx，实际 %dx（0 = 驱动没给到这个采样数）", cfg.msaaSamples, (int)got);
    }
    LOG_D("OpenGL %s | GLSL %s", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    LOG_D("GPU vendor=%s renderer=%s", vendor ? vendor : "?", renderer ? renderer : "?");

    // Show window immediately so user sees it while heavy init runs
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glfwSwapBuffers(m_window);

    // Scene: shaders / tile mesh / planets (heap objects freed by LevelScene)
    m_scene = std::make_unique<adofai::LevelScene>();
    // LauncherConfig（app 的产品配置）→ SceneConfig（库的宿主无关配置）
    adofai::SceneConfig sceneCfg;
    sceneCfg.trackColorOverride = cfg.trackColorOverride;
    sceneCfg.trackFillColor = cfg.trackFillColor;
    sceneCfg.trackStrokeColor = cfg.trackStrokeColor;
    sceneCfg.autoStroke = cfg.autoStroke;
    sceneCfg.showTrail = cfg.showTrail;
    sceneCfg.trailDuration = cfg.trailDuration;
    sceneCfg.trailSampleRate = cfg.trailSampleRate;
    sceneCfg.trailAdaptive = cfg.trailAdaptive;
    sceneCfg.trailTargetFps = cfg.trailTargetFps;
    sceneCfg.trailRateMin = cfg.trailRateMin;
    sceneCfg.trailRateMax = cfg.trailRateMax;
    sceneCfg.trailPerTile = cfg.trailPerTile;
    sceneCfg.trailSamplesPerTile = cfg.trailSamplesPerTile;
    sceneCfg.trailLengthInTiles = cfg.trailLengthInTiles;
    sceneCfg.trailTiles = cfg.trailTiles;
    sceneCfg.trailSagitta = cfg.trailSagitta;
    sceneCfg.cullBlockFloors = cfg.cullBlockFloors;
    sceneCfg.deterministicCull = !cfg.capturePath.empty();
    sceneCfg.showAllTiles = cfg.showAllTiles;
    sceneCfg.residentBytes = [] { return appmem::currentResidentBytes(); };
    if (!m_scene->init(sceneCfg, *m_level, m_timeline)) {
        // Shader compile failed; fall back strings are already exhausted
        m_scene.reset();
        glfwDestroyWindow(m_window);
        return false;
    }

    // Camera + background
    {
        // 优先级：显式 `--bg` > 谱面的 settings.backgroundColor > cfg 默认。
        // （谱面优先是为了 neon_alpha 这类"谱面自带底色"的用例；但 CLI 显式给了就 CLI 赢。）
        std::string hex = cfg.bgExplicit ? cfg.backgroundColor : m_level->settings.backgroundColor;
        if (hex.empty()) hex = cfg.backgroundColor;
        if (hex.length()>=6) { unsigned r,g,b; sscanf(hex.c_str(),"%02x%02x%02x",&r,&g,&b);
            m_bgR=r/255.0f; m_bgG=g/255.0f; m_bgB=b/255.0f; }
    }
    m_camCtrl.attach(m_camera);
    m_camera.setAdoZoom(m_level->settings.zoom);   // 谱面 zoom 是 ADOFAI 口径（越大看得越远）
    if (m_level->tileCount() > 0) {
        const auto& p0 = m_level->tilePositionAt(0);
        m_camCtrl.snapTo(p0[0], p0[1]);
        LOG_I("camera init: tile0=(%.3f,%.3f) adoZoom=%.1f internalZoom=%.2f", (double)p0[0], (double)p0[1],
              (double)m_level->settings.zoom, (double)m_camera.zoom());
    }
    rebuildCameraTimeline();

    // Mesh build: 同步（主线程、主上下文）。异步/共享上下文那条路已删除：
    // VAO 是上下文私有的，跨上下文建出来的 VAO 主上下文看不到（整帧黑屏）。
    m_scene->buildSync(*m_level);

    // Hitsound attach
    if (m_hitsoundMgr->isSynthesized()) {
        m_audioEngine->attachExternal(m_hitsoundMgr->buffer(), m_hitsoundMgr->totalFrames(),
            m_hitsoundMgr->channels(), m_hitsoundMgr->sampleRate(),
            m_hitsoundMgr->cursor(), m_hitsoundMgr->playing());
    }

    // Input callbacks → CameraController (pan/zoom/click state)
    glfwSetWindowUserPointer(m_window, &m_camCtrl);
    // **窗口点 vs 帧缓冲像素**：Retina/全屏下两者不等，光标换算必须知道窗口点尺寸。
    // 初始化喂一次，之后跟随 window-size / framebuffer-size 回调更新。
    {
        int ww = 0, wh = 0;
        glfwGetWindowSize(m_window, &ww, &wh);
        m_camCtrl.setWindowSizePts(ww, wh);
    }
    glfwSetWindowSizeCallback(m_window, [](GLFWwindow* w, int ww, int wh) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        if (ctrl) ctrl->setWindowSizePts(ww, wh);
    });
    glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow* w, int, int) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        int ww = 0, wh = 0;
        glfwGetWindowSize(w, &ww, &wh);
        if (ctrl) ctrl->setWindowSizePts(ww, wh);
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* w, int b, int a, int) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        if (b != GLFW_MOUSE_BUTTON_LEFT) return;
        // **按下前先取一次真实光标位置**：窗口刚开、鼠标还没动过时，GLFW 不会送
        // cursor-pos 事件，`m_cursorX/Y` 还是 (0,0) —— 此时按下会把 dragStart 记成 (0,0)，
        // 第一次拖动就把镜头甩出几百砖（用户看到的"传送到不在任何砖上的地方"）。
        if (a == GLFW_PRESS) {
            double cx = 0.0, cy = 0.0;
            glfwGetCursorPos(w, &cx, &cy);
            ctrl->onCursorPos(cx, cy);
        }
        ctrl->onMouseButton(a == GLFW_PRESS);
    });
    glfwSetCursorPosCallback(m_window, [](GLFWwindow* w, double x, double y) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        ctrl->onCursorPos(x, y);
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow* w, double, double dy) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        ctrl->onScroll(dy);
    });

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    // **抢一次键盘焦点**：从终端直接跑 `build/adocao`（不是 .app 包）时，macOS 不会自动把
    // 这个进程当成前台 App，窗口出来了但键盘事件照样送到别处（空格/方向键全失效）。
    // `glfwFocusWindow` 在 macOS 上会 makeKeyAndOrderFront + activateIgnoringOtherApps。
    if (cfg.capturePath.empty()) glfwFocusWindow(m_window);

    m_lastFrameTime = glfwGetTime();
    m_autoPlayTriggerTime = glfwGetTime() + (cfg.autoPlay ? 0.5 : 999999.0);
    return true;
}

// 建运镜时间线 + 把控制器摆到 settings 的初始状态。加载后调用一次；换关卡要重调。
void GameWindow::rebuildCameraTimeline() {
    if (!m_level || !m_timeline || !m_cameraTimeline) return;
    std::array<double, 2> p0{0.0, 0.0};
    if (m_level->tileCount() > 0) p0 = m_level->tilePositionAt(0);
    const float bpm0 = m_timeline->tileBPMs().empty() ? 100.0f : m_timeline->tileBPMs()[0];
    m_cameraDirector.reset(m_level->settings, m_cameraTimeline->keys(), p0[0], p0[1], bpm0);
    m_cameraDirector.resetDrive();
    m_wasPlaying = false;
    if (m_cameraTimeline && !m_cameraTimeline->empty()) {
        LOG_I("camera: %zu 条运镜事件（已展开/已排序），settings.relativeTo=%s",
              m_cameraTimeline->keys().size(), m_level->settings.relativeTo.c_str());
    }
}

void GameWindow::navigateToTile(int floor, bool center) {
    if (floor < 0 || floor >= (int)m_level->tileCount()) return;
    // 只有"跳转"类操作（书签、Ctrl+方向键）才把镜头移过去；
    // **点击选砖不移动镜头** —— 你点的那块本来就在眼前，把相机瞬移过去反而像"传送"
    // （用户实测：按下后手抖 3 px 就被当成点击，镜头跳出去好几砖）。
    if (center) {
        const auto& p = m_level->tilePositionAt(floor);
        m_camCtrl.snapTo(p[0], p[1]);
    }
    LOG_D("navigate: tile=%d center=%d cam=(%.2f,%.2f)", floor, (int)center,
          m_camera.targetX(), m_camera.targetY());
    m_selectedTile = floor;
}

void GameWindow::handleInput() {
    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);

    // 窗口模式在启动时就定好了（用户 2026-10-19 拍板）：运行期**不允许**切全屏，
    // 所以 Alt+Enter 不再是快捷键（真按了也不会有任何反应）。

    double now = glfwGetTime();

    // Space toggles playback (or auto-play trigger)
    bool spacePressed = (glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS)
                     || (m_cfg->autoPlay && now >= m_autoPlayTriggerTime && !m_playback->isPlaying());
    if (spacePressed && !m_wasSpacePressed) {
        m_autoPlayTriggerTime = 999999.0;
        if (!m_playback->isPlaying()) {
            if (m_selectedTile >= 0) {
                int startFloor = m_selectedTile;
                m_selectedTile = -1;
                jumpToTile(*m_timeline, *m_playback, *m_audioEngine, *m_hitsoundMgr, *m_level, startFloor);
            } else {
                m_playback->start(glfwGetTime());
                m_hitsoundMgr->resetAt(0);
                m_musicPending = true;  // always delay audio start by audioStartOffset (pre-roll)
            }
        } else { m_playback->stop(); m_audioEngine->pause(); m_musicPending = false; }
    }
    m_wasSpacePressed = spacePressed;

    // Click-to-select tile (only when stopped)
    if (!m_playback->isPlaying() && m_camCtrl.consumeClick()) {
        int fbW, fbH;
        glfwGetFramebufferSize(m_window, &fbW, &fbH);
        glm::dvec2 world = m_camCtrl.screenToWorld(m_camCtrl.cursorFbX(fbW), m_camCtrl.cursorFbY(fbH),
                                                   fbW, fbH, m_targetAspect);

        int best = -1; double bestDist = 1.0;
        for (int i = 0; i < (int)m_level->tileCount()-1; i++) {
            double dx = m_level->tilePositionAt(i)[0] - world.x;
            double dy = m_level->tilePositionAt(i)[1] - world.y;
            double d = dx*dx + dy*dy;
            if (d < bestDist*bestDist) { bestDist = std::sqrt(d); best = i; }
        }
        if (best >= 0) navigateToTile(best, /*center=*/false);
        else m_selectedTile = -1;
    }

    // Bookmark navigation: Ctrl+Left/Right with long-press repeat (only when stopped).
    // macOS reserves Ctrl+←/→ for Mission Control space switching (events never
    // reach the app), so on Apple platforms ⌘(Command)+←/→ works too.
    bool navMod = (glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
               || (glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
#ifdef __APPLE__
    navMod = navMod || (glfwGetKey(m_window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS)
                    || (glfwGetKey(m_window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS);
#endif
    if (!m_playback->isPlaying() && !m_level->bookmarkFloors.empty()) {
        static double bmLHoldStart = 0, bmRHoldStart = 0;
        bool bmL = navMod && (glfwGetKey(m_window, GLFW_KEY_LEFT) == GLFW_PRESS);
        bool bmR = navMod && (glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS);
        double now = glfwGetTime();
        auto jumpBM = [&](bool left) {
            int cur = m_selectedTile;
            if (cur < 0) {
                // No tile selected: anchor on the tile nearest the view centre.
                double cx = m_camera.targetX(), cy = m_camera.targetY();
                double bestD = -1.0;
                int nTiles = (int)m_level->tileCount() - 1;   // skip synthetic last tile
                for (int i = 0; i < nTiles; i++) {
                    double dx = m_level->tilePositionAt(i)[0] - cx;
                    double dy = m_level->tilePositionAt(i)[1] - cy;
                    double d = dx * dx + dy * dy;
                    if (bestD < 0.0 || d < bestD) { bestD = d; cur = i; }
                }
                if (cur < 0) return;
            }
            int target = -1;
            if (left) { for (int b : m_level->bookmarkFloors) { if (b < cur) target = b; else break; } }
            else      { for (int b : m_level->bookmarkFloors) { if (b > cur) { target = b; break; } } }
            if (target >= 0) navigateToTile(target);
        };
        if (bmL) {
            if (bmLHoldStart == 0) { bmLHoldStart = now; jumpBM(true); }
            else if (now - bmLHoldStart >= 0.5) { bmLHoldStart = now; jumpBM(true); }
        } else { bmLHoldStart = 0; }
        if (bmR) {
            if (bmRHoldStart == 0) { bmRHoldStart = now; jumpBM(false); }
            else if (now - bmRHoldStart >= 0.5) { bmRHoldStart = now; jumpBM(false); }
        } else { bmRHoldStart = 0; }
    }

    // Arrow key tile navigation: long-press with 0.5s initial delay (only when stopped, tile selected, Ctrl NOT held)
    bool ctrlHeld = navMod;
    if (!m_playback->isPlaying() && m_selectedTile >= 0 && !ctrlHeld) {
        static double arrowHoldStart = 0;
        bool al=(glfwGetKey(m_window,GLFW_KEY_LEFT)==GLFW_PRESS);
        bool ar=(glfwGetKey(m_window,GLFW_KEY_RIGHT)==GLFW_PRESS);
        int tn=(int)m_level->tileCount()-1;
        if (al || ar) {
            double now = glfwGetTime();
            bool firstPress = (arrowHoldStart == 0);
            if (firstPress) arrowHoldStart = now;
            bool move = firstPress || (now - arrowHoldStart >= 0.5);
            if (move) {
                if (al && m_selectedTile > 0) m_selectedTile--;
                if (ar && m_selectedTile < tn - 1) m_selectedTile++;
                navigateToTile(m_selectedTile);
            }
        } else {
            arrowHoldStart = 0;
        }
    }
}

void GameWindow::update(float deltaMs) {
    double now = glfwGetTime();

    // Playback update
    {
    profile::Scoped _sPlayback("playback-sync");
    if (m_playback->isPlaying()) {
        // Delayed music start: wait for audioStartOffset before playing from position 0
        if (m_musicPending && m_playback->elapsedTimeMs() >= m_playback->audioStartOffset() * 1000.0f) {
            m_musicPending = false;
            if (m_audioEngine->hasMusic()) {
                m_audioEngine->seek(0);
            }
            m_audioEngine->play();
        }
        if (m_audioEngine->hasMusic() && m_audioEngine->isPlaying()) {
            m_playback->syncToAudio(m_audioEngine->position(), m_level->settings.offset/1000.0f);
        } else {
            m_playback->updateWallClock(now);
        }
    }
    }

    if (m_playback->isPlaying()) {
        profile::Scoped _s("applyFrame");
        m_scene->applyFrame(m_playback->frame(), *m_timeline);
    }

    // 关卡相机：播放时由 CameraDirector 驱动（settings + MoveCamera 时间线）。
    // 时间用 PlaybackClock::timeInLevel()（与 tileStartTimes 同一条基准）。
    const bool playing = m_playback->isPlaying();
    profile::Scoped _sCam("camera");
    if (playing && m_level->tileCount() > 0) {
        int tileIdx = m_playback->currentTileIndex();
        if (tileIdx < 0) tileIdx = 0;
        if (tileIdx >= (int)m_level->tileCount() - 1) tileIdx = (int)m_level->tileCount() - 2;
        const auto p = m_level->tilePositionAt(tileIdx);
        if (env::flag("ADOCAO_CAM_DEBUG")) {
            LOG_I("capture-pos: tileIdx=%d p=(%.2f,%.2f) windowed=%d", tileIdx,
                  (double)p[0], (double)p[1], (int)m_level->tilesWindowed());
        }
        const auto& bpms = m_timeline->tileBPMs();
        const float bpm = (tileIdx >= 0 && (size_t)tileIdx < bpms.size()) ? bpms[(size_t)tileIdx] : 100.0f;
        const double pitch = std::max(0.1, (double)m_level->settings.pitch / 100.0);
        const double t = m_playback->timeInLevel();

        // seek/update 的判据在库里（`CameraDirector::drive`），宿主不再自己判断。
        m_cameraDirector.drive(t, (double)deltaMs / 1000.0, bpm, pitch, p[0], p[1], tileIdx);

        m_camera.setTarget(m_cameraDirector.x(), m_cameraDirector.y());
        m_camera.setAdoZoom((float)m_cameraDirector.zoom());
        m_camera.setRotation((float)m_cameraDirector.rotation());
    } else if (m_wasPlaying) {
        m_cameraDirector.resetDrive();
        // 停下：把当前视角接收为拖拽的基准（否则会跳回播放前的位置）。
        // **保留旋转**（Re_ADOJAS 也保留）：CameraController 的 screenToWorld/拖动已经
        // 按 camera.rotation() 反旋转，所以暂停时点击选中仍然准确。
        m_camCtrl.snapTo(m_camera.targetX(), m_camera.targetY());
        m_cameraDirector.resetDrive();
    }
    m_wasPlaying = playing;

    // Pan/drag (only when not playing)
    int fbW, fbH, winW, winH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    glfwGetWindowSize(m_window, &winW, &winH);
    m_camCtrl.update(fbW, fbH, m_targetAspect, !m_playback->isPlaying(), winW, winH);

    // **没有人拖拽，相机却动了** —— 与 AnimateTrack/MoveTrack 移动砖的情形区分开。
    // 用户报"传送"时，这条日志能直接回答：是相机被谁改了，还是只是砖在动。
    if (!m_camCtrl.dragging() && !m_playback->isPlaying()) {
        const double dx = m_camera.targetX() - m_lastCamX;
        const double dy = m_camera.targetY() - m_lastCamY;
        if (dx * dx + dy * dy > 0.25) {   // >0.5 砖
            LOG_W("camera-jump: target (%.3f,%.3f) → (%.3f,%.3f)  zoom %.1f  拖拽中=%d 播放中=%d",
                  m_lastCamX, m_lastCamY, m_camera.targetX(), m_camera.targetY(),
                  (double)m_camera.zoom(), (int)m_camCtrl.dragging(), (int)m_playback->isPlaying());
        }
    }
    m_lastCamX = m_camera.targetX();
    m_lastCamY = m_camera.targetY();

    // **每秒一条状态**（INFO，进日志文件）：相机 / 缩放 / 拖拽 / 播放 / 时间。
    // 复现"镜头乱跳"时，把这一串拿去就能看出是相机在动还是谱面在动、动在什么时间点。
    if (env::flag("ADOCAO_STATE_LOG")) {   // 排查用：默认关，否则日志每秒长一行
        static double lastLog = 0.0;
        const double now = glfwGetTime();
        if (now - lastLog >= 1.0) {
            lastLog = now;
            LOG_I("state: t=%.2fs cam=(%.2f,%.2f) zoom=%.1f rot=%.1f drag=%d play=%d tile=%d focus=%d space=%d",
                  m_playback->timeInLevel(), m_camera.targetX(), m_camera.targetY(),
                  (double)m_camera.zoom(), (double)m_camera.rotation(),
                  (int)m_camCtrl.dragging(), (int)m_playback->isPlaying(), m_selectedTile,
                  (int)glfwGetWindowAttrib(m_window, GLFW_FOCUSED),
                  (int)(glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS));
        }
    }
}

void GameWindow::render() {
    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    LetterboxedViewport vp = computeLetterbox(fbW, fbH, m_targetAspect);
    m_scene->setViewportHeightPx(vp.h);   // LOD：视口像素高度（每世界单位多少像素）

    {
        adofai::profile::Scoped _s("setup");   // 帧头：letterbox + 两次 clear + setAspect
        glViewport(0, 0, fbW, fbH);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);

        glViewport(vp.x, vp.y, vp.w, vp.h);
        glScissor(vp.x, vp.y, vp.w, vp.h);
        glEnable(GL_SCISSOR_TEST);
        glClearColor(m_bgR, m_bgG, m_bgB, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        m_camera.setAspect((float)vp.w, (float)vp.h);
    }

    bool playing = m_playback->isPlaying();
    // Volume 砖色：音乐位置的 RMS（与抓帧/音频播放无关，纯包络 → 可复现）。
    const float audioPos = m_playback->timeInLevel() + m_playback->preRoll() - m_playback->audioStartOffset();
    m_scene->render(m_camera, *m_timeline, playing, m_playback->timeInLevel(),
                    playing ? -1 : m_selectedTile, m_audioEngine->amplitudeAt(audioPos));
    // (glfwSwapBuffers is called by run() so it can time the work separately
    // from the vsync/pacing wait.)
}

// （原来这里是 `releaseMeshTemporaries()`：网格建完就清 `tileHasTwirl/tileHasSetSpeed`
//  「省内存」。**那是个真 bug**（2026-10-17 用户报"事件图标呢？2021 没有"）：
//  ③ 切片 4 之后每砖图标位**不再有**任何密集数组（`m_iconBits` 已删），图标位是 `deriveRange`
//  在**绘制时**按窗口现从这两张位图派生的 —— 清掉它们，非窗口化的谱面图标就**全部消失**，
//  而且不崩、不报错（`i < size()` 保护把条件变成恒假）。像素门槛也没抓到：基线是在同一个
//  bug 下存的（两边一起丢图标 = 逐字节相同）。整条链现在就靠 `releaseMemory()` 那句
//  "必须留着 tileBPMs / tileHasTwirl / tileHasSetSpeed"（见 LevelData.hpp 的存活契约）。

// 确定性抓帧：固定"关卡时刻 + 相机 + 缩放"渲染**一帧**，写 PNG 后退出。
//
// 为什么不能用主循环：主循环里 update() 会 `m_playback->updateWallClock(glfwGetTime())`
// 把时刻重新锚到墙上时钟，还会 `setMeasuredFrameMs()` 喂拖尾自适应 → 两次跑不可能逐字节相同。
// 这里绕开这一切：直接 startAt 一个绝对时刻，之后**不再调用** update()，只手动做
// "给场景一帧 + 相机 snap + 抓像素"。所以同一组参数两次跑必须 byte-identical —— 这是
// `--capture` 存在的唯一理由（给"GPU 实例滑窗"这类改动当像素级验收）。
void GameWindow::captureAndExit() {
    if (!m_scene->meshReady()) { LOG_W("capture: 网格没建好，放弃"); return; }

    // 2) 摆到目标时刻：timeInLevel = elapsed/1000 - preRoll，elapsed = (audioPos + audioStartOffset)*1000
    //    → audioPos = T + preRoll - audioStartOffset。wallClock 传 0：之后不再有 wall clock 参与。
    //    `--capture-tile N` 把 T 换成第 N 砖的起始时刻：6 万砖的验收谱面上，按秒给的时刻会随
    //    砖时长累积漂移，按砖给则永远落在同一砖（相机会 snap 到它），门槛不受时间轴数学影响。
    // 抓帧必须**确定性**：任何"由帧时间/墙上时钟/音频反馈决定的量"都不许进这条路径
    // （2026-10 真踩过：拖尾的自适应采样率让一个数值逐位相同、只快了几毫秒的改动，
    //  看起来改了 31/51 个像素状态）。main.cpp 已在 --capture 时把 trailAdaptive 关掉，
    // 这里再守一道 —— 真被打开了就喊出来，别让它静默污染基线。
    if (m_cfg->trailAdaptive) {
        LOG_E("capture-selfcheck: trailAdaptive=true —— 抓帧不再确定性！采样率会被帧时间驱动（应为 false）");
    } else {
        LOG_I("capture-selfcheck: 拖尾采样率已钉死 = %.1f Hz（adaptive off，确定性抓帧）",
              (double)m_cfg->trailSampleRate);
    }
    double targetTime = (double)m_cfg->captureTime;   // double：2.64e6 BPM 的谱砖区间只有 ~1e-6 s
    double capSpan = 1e-6;   // 目标砖的区间宽度（用于抓帧自校正）
    if (m_cfg->captureTile >= 0) {
        size_t N = (size_t)m_cfg->captureTile;
        const size_t nTiles = (size_t)m_timeline->tileCount();
        if (N < nTiles) {
            // 取这一砖区间的**中点**（用 st[N+1]-st[N]，不要用 tileDurations —— 中旋砖的
            // 时长与实际区间不是一回事，会落到隔壁砖）。audioPos 是 float（千秒量级只有
            // ~1e-4 s 分辨率），落在区间起点上会被舍到上一砖，currentTileIndex() 就变成 N-1。
            double span = (N + 1 < nTiles) ? (m_timeline->startTimeAt((int64_t)N + 1) -
                                               m_timeline->startTimeAt((int64_t)N)) : 0.0;
            capSpan = std::max(1e-9, span);
            targetTime = (double)m_timeline->startTimeAt((int64_t)N) + 0.5 * span;
            // 自检：大砖号上 `startTimeAt` 是 float 累加，区间宽度有 ~1e-4 的相对误差，
            // "中点"可能已经越到下一砖（实测 500000 → 500001）。落在隔壁就往回退。
            for (int tries = 0; tries < 8 &&
                                (size_t)m_timeline->findTileIndex((double)targetTime) != N; ++tries) {
                targetTime -= 0.25 * std::max(1e-9, span);
            }
        } else {
            LOG_W("capture: --capture-tile %d 超出 %zu 砖，退回 --capture-time", m_cfg->captureTile, nTiles);
        }
    }
    // **按关卡时间定位（double）**：`audioPos` 是 float，6.8e7 砖的谱面上分辨率只有 ~1e-4 s，
    // 会漂到隔壁砖（实测 singularity_mid 漂 3 块、门槛直接判"抓错砖"）。
    m_playback->startAtLevelTime(targetTime);
    // **抓帧定点自校正**：按砖号定位必须真的落在该砖上。`--capture-tile` 用 1/2 区间中点，
    // 但 2.64e6 BPM 的谱上砖区间极窄，float 级误差就会落到隔壁（实测 500000 → 500001/500003）。
    // 这里按**播放器自己报的** currentTileIndex 收敛，最多 8 次。
    if (m_cfg->captureTile >= 0) {
        const int64_t want = (int64_t)m_cfg->captureTile;
        for (int i = 0; i < 8; ++i) {
            const int64_t got = m_playback->currentTileIndex();
            if (got == want) break;
            const double dir = (got > want) ? -1.0 : 1.0;
            targetTime += dir * 0.25 * capSpan;
            m_playback->stop();
            m_playback->startAtLevelTime(targetTime);
        }
        if (m_playback->currentTileIndex() != want)
            LOG_W("capture: --capture-tile %lld 未能定点（落在 %lld）", (long long)want,
                  (long long)m_playback->currentTileIndex());
    }
    m_scene->applyFrame(m_playback->frame(), *m_timeline);

    int tileIdx = m_playback->currentTileIndex();
    if (tileIdx < 0) tileIdx = 0;
    // 结尾停在**真实最后一格**（tileCount 含末尾合成砖，减 2 才是最后一格）
    if (tileIdx >= (int)m_level->tileCount() - 1) tileIdx = (int)m_level->tileCount() - 2;
    if (m_cfg->captureCamera && m_level->tileCount() > 0) {
        // 与播放时同一条相机路径：settings + 到目标时刻为止的全部运镜事件。
        const auto p = m_level->tilePositionAt(tileIdx);
        if (env::flag("ADOCAO_CAM_DEBUG")) {
            LOG_I("capture-pos: tileIdx=%d p=(%.2f,%.2f) windowed=%d", tileIdx,
                  (double)p[0], (double)p[1], (int)m_level->tilesWindowed());
        }
        const double pitch = std::max(0.1, (double)m_level->settings.pitch / 100.0);
        m_cameraDirector.seek(m_playback->timeInLevel(), p[0], p[1], tileIdx, pitch);
        m_camCtrl.snapTo(m_cameraDirector.x(), m_cameraDirector.y());
        if (env::flag("ADOCAO_CAM_DEBUG")) {
            LOG_I("capture-cam: target=(%.2f,%.2f) zoom=%.2f", m_camera.targetX(), m_camera.targetY(), (double)m_camera.zoom());
        }
        m_camera.setRotation((float)m_cameraDirector.rotation());
        LOG_I("capture-selfcheck: 关卡相机 t=%.4f pos=(%.4f, %.4f) rot=%.3f zoom=%.3f",
              m_playback->timeInLevel(), m_cameraDirector.x(), m_cameraDirector.y(),
              m_cameraDirector.rotation(), m_cameraDirector.zoom());
    } else if (tileIdx >= 0 && tileIdx < (int)m_level->tileCount()) {
        m_camCtrl.snapTo(m_level->tilePositionAt(tileIdx)[0], m_level->tilePositionAt(tileIdx)[1]);
    }
    // `--capture-camera` 下 **zoom 由关卡相机说了算**（`--capture-zoom` 被忽略）——
    // 否则运镜的 zoom 事件会被 CLI 覆盖掉，等于没测。普通抓帧仍走 --capture-zoom。
    if (m_cfg->captureFitAll) {
        // 包住"变换后"的全部砖（原始位置 + 关键帧动画的位移），用于复现编辑器的全图构图。
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        const auto& tls = m_level->tiles;
        for (size_t i = 0; i < tls.size(); ++i) {
            double x = tls[i].position[0], y = tls[i].position[1];
            if (const adofai::TileAnimation* anim = m_scene->animation()) {
                adofai::TileAnimSample an{};
                if (anim->sample((int64_t)i, targetTime, an)) { x += an.posX; y += an.posY; }
            }
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
        const float pad = 1.5f;
        minX -= pad; maxX += pad; minY -= pad; maxY += pad;
        m_camCtrl.snapTo((minX + maxX) * 0.5, (minY + maxY) * 0.5);
        int fbw = 1280, fbh = 720;
        glfwGetFramebufferSize(m_window, &fbw, &fbh);
        const double aspect = (fbh > 0) ? (double)fbw / (double)fbh : 16.0 / 9.0;
        const double w = std::max(1e-6, maxX - minX), h = std::max(1e-6, maxY - minY);
        const double needH = std::max(h, w / aspect);
        m_camera.setZoom((float)(1200.0 / needH));   // 视野高 = 1200/zoom 砖
    } else if (m_cfg->captureCamera) m_camera.setAdoZoom((float)m_cameraDirector.zoom());
    else                      m_camera.setZoom(m_cfg->captureZoom);   // 本类口径：100 = 视野高 12 砖

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    m_camCtrl.update(fbW, fbH, m_targetAspect, true);   // 与主循环同一条相机路径（不播放 → pan 分支，但无输入）
    // 帧时间：glFinish 前后计时（只统计"这一帧的命令 + 等 GPU 做完"，不含加载/建 mesh）。
    // 第一帧自带一次性开销（形状表纹理上传、VAO/pipeline 首次绑定），所以想看**稳态**
    // 帧时间就设 `ADOCAO_CAPTURE_FRAMES=N`：多渲染 N-1 次，取重复帧的中位数当 render_ms，
    // 第一帧单独报成 first_ms。像素只取最后一次（同一状态重复渲染，逐位相同）。
    int frames = 1;
    if (const char* e = std::getenv("ADOCAO_CAPTURE_FRAMES")) { frames = std::atoi(e); }
    if (frames < 1) frames = 1;
    if (frames > 200) {
        LOG_W("capture: ADOCAO_CAPTURE_FRAMES=%d 超过上限 200，按 200 渲染", frames);
        frames = 200;
    }
    const auto tRender0 = std::chrono::steady_clock::now();
    profile::frameBegin();
    render();
    { adofai::profile::Scoped _s("glfinish"); glFinish(); }   // = 这一帧的 GPU 等完（CPU 提交之外的部分）
    profile::frameEnd();
    const auto tRender1 = std::chrono::steady_clock::now();
    const double firstMs = std::chrono::duration<double, std::milli>(tRender1 - tRender0).count();
    std::vector<double> reps;
    for (int f = 1; f < frames; f++) {
        const auto a = std::chrono::steady_clock::now();
        profile::frameBegin();
        render();
        { adofai::profile::Scoped _s("glfinish"); glFinish(); }
        profile::frameEnd();
        const auto b = std::chrono::steady_clock::now();
        reps.push_back(std::chrono::duration<double, std::milli>(b - a).count());
    }
    double renderMs = firstMs;
    if (!reps.empty()) {
        std::sort(reps.begin(), reps.end());
        renderMs = reps[reps.size() / 2];
    }
    const int dragged = m_scene->drawnInstances();

    // 3) 读像素 + 写 PNG（OpenGL 原点在左下，PNG 在左上 → 翻一次）
    const size_t rowBytes = (size_t)fbW * 4;
    std::vector<unsigned char> px(rowBytes * (size_t)fbH), flip(rowBytes * (size_t)fbH);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < fbH; y++)
        std::memcpy(&flip[(size_t)y * rowBytes], &px[(size_t)(fbH - 1 - y) * rowBytes], rowBytes);

    if (stbi_write_png(m_cfg->capturePath.c_str(), fbW, fbH, 4, flip.data(), (int)rowBytes)) {
        LOG_I("capture: %s %dx%d t=%.4fs zoom=%.1f tile=%d req=%d render_ms=%.3f first_ms=%.2f frames=%d drawn=%d",
              m_cfg->capturePath.c_str(), fbW, fbH, targetTime, m_cfg->captureZoom, tileIdx,
              m_cfg->captureTile, renderMs, firstMs, frames, dragged);
    } else {
        // 前缀**不能**是 `capture:`：`capture-gate.sh` 用"日志里最后一条 capture: 行"核对
        // tile/req，一条不是抓帧结果的 capture: 行会把解析带偏（真踩过：写 PNG 失败那次
        // 让 48 个状态报"抓错砖"）。抓帧块里的辅助日志统一用 capture-selfcheck:/capture-write:。
        LOG_E("capture-write: 写 PNG 失败 %s", m_cfg->capturePath.c_str());
    }
    if (profile::enabled()) {
            // 两条口径：首帧=一次性 GL 开销的单次快照；稳态=第 2..N 帧的 min/中位/max（抗单样本噪声）
            if (frames == 1) {
                const auto& f = profile::firstFrame();
                const double sum = profile::sumMs(f);
                LOG_I("profile(单帧): %s ｜段和 %.3f / 帧 %.3f（%.0f%%）", profile::reportFirst().c_str(),
                      sum, f.ms, f.ms > 0 ? 100.0 * sum / f.ms : 0.0);
                if (sum > f.ms * 1.05 + 0.5)
                    LOG_W("profile(单帧): 段和 %.3f ms > 该帧自量 %.3f ms —— 口径有问题（别信这张表）",
                          sum, f.ms);
            } else {
                LOG_I("profile(首帧): %s", profile::reportFirst().c_str());
                LOG_I("profile(稳态): %s", profile::reportSteady().c_str());
                const double secSum = profile::steadySectionSumMed();
                const double fmed = profile::steadyFrameMed();
                if (secSum > fmed * 1.05 + 0.5)
                    LOG_W("profile(稳态): 段中位和 %.3f ms > 帧中位 %.3f ms —— 口径有问题（别信这张表）",
                          secSum, fmed);
            }
        }

    // 4) **自检：同一帧再渲染一次，逐字节比** —— 这是"抓帧必须确定性"的机械保证。
    // 为什么这么做：与其去追"哪些量是反馈驱动的"（追不完 —— 帧时间、墙上时钟、音频位置、
    // GPU 计时、系统负载……），不如让抓帧**自己证明**这一帧是可复现的：任何 2026-10 那种
    // "拖尾自适应采样率"式的东西混进来，两次渲染就会不同，这里当场抓住，无需知道它是什么。
    {
        std::vector<unsigned char> px2(rowBytes * (size_t)fbH);
        render();
        glFinish();
        glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px2.data());
        if (std::memcmp(px.data(), px2.data(), px.size()) != 0) {
            size_t diffBytes = 0, firstBad = px.size();
            for (size_t i = 0; i < px.size(); ++i) {
                if (px[i] != px2[i]) { ++diffBytes; if (firstBad == px.size()) firstBad = i; }
            }
            LOG_E("capture-selfcheck: **自检失败** —— 同一帧两次渲染结果不同（首个差异字节 %zu，%zu/%zu 字节 = %.4f%%）："
                  "抓帧路径里混进了非确定性的输入（帧时间 / 墙上时钟 / 音频 / GPU 计时 / 负载…）。"
                  "这类量必须先写死（参考 trailAdaptive）再进抓帧路径。",
                  firstBad, diffBytes, px.size(), 100.0 * (double)diffBytes / (double)px.size());
            std::fflush(nullptr);
            std::exit(3);   // 明确失败：门槛报红，且 PNG 已写出便于诊断
        }
        LOG_I("capture-selfcheck: 自检通过（同一帧两次渲染逐字节相同 → 这一帧是可复现的）");
    }
}

namespace {
// 浸泡统计（"100% 从头到尾"那类整程跑）用的分位数：样本量到 10 万级也只是一次 sort
double pctOf(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    return v[i < v.size() ? i : v.size() - 1];
}
void soakLine(const char* tag, const std::vector<double>& work, const std::vector<double>& frame,
              const std::vector<int>& drawn, double overBudgetMs) {
    if (work.empty()) return;
    size_t over = 0;
    for (double w : work) if (w > overBudgetMs) ++over;
    std::vector<int> d = drawn;
    std::sort(d.begin(), d.end());
    const int dMed = d.empty() ? 0 : d[d.size() / 2];
    const int dMax = d.empty() ? 0 : d.back();
    LOG_I("soak(%s): 帧 %zu｜work p50=%.2f p95=%.2f p99=%.2f max=%.2f ms｜frame p50=%.2f p95=%.2f max=%.2f｜"
          ">%.1fms %zu 帧(%.2f%%)｜drawn 中位 %d 峰值 %d｜常驻 %.0f MB",
          tag, work.size(), pctOf(work, 0.50), pctOf(work, 0.95), pctOf(work, 0.99),
          pctOf(work, 1.0), pctOf(frame, 0.50), pctOf(frame, 0.95), pctOf(frame, 1.0),
          overBudgetMs, over, 100.0 * (double)over / (double)work.size(), dMed, dMax,
          (double)appmem::currentResidentBytes() / 1048576.0);
}
// ---- Profiler 的独立 CSV 文件（ADOCAO.prof） ---------------------------------
// 与 ADOCAO.log 完全分开：每次采样行**立即写入并 flush**（不攒到关窗口），
// 这样运行中就能用 tail/csv 转表。列见下面的表头。
struct ProfFile {
    FILE* f = nullptr;
    long long runId = 0;
    bool headerDone = false;

    void ensureOpen() {
        if (f || headerDone) return;
        headerDone = true;
        // 默认跟 ADOCAO.log 放同一个目录（exe 旁，或 macOS .app / 系统日志目录那套回退），
        // 而不是"当前工作目录"：用户报过"profiler 什么也没留下" —— 文件其实落在了他
        // 启动 .bat 时的 CWD 里，跟 exe/日志不在一处。日志里也打**绝对路径**，别再靠猜。
        std::filesystem::path path;
        if (const char* env = std::getenv("ADOCAO_PROFILE_FILE")) {
            if (*env) path = env;   // 相对 = 当前工作目录（显式给出就按给的算）
        }
        if (path.empty()) {
            const std::filesystem::path log(Logger::instance().path());
            path = (log.has_parent_path() ? log.parent_path() : std::filesystem::path("."))
                 / "ADOCAO.prof";
        }
        std::error_code ec;
        const auto absPath = std::filesystem::absolute(path, ec);
        const std::string shown = (ec ? path : absPath).string();
        f = std::fopen(shown.c_str(), "a");
        if (!f) { LOG_W("profiler: 打不开 %s（%s）", shown.c_str(), std::strerror(errno)); return; }
        runId = (long long)std::time(nullptr);
        std::fprintf(f, "# ADOCAO profiler v1 (CSV; 每行立即 flush)\n");
        std::fprintf(f,
            "kind,run,t_s,frames,work_p50_ms,work_p95_ms,work_p99_ms,work_max_ms,"
            "frame_p50_ms,frame_p95_ms,frame_max_ms,over_16_7,over_pct,drawn_med,drawn_max,"
            "rss_mb,segment,seg_p50_ms,seg_min_ms,seg_max_ms,note\n");
        std::fflush(f);
        LOG_I("profiler: 写入 %s（run=%lld，CSV，每行 flush）", shown.c_str(), runId);
    }
    void row(const char* fmt, ...) {
        ensureOpen();
        if (!f) return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fflush(f);   // 关键：每次直接写，不等关窗口
    }
    void close() { if (f) { std::fflush(f); std::fclose(f); f = nullptr; } }
};

// CSV 行构造：21 列（见表头）。空字段用空串。
inline std::string csvRow(const std::vector<std::string>& cols) {
    std::string out;
    for (size_t i = 0; i < cols.size(); i++) {
        if (i) out += ',';
        out += cols[i];
    }
    return out;
}
inline std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap; va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// 一行 interval/end 统计（CSV 列顺序见表头）。窗口为空时跳过。
void profStatsRow(ProfFile& pf, const char* kind, long long run, double t, size_t frames,
                  const std::vector<double>& work, const std::vector<double>& frame,
                  const std::vector<int>& drawn, double budgetMs) {
    if (work.empty()) return;
    size_t over = 0;
    for (double w : work) if (w > budgetMs) ++over;
    std::vector<int> d = drawn;
    std::sort(d.begin(), d.end());
    std::vector<std::string> c(21);
    c[0] = kind;
    c[1] = std::to_string(run);
    c[2] = fmt("%.3f", t);
    c[3] = std::to_string(frames);
    c[4] = fmt("%.3f", pctOf(work, 0.50));
    c[5] = fmt("%.3f", pctOf(work, 0.95));
    c[6] = fmt("%.3f", pctOf(work, 0.99));
    c[7] = fmt("%.3f", pctOf(work, 1.0));
    c[8] = fmt("%.3f", pctOf(frame, 0.50));
    c[9] = fmt("%.3f", pctOf(frame, 0.95));
    c[10] = fmt("%.3f", pctOf(frame, 1.0));
    c[11] = std::to_string(over);
    c[12] = fmt("%.2f", 100.0 * (double)over / (double)work.size());
    c[13] = d.empty() ? "" : std::to_string(d[d.size() / 2]);
    c[14] = d.empty() ? "" : std::to_string(d.back());
    c[15] = fmt("%.0f", (double)appmem::currentResidentBytes() / 1048576.0);
    pf.row("%s", csvRow(c).c_str());
}

// 退出时把每个段（cull/upload/draw/trailSample…）的稳态分位数写成独立行，便于透视表。
void profSegmentRows(ProfFile& pf, long long run, double t) {
    for (const auto& sec : profile::sections()) {
        if (sec.samples.empty()) continue;
        std::vector<double> v = sec.samples;
        std::sort(v.begin(), v.end());
        std::vector<std::string> c(21);
        c[0] = "segment"; c[1] = std::to_string(run); c[2] = fmt("%.3f", t);
        c[16] = sec.name;
        c[17] = fmt("%.3f", pctOf(v, 0.50));
        c[18] = fmt("%.3f", v.front());
        c[19] = fmt("%.3f", v.back());
        pf.row("%s", csvRow(c).c_str());
    }
    if (!profile::frameSamples().empty()) {
        std::vector<double> v = profile::frameSamples();
        std::sort(v.begin(), v.end());
        std::vector<std::string> c(21);
        c[0] = "frame"; c[1] = std::to_string(run); c[2] = fmt("%.3f", t);
        c[3] = std::to_string(v.size());
        c[8] = fmt("%.3f", pctOf(v, 0.50));
        c[9] = fmt("%.3f", pctOf(v, 0.95));
        c[10] = fmt("%.3f", pctOf(v, 1.0));
        pf.row("%s", csvRow(c).c_str());
    }
}

}  // namespace

void GameWindow::run() {
    // 开发：确定性抓帧优先于主循环（--capture）
    if (!m_cfg->capturePath.empty()) {
        captureAndExit();
        m_audioEngine->shutdown();
        m_scene.reset();
        glfwDestroyWindow(m_window);
        return;
    }

    // `--start-at`：等价于"选砖后按空格"——时钟/音乐/音色都 seek 到该时刻再开播。
    if (m_cfg->startAt > 0.0f) {
        const float audioPos = m_cfg->startAt + m_playback->preRoll() - m_playback->audioStartOffset();
        m_playback->startAt(glfwGetTime(), audioPos, 0.0f);
        m_hitsoundMgr->resetAt(audioPos);
        if (m_audioEngine->hasMusic()) {
            m_audioEngine->seek(audioPos);
            m_audioEngine->play();
        }
        LOG_I("start-at: %.3fs（audioPos=%.3fs）", (double)m_cfg->startAt, (double)audioPos);
    }

    double targetFrameTime = 1.0 / 320.0;
    // 诊断：`ADOCAO_UNCAPPED=1` 关掉 320fps 的帧率上限（profiler 要量机器真实上限）。
    // 不设 = 行为完全不变。按首个字符判断（与 ADOCAO_PROFILE 同口径）：
    // `ADOCAO_UNCAPPED=0` 必须是**关**，不能因为"设了"就开 —— 老写法是 getenv() 非空即开。
    if (env::flag("ADOCAO_UNCAPPED")) {
        targetFrameTime = 0.0;
        LOG_I("帧率上限已关闭（ADOCAO_UNCAPPED=1）");
    } else {
        LOG_I("帧率上限：320 fps（ADOCAO_UNCAPPED=1 关闭节流）");
    }
    // 诊断钩子：跑到 N 毫秒后**正常退出**（好让 `profile::reportSteady()` 打分段报告）。
    // 只给 `ADOCAO_PROFILE=1` 的全流程剖面用；不设 = 行为完全不变。
    double exitAfterMs = 0.0;
    if (const char* e = std::getenv("ADOCAO_EXIT_AFTER_MS")) exitAfterMs = std::atof(e);
    const double runT0 = glfwGetTime();

    std::vector<double> soakWork, soakFrame;
    std::vector<int> soakDrawn;
    double soakLastLog = 0.0;
    ProfFile prof;                        // ADOCAO.prof（CSV，逐行 flush）
    std::vector<double> ivWork, ivFrame;  // 只装"本采样间隔"的帧（CSV 要看随时间变化）
    std::vector<int> ivDrawn;
    if (profile::enabled()) prof.ensureOpen();
    // 采样间隔可调：`ADOCAO_PROFILE_INTERVAL_MS`（默认 10000）。只影响"每 N 秒打一行"，
    // 不影响逐帧采样本身（每帧都计入；结尾的 reportSteady 也是整段）。
    double soakIntervalSec = 10.0;
    if (const char* e = std::getenv("ADOCAO_PROFILE_INTERVAL_MS")) {
        const double v = std::atof(e);
        if (v >= 100.0) soakIntervalSec = v / 1000.0;
    }
    char soakTag[32];
    std::snprintf(soakTag, sizeof(soakTag), "%gs", soakIntervalSec);

    while (!glfwWindowShouldClose(m_window)) {
        const double loopT0 = glfwGetTime();
        glfwPollEvents();
#ifdef __APPLE__
        // 全屏进入要等窗口真正上屏（见 GameWindow.hpp 的成员注释）；非 Apple 平台没有这条路径。
        if (m_pendingFullscreenEnter) {
            m_pendingFullscreenEnter = false;
            macosBorderlessEnter(m_window);
            glfwFocusWindow(m_window);   // 进全屏后同样要抢焦点
        }
#endif
        handleInput();

        // Frame pacing
        double now = glfwGetTime();
        double elapsed = now - m_lastFrameTime;
        if (elapsed < targetFrameTime && elapsed > 0) {
            double remaining = targetFrameTime - elapsed;
            if (remaining > 0.002)
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.001));
            while ((now = glfwGetTime()) < m_lastFrameTime + targetFrameTime) {}
            elapsed = targetFrameTime;
        }
        float deltaMs = (float)(elapsed * 1000.0);
        m_lastFrameTime = now;
        if (deltaMs > 500.0f) deltaMs = 0.0f;
        else if (deltaMs > 100.0f) deltaMs = 100.0f;

        {
            // Time the real work (update+render, excluding the vsync wait) so
            // the scene can auto-tune the trail sample rate.
            profile::frameBegin();
            double t0 = glfwGetTime();
            update(deltaMs);
            render();
            double workMs = (glfwGetTime() - t0) * 1000.0;
            profile::frameEnd();
            m_scene->setMeasuredFrameMs(workMs);
            glfwSwapBuffers(m_window);
            if (exitAfterMs > 0.0 && (glfwGetTime() - runT0) * 1000.0 >= exitAfterMs) {
                LOG_I("run: ADOCAO_EXIT_AFTER_MS=%.0f 到点，正常退出（输出分段 profile）", exitAfterMs);
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            if (profile::enabled()) {
                // 浸泡统计：`work` = update+render，`frame` = 算上 swap/pacing 的整圈
                soakWork.push_back(workMs);
                soakFrame.push_back((glfwGetTime() - loopT0) * 1000.0);
                soakDrawn.push_back(m_scene->drawnInstances());
                ivWork.push_back(workMs);
                ivFrame.push_back(soakFrame.back());
                ivDrawn.push_back(soakDrawn.back());
                const double nowS = glfwGetTime();
                if (nowS - soakLastLog >= soakIntervalSec) {
                    soakLastLog = nowS;
                    soakLine(soakTag, soakWork, soakFrame, soakDrawn, 1000.0 / 60.0);
                    // .prof：本间隔一行（立即 flush）
                    profStatsRow(prof, "interval", prof.runId, nowS - runT0, ivWork.size(),
                                 ivWork, ivFrame, ivDrawn, 1000.0 / 60.0);
                    ivWork.clear(); ivFrame.clear(); ivDrawn.clear();
                }
            }
        }
    }
    if (profile::enabled()) {
        soakLine("结束", soakWork, soakFrame, soakDrawn, 1000.0 / 60.0);
        LOG_I("soak(结束-分段): %s", profile::reportSteady().c_str());
        const double tEnd = glfwGetTime() - runT0;
        profStatsRow(prof, "end", prof.runId, tEnd, soakWork.size(), soakWork, soakFrame,
                     soakDrawn, 1000.0 / 60.0);
        profSegmentRows(prof, prof.runId, tEnd);
        prof.close();
    }

    m_audioEngine->shutdown();
    // Cleanup GL objects while the context is still current
    m_scene.reset();
#ifdef __APPLE__
    // **必须在 destroyWindow 之前**：macosBorderlessExit 会碰 NSWindow
    // （glfwGetCocoaWindow），销毁之后再调就是 dangling pointer。
    if (!m_nativeCleanupDone && m_isFullscreen) { macosBorderlessExit(m_window); }
    m_nativeCleanupDone = true;
#endif
    glfwDestroyWindow(m_window);
}

void showGameWindow(const LauncherConfig& cfg, LoadResult& result) {
    GameWindow gw;
    if (gw.init(cfg, result)) gw.run();
}
