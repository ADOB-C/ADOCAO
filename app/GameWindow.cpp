#include "GameWindow.hpp"

#include "LauncherWindow.hpp"
#include "LevelLoader.hpp"
#include "LevelScene.hpp"
#include "glad/gl_core.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/util/Logger.hpp"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// 实现（STB_IMAGE_WRITE_IMPLEMENTATION）在 app/MapExport.cpp —— 同一个二进制里只留一份。
#include "stb_image_write.h"

namespace {

// Start playback from the given floor (used for Space-from-selected-tile).
static void jumpToTile(Timeline& timeline, PlaybackClock& clock, AudioEngine& audio, HitsoundManager& hs,
                        const LevelData& level, int floor) {
    if (floor < 0 || floor >= (int)level.tiles.size()) return;
    double targetTime = timeline.tileStartTimes()[floor];
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

GameWindow::~GameWindow() = default;

bool GameWindow::init(const LauncherConfig& cfg, LoadResult& result) {
    m_cfg = &cfg;
    m_level = result.level.get();
    m_timeline = result.timeline.get();
    m_playback = result.playback.get();
    m_hitsoundMgr = &result.hitsounds;
    m_audioEngine = &result.audio;
    m_targetAspect = (float)cfg.resolutionW / (float)cfg.resolutionH;
    LOG_D("GameWindow::init: %zu tiles, fullscreen=%d exclusive=%d", m_level->tiles.size(), cfg.fullscreen, cfg.exclusiveFullscreen);

    // Create window
    m_exclusiveFullscreen = cfg.exclusiveFullscreen;
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
    int winW = (int)(cfg.resolutionW / csx); if (winW < 1) winW = 1;
    int winH = (int)(cfg.resolutionH / csy); if (winH < 1) winH = 1;
    int screenW = mode ? (int)(mode->width / csx) : winW; if (screenW < 1) screenW = 1;
    int screenH = mode ? (int)(mode->height / csy) : winH; if (screenH < 1) screenH = 1;
    int fsW = mode ? screenW : winW;
    int fsH = mode ? screenH : winH;
    m_windowedW = winW;
    m_windowedH = winH;

    if (cfg.msaaSamples > 0) glfwWindowHint(GLFW_SAMPLES, cfg.msaaSamples);
    // 4.1 而不是 3.3：砖块几何的 VS 要用 `fma()`（GLSL 4.00+）来逐字镜像参考实现的融合结构。
    // 4.1 是 macOS 的上限，也是本机实测**像素中性**的（34 个验收状态在 3.3 与 4.1 下逐字节相同）。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    if (cfg.fullscreen) {
        if (cfg.exclusiveFullscreen) {
            // Exclusive fullscreen: GPU dedicated to this app, mode switch
            glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
            m_window = glfwCreateWindow(fsW, fsH, "ADOCAO", primary, nullptr);
        } else {
            // Borderless windowed fullscreen: compositor still active
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            m_window = glfwCreateWindow(fsW, fsH, "ADOCAO", nullptr, nullptr);
            glfwSetWindowPos(m_window, 0, 0);
        }
    } else {
        glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
        m_window = glfwCreateWindow(winW, winH, "ADOCAO", nullptr, nullptr);
        if (primary) {
            // Center using the monitor work area in logical points
            int wx, wy, ww, wh;
            glfwGetMonitorWorkarea(primary, &wx, &wy, &ww, &wh);
            glfwSetWindowPos(m_window, wx + (ww - winW) / 2, wy + (wh - winH) / 2);
            m_windowedX = wx + (ww - winW) / 2;
            m_windowedY = wy + (wh - winH) / 2;
        }
    }
    if (!m_window) { LOG_E("Failed to create game window"); return false; }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(0);
    if (!loadGLCore()) { LOG_E("Failed to load OpenGL functions"); glfwDestroyWindow(m_window); return false; }
    if (cfg.msaaSamples > 0) glEnable(GL_MULTISAMPLE);
    LOG_D("OpenGL %s | GLSL %s", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    // Detect GPU: only NVIDIA has reliable GL context sharing across threads.
    // Intel & AMD iGPUs often fail to share VAO/VBO between contexts → sync build.
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    LOG_D("GPU vendor=%s renderer=%s", vendor ? vendor : "?", renderer ? renderer : "?");
    bool useAsyncBuild = false;
    if (vendor) {
        std::string v(vendor);
        useAsyncBuild = (v.find("NVIDIA") != std::string::npos);
    }
    LOG_D("Async build: %s", useAsyncBuild ? "ON" : "OFF (sync)");

    // Show window immediately so user sees it while heavy init runs
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glfwSwapBuffers(m_window);

    // Scene: shaders / tile mesh / planets (heap objects freed by LevelScene)
    m_scene = std::make_unique<LevelScene>();
    if (!m_scene->init(cfg, *m_level)) {
        // Shader compile failed; fall back strings are already exhausted
        m_scene.reset();
        glfwDestroyWindow(m_window);
        return false;
    }

    // Camera + background
    {
        std::string hex = cfg.backgroundColor;
        if (hex.length()>=6) { unsigned r,g,b; sscanf(hex.c_str(),"%02x%02x%02x",&r,&g,&b);
            m_bgR=r/255.0f; m_bgG=g/255.0f; m_bgB=b/255.0f; }
    }
    m_camCtrl.attach(m_camera);
    m_camera.setZoom(m_level->settings.zoom);
    if (!m_level->tiles.empty()) {
        auto& t = m_level->tiles[0];
        m_camCtrl.snapTo(t.position[0], t.position[1]);
    }

    // Mesh build: async on a shared GL context (NVIDIA), sync otherwise
    if (useAsyncBuild) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_SAMPLES, 0);
        m_sharedWindow = glfwCreateWindow(1, 1, "buildctx", nullptr, m_window);
        if (m_sharedWindow) {
            LOG_D("Spawning background mesh build thread");
            if (!m_scene->beginAsyncBuild(*m_level, m_sharedWindow)) {
                LOG_W("Async build failed to start, falling back to sync build");
                glfwDestroyWindow(m_sharedWindow);
                m_sharedWindow = nullptr;
            }
        } else {
            LOG_W("Shared context creation failed, falling back to sync build");
        }
    }
    if (!m_sharedWindow) {
        // Sync: build on main thread (window already visible)
        m_scene->buildSync(*m_level);
        releaseMeshTemporaries();
    }

