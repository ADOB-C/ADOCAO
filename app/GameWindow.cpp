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
#include <thread>
#include <vector>

namespace {

// Start playback from the given floor (used for Space-from-selected-tile).
static void jumpToTile(Timeline& timeline, PlaybackClock& clock, AudioEngine& audio, HitsoundManager& hs,
                        const LevelData& level, int floor) {
    if (floor < 0 || floor >= (int)level.tiles.size()) return;
    double targetTime = timeline.tileStartTimes()[floor];
    float offsetSec = level.settings.offset / 1000.0f;
    float audioPos = (float)(targetTime + offsetSec);
    if (audioPos < 0) audioPos = 0;
    clock.startAt(glfwGetTime(), audioPos, offsetSec);
    hs.resetAt(audioPos);
    if (audio.hasMusic()) { audio.seek(audioPos); audio.play(); }
    else audio.play();
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
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
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

    // Bookmark navigation: Ctrl+Left/Right with long-press repeat (only when stopped)
    if (!m_playback->isPlaying() && !m_level->bookmarkFloors.empty()) {
        static double bmLHoldStart = 0, bmRHoldStart = 0;
        bool bmL=(glfwGetKey(m_window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS || glfwGetKey(m_window,GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS)
              && (glfwGetKey(m_window,GLFW_KEY_LEFT)==GLFW_PRESS);
        bool bmR=(glfwGetKey(m_window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS || glfwGetKey(m_window,GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS)
              && (glfwGetKey(m_window,GLFW_KEY_RIGHT)==GLFW_PRESS);
        double now = glfwGetTime();
        auto jumpBM = [&](bool left) {
            int cur=m_selectedTile, target=-1;
            if (left) { for (int b : m_level->bookmarkFloors) { if (b<cur) target=b; else break; } }
            else      { for (int b : m_level->bookmarkFloors) { if (b>cur) { target=b; break; } } }
            if (target>=0) navigateToTile(target);
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
    bool ctrlHeld = (glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
                 || (glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
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

    glfwSwapBuffers(m_window);
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

void GameWindow::run() {
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
            update(deltaMs);
            render();
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
