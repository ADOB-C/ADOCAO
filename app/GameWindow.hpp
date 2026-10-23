#pragma once

#include "CameraController.hpp"
#include "render/Camera.hpp"
#include "core/camera/CameraDirector.hpp"
#include <memory>

struct GLFWwindow;
struct LauncherConfig;
struct LoadResult;
namespace adofai { struct LevelData; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Timeline; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class PlaybackClock; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class HitsoundManager; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class AudioEngine; }
namespace adofai { class LevelScene; }   // 库侧类型：前置声明也要在 adofai 里


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
    void run();

private:
    GLFWwindow* m_window = nullptr;

    // Scene (shaders / tile mesh / planets / trails) + camera navigation
    std::unique_ptr<adofai::LevelScene> m_scene;
    adofai::Camera m_camera;
    CameraController m_camCtrl;   // attached to m_camera in init()
    // 关卡相机（docs/camera.md）：settings 是第一层，MoveCamera 时间线是第二层。
    // 只在播放/抓帧（--capture-camera）时驱动；停下后回到用户拖拽的导航相机。
    const adofai::CameraTimeline* m_cameraTimeline = nullptr;   // 归 LoadResult 所有
    adofai::CameraDirector m_cameraDirector;
    bool   m_wasPlaying = false;

    const LauncherConfig* m_cfg = nullptr;
    adofai::LevelData* m_level = nullptr;
    adofai::Timeline* m_timeline = nullptr;
    adofai::PlaybackClock* m_playback = nullptr;
    adofai::HitsoundManager* m_hitsoundMgr = nullptr;
    adofai::AudioEngine* m_audioEngine = nullptr;

    // Playback/UI state
    int m_selectedTile = -1;
    bool m_wasSpacePressed = false;
    double m_autoPlayTriggerTime = 0;
    bool m_musicPending = false;
    bool m_isFullscreen = false;
#ifdef __APPLE__
    // 全屏进入要**等窗口真正上屏**再做（SDL 的 pendingWindowOperation）：
    // 建完窗口后一帧再进全屏（窗口还没上屏时设置 frame 会被系统忽略）。
    bool m_pendingFullscreenEnter = false;
    bool m_nativeCleanupDone = false;   // run() 收尾已做过原生清理（析构别再碰 window）
#endif
    // 实际拿到的 GL 版本（4.1 拿不到时会退到 3.3，那时砖块几何走 CPU 回退路）。
    int m_windowedX = 0, m_windowedY = 0;
    int m_windowedW = 1920, m_windowedH = 1080;
    float m_targetAspect = 16.0f / 9.0f;
    float m_bgR = 0, m_bgG = 0, m_bgB = 0;
    double m_lastFrameTime = 0;

    void navigateToTile(int floor);
    void rebuildCameraTimeline();
    void handleInput();
    void update(float deltaMs);
    void render();

    // 开发：确定性抓帧（--capture）—— 固定时刻/相机/缩放渲染一帧写 PNG 后退出。
    // 只给"像素 diff"当验收工具用，不进正常主循环。
    void captureAndExit();
};
