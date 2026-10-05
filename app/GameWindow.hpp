#pragma once

#include "CameraController.hpp"
#include "render/Camera.hpp"
#include <memory>

struct GLFWwindow;
struct LauncherConfig;
struct LoadResult;
class LevelData;
class Timeline;
class PlaybackClock;
class HitsoundManager;
class AudioEngine;
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
    void run();

private:
    GLFWwindow* m_window = nullptr;
    GLFWwindow* m_sharedWindow = nullptr;

    // Scene (shaders / tile mesh / planets / trails) + camera navigation
    std::unique_ptr<LevelScene> m_scene;
    Camera m_camera;
    CameraController m_camCtrl;   // attached to m_camera in init()

    const LauncherConfig* m_cfg = nullptr;
    LevelData* m_level = nullptr;
    Timeline* m_timeline = nullptr;
    PlaybackClock* m_playback = nullptr;
    HitsoundManager* m_hitsoundMgr = nullptr;
    AudioEngine* m_audioEngine = nullptr;

    // Playback/UI state
    int m_selectedTile = -1;
    bool m_wasSpacePressed = false;
    bool m_wasAltEnterPressed = false;
    double m_autoPlayTriggerTime = 0;
    bool m_musicPending = false;
    bool m_exclusiveFullscreen = true;
    bool m_isFullscreen = false;
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
