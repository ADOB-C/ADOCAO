#pragma once

#include "CameraController.hpp"
#include "render/Camera.hpp"
#include <memory>

struct GLFWwindow;
struct LauncherConfig;
struct LoadResult;
namespace adofai { class LevelData; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Timeline; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class PlaybackClock; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class HitsoundManager; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class AudioEngine; }   // 库侧类型：前置声明也要在 adofai 里
class LevelScene;

void showGameWindow(const LauncherConfig& cfg, LoadResult& loadResult);

// Game window frame loop. Owns the window + GL context and the per-frame
// orchestration: input/playback controls, music↔clock sync, and delegating
// presentation to LevelScene (camera logic lives in CameraController).
class GameWindow {
public:
    GameWindow() = default;
    ~GameWindow();   // out-of-line: LevelScene is only forward-declared here

    bool init(const LauncherConfig& cfg, LoadResult& loadResult);
    // 砖块几何的 GLSL 路需要 GLSL 4.00+（`fma()`）。
    bool gpuTileGeometryUsable() const { return m_glMajor >= 4; }
    int  glMajor() const { return m_glMajor; }
    int  glMinor() const { return m_glMinor; }
    void run();

private:
    GLFWwindow* m_window = nullptr;
    GLFWwindow* m_sharedWindow = nullptr;

    // Scene (shaders / tile mesh / planets / trails) + camera navigation
    std::unique_ptr<LevelScene> m_scene;
    adofai::Camera m_camera;
    CameraController m_camCtrl;   // attached to m_camera in init()

    const LauncherConfig* m_cfg = nullptr;
    adofai::LevelData* m_level = nullptr;
    adofai::Timeline* m_timeline = nullptr;
    adofai::PlaybackClock* m_playback = nullptr;
    adofai::HitsoundManager* m_hitsoundMgr = nullptr;
    adofai::AudioEngine* m_audioEngine = nullptr;

    // Playback/UI state
    int m_selectedTile = -1;
    bool m_wasSpacePressed = false;
    bool m_wasAltEnterPressed = false;
    double m_autoPlayTriggerTime = 0;
    bool m_musicPending = false;
    bool m_exclusiveFullscreen = true;
    bool m_isFullscreen = false;
    // 实际拿到的 GL 版本（4.1 拿不到时会退到 3.3，那时砖块几何走 CPU 回退路）。
    int m_glMajor = 0, m_glMinor = 0;
    int m_windowedX = 0, m_windowedY = 0;
    int m_windowedW = 1920, m_windowedH = 1080;
    float m_targetAspect = 16.0f / 9.0f;
    float m_bgR = 0, m_bgG = 0, m_bgB = 0;
    double m_lastFrameTime = 0;

    void navigateToTile(int floor);
    void handleInput();
    void update(float deltaMs);
    void render();
    void toggleFullscreen();
    void releaseMeshTemporaries();

    // 开发：确定性抓帧（--capture）—— 固定时刻/相机/缩放渲染一帧写 PNG 后退出。
    // 只给"像素 diff"当验收工具用，不进正常主循环。
    void captureAndExit();
};