    // Hitsound attach
    if (m_hitsoundMgr->isSynthesized()) {
        m_audioEngine->attachExternal(m_hitsoundMgr->buffer(), m_hitsoundMgr->totalFrames(),
            m_hitsoundMgr->channels(), m_hitsoundMgr->sampleRate(),
            m_hitsoundMgr->cursor(), m_hitsoundMgr->playing());
    }

    // Input callbacks → CameraController (pan/zoom/click state)
    glfwSetWindowUserPointer(m_window, &m_camCtrl);
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* w, int b, int a, int) {
        auto* ctrl = static_cast<CameraController*>(glfwGetWindowUserPointer(w));
        if (b == GLFW_MOUSE_BUTTON_LEFT)
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

    m_lastFrameTime = glfwGetTime();
    m_autoPlayTriggerTime = glfwGetTime() + (cfg.autoPlay ? 0.5 : 999999.0);
    return true;
}

void GameWindow::navigateToTile(int floor) {
    if (floor < 0 || floor >= (int)m_level->tiles.size()) return;
    auto& t = m_level->tiles[floor];
    m_camCtrl.snapTo(t.position[0], t.position[1]);
    m_selectedTile = floor;
}

void GameWindow::handleInput() {
    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);

    // Alt+Enter toggles fullscreen
    bool altEnter = (glfwGetKey(m_window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS
                  || glfwGetKey(m_window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS)
                 && glfwGetKey(m_window, GLFW_KEY_ENTER) == GLFW_PRESS;
    if (altEnter && !m_wasAltEnterPressed) {
        toggleFullscreen();
    }
    m_wasAltEnterPressed = altEnter;

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
        glm::dvec2 world = m_camCtrl.screenToWorld(m_camCtrl.cursorX(), m_camCtrl.cursorY(),
                                                   fbW, fbH, m_targetAspect);

        int best = -1; double bestDist = 1.0;
        for (int i = 0; i < (int)m_level->tiles.size()-1; i++) {
            double dx = m_level->tiles[i].position[0] - world.x;
            double dy = m_level->tiles[i].position[1] - world.y;
            double d = dx*dx + dy*dy;
            if (d < bestDist*bestDist) { bestDist = std::sqrt(d); best = i; }
        }
        if (best >= 0) navigateToTile(best);
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
                int nTiles = (int)m_level->tiles.size() - 1;   // skip synthetic last tile
                for (int i = 0; i < nTiles; i++) {
                    double dx = m_level->tiles[i].position[0] - cx;
                    double dy = m_level->tiles[i].position[1] - cy;
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
        int tn=(int)m_level->tiles.size()-1;
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

void GameWindow::update(float) {
    double now = glfwGetTime();

    // Playback update
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

    if (m_playback->isPlaying()) {
        m_scene->applyFrame(m_playback->frame(), *m_timeline);
    }

    // Camera follow during playback
    if (m_playback->isPlaying()) {
        int tileIdx = m_playback->currentTileIndex();
        if (tileIdx >= 0 && tileIdx < (int)m_level->tiles.size()) {
            auto& p = m_level->tiles[tileIdx].position;
            m_camCtrl.snapTo(p[0], p[1]);
        }
    }

    // Pan/drag (only when not playing)
    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    m_camCtrl.update(fbW, fbH, m_targetAspect, !m_playback->isPlaying());
}

void GameWindow::render() {
    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    LetterboxedViewport vp = computeLetterbox(fbW, fbH, m_targetAspect);

    glViewport(0, 0, fbW, fbH);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);

    glViewport(vp.x, vp.y, vp.w, vp.h);
    glScissor(vp.x, vp.y, vp.w, vp.h);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(m_bgR, m_bgG, m_bgB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    m_camera.setAspect((float)vp.w, (float)vp.h);

    bool playing = m_playback->isPlaying();
    m_scene->render(m_camera, *m_timeline, playing, m_playback->timeInLevel(),
                    playing ? -1 : m_selectedTile);
    // (glfwSwapBuffers is called by run() so it can time the work separately
    // from the vsync/pacing wait.)
}

void GameWindow::toggleFullscreen() {
    if (m_isFullscreen) {
        // Go windowed
        glfwSetWindowMonitor(m_window, nullptr,
            m_windowedX, m_windowedY, m_windowedW, m_windowedH, 0);
        m_isFullscreen = false;
    } else {
        // Save windowed position and size
        glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
        glfwGetWindowSize(m_window, &m_windowedW, &m_windowedH);

        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primary);
        if (!mode) return;

        // Convert physical pixel mode to logical points (Retina content scale)
        float csx = 1.0f, csy = 1.0f;
        glfwGetMonitorContentScale(primary, &csx, &csy);
        int fsW = (int)(mode->width / (csx > 0.0f ? csx : 1.0f));
        int fsH = (int)(mode->height / (csy > 0.0f ? csy : 1.0f));

        glfwSetWindowMonitor(m_window, primary, 0, 0, fsW, fsH, GLFW_DONT_CARE);
        m_isFullscreen = true;
    }
}

void GameWindow::releaseMeshTemporaries() {
    std::vector<double>().swap(m_level->angleData);
    m_level->tileBPMs.clear(); m_level->tileBPMs.shrink_to_fit();
    m_level->tileHasTwirl.clear(); m_level->tileHasTwirl.shrink_to_fit();
    m_level->tileHasSetSpeed.clear(); m_level->tileHasSetSpeed.shrink_to_fit();
}

// 确定性抓帧：固定"关卡时刻 + 相机 + 缩放"渲染**一帧**，写 PNG 后退出。
//
// 为什么不能用主循环：主循环里 update() 会 `m_playback->updateWallClock(glfwGetTime())`
// 把时刻重新锚到墙上时钟，还会 `setMeasuredFrameMs()` 喂拖尾自适应 → 两次跑不可能逐字节相同。
// 这里绕开这一切：直接 startAt 一个绝对时刻，之后**不再调用** update()，只手动做
// "给场景一帧 + 相机 snap + 抓像素"。所以同一组参数两次跑必须 byte-identical —— 这是
// `--capture` 存在的唯一理由（给"GPU 实例滑窗"这类改动当像素级验收）。
void GameWindow::captureAndExit() {
    // 1) 等网格建好（macOS 同步建；Windows/Linux 可能是共享上下文异步建）
    for (int i = 0; i < 120000 && !m_scene->meshReady(); i++) {
        glfwPollEvents();
        if (m_sharedWindow && m_scene->pollAsyncBuild()) {
            glfwDestroyWindow(m_sharedWindow);
            m_sharedWindow = nullptr;
            releaseMeshTemporaries();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!m_scene->meshReady()) { LOG_W("capture: 网格没建好，放弃"); return; }

    // 2) 摆到目标时刻：timeInLevel = elapsed/1000 - preRoll，elapsed = (audioPos + audioStartOffset)*1000
    //    → audioPos = T + preRoll - audioStartOffset。wallClock 传 0：之后不再有 wall clock 参与。
    //    `--capture-tile N` 把 T 换成第 N 砖的起始时刻：6 万砖的验收谱面上，按秒给的时刻会随
    //    砖时长累积漂移，按砖给则永远落在同一砖（相机会 snap 到它），门槛不受时间轴数学影响。
    float targetTime = m_cfg->captureTime;
    if (m_cfg->captureTile >= 0) {
        const auto& st = m_timeline->tileStartTimes();
        size_t N = (size_t)m_cfg->captureTile;
        if (N < st.size()) {
            // 取这一砖区间的**中点**（用 st[N+1]-st[N]，不要用 tileDurations —— 中旋砖的
            // 时长与实际区间不是一回事，会落到隔壁砖）。audioPos 是 float（千秒量级只有
            // ~1e-4 s 分辨率），落在区间起点上会被舍到上一砖，currentTileIndex() 就变成 N-1。
            double span = (N + 1 < st.size()) ? (st[N + 1] - st[N]) : 0.0;
            targetTime = (float)(st[N] + 0.5 * span);
        } else {
            LOG_W("capture: --capture-tile %d 超出 %zu 砖，退回 --capture-time", m_cfg->captureTile, st.size());
        }
    }
    const float audioPos = targetTime + m_playback->preRoll() - m_playback->audioStartOffset();
    m_playback->startAt(0.0, audioPos, 0.0f);
    m_scene->applyFrame(m_playback->frame(), *m_timeline);

    int tileIdx = m_playback->currentTileIndex();
    if (tileIdx >= 0 && tileIdx < (int)m_level->tiles.size())
        m_camCtrl.snapTo(m_level->tiles[tileIdx].position[0], m_level->tiles[tileIdx].position[1]);
    m_camera.setZoom(m_cfg->captureZoom);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    m_camCtrl.update(fbW, fbH, m_targetAspect, true);   // 与主循环同一条相机路径（不播放 → pan 分支，但无输入）
    render();
    glFinish();

    // 3) 读像素 + 写 PNG（OpenGL 原点在左下，PNG 在左上 → 翻一次）
    const size_t rowBytes = (size_t)fbW * 4;
    std::vector<unsigned char> px(rowBytes * (size_t)fbH), flip(rowBytes * (size_t)fbH);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < fbH; y++)
        std::memcpy(&flip[(size_t)y * rowBytes], &px[(size_t)(fbH - 1 - y) * rowBytes], rowBytes);

    if (stbi_write_png(m_cfg->capturePath.c_str(), fbW, fbH, 4, flip.data(), (int)rowBytes))
        LOG_I("capture: %s %dx%d t=%.4fs zoom=%.1f tile=%d req=%d", m_cfg->capturePath.c_str(),
              fbW, fbH, targetTime, m_cfg->captureZoom, tileIdx, m_cfg->captureTile);
    else
        LOG_W("capture: 写 PNG 失败 %s", m_cfg->capturePath.c_str());
}

void GameWindow::run() {
    // 开发：确定性抓帧优先于主循环（--capture）
    if (!m_cfg->capturePath.empty()) {
        captureAndExit();
        m_audioEngine->shutdown();
        m_scene.reset();
        glfwDestroyWindow(m_window);
        return;
    }

    double targetFrameTime = 1.0 / 320.0;

    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        handleInput();

        // Check async build completion (NVIDIA/AMD only)
        if (!m_scene->meshReady() && m_sharedWindow) {
            if (m_scene->pollAsyncBuild()) {
                glfwDestroyWindow(m_sharedWindow);
                m_sharedWindow = nullptr;
                releaseMeshTemporaries();
                LOG_D("Async mesh build complete");
            }
        }

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

        if (m_scene->meshReady()) {
            // Time the real work (update+render, excluding the vsync wait) so
            // the scene can auto-tune the trail sample rate.
            double t0 = glfwGetTime();
            update(deltaMs);
            render();
            double workMs = (glfwGetTime() - t0) * 1000.0;
            m_scene->setMeasuredFrameMs(workMs);
            glfwSwapBuffers(m_window);
        } else {
            // Still building async: show background color, keep responsive
            int fbW, fbH;
            glfwGetFramebufferSize(m_window, &fbW, &fbH);
            glViewport(0, 0, fbW, fbH);
            glClearColor(m_bgR, m_bgG, m_bgB, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glfwSwapBuffers(m_window);
        }
    }

    m_audioEngine->shutdown();
    // Cleanup GL objects while the context is still current
    m_scene.reset();
    glfwDestroyWindow(m_window);
}

void showGameWindow(const LauncherConfig& cfg, LoadResult& result) {
    GameWindow gw;
    if (gw.init(cfg, result)) gw.run();
}
