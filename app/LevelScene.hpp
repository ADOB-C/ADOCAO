#pragma once

#include "core/util/Scheduler.hpp"   // Handle 是**按值**成员 → 需要完整类型

#include <future>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;
struct LauncherConfig;
namespace adofai {

class Scheduler; struct PlaybackFrame; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Camera; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class LevelData; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Planet; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Shader; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class TileMesh; }   // 库侧类型：前置声明也要在 adofai 里
namespace adofai { class Timeline; }   // 库侧类型：前置声明也要在 adofai 里

// LevelScene owns the GL drawables of a loaded level — shaders, tile mesh,
// planets with trails — plus the per-frame scene state (track appear/
// disappear animation). Pure presentation layer: GameWindow feeds it the
// playback frame + camera and calls render(); the scene never touches
// windows, audio or input directly.
class LevelScene {
public:
    LevelScene();
    ~LevelScene();

    LevelScene(const LevelScene&) = delete;
    LevelScene& operator=(const LevelScene&) = delete;

    // Compile shaders (with inline fallback), create planets and allocate the
    // tile mesh. A GL context must be current. Returns false on shader failure.
    bool init(const LauncherConfig& cfg, const adofai::LevelData& level);

    // Build tile mesh + planet GPU buffers synchronously (main thread/context).
    // After this the caller must release the LevelData mesh temporaries
    // (angleData / tileBPMs / ...).
    void buildSync(const adofai::LevelData& level);

    void setViewportHeightPx(int h);

    bool meshReady() const { return m_meshReady; }
    int drawnInstances() const;   // 开发/诊断：上一次 draw 画了多少个实例

    // Apply the latest playback frame to planet positions and trail samples
    // (call only while playing).
    void applyFrame(const adofai::PlaybackFrame& frame, const adofai::Timeline& timeline);

    // GameWindow reports the measured work time (ms, excluding vsync/sleep) of
    // the previous frame. Used by the trail sample-rate governor.
    void setMeasuredFrameMs(double workMs);

    // Draw the whole scene into the current viewport using the given camera.
    // highlightTile: selected tile index to outline, or -1 for none (only
    // drawn while stopped).
    // volumeAmplitude：当前音乐 RMS（0..1），给 trackColorType=Volume 用（见 AudioEngine::amplitudeAt）。
    void render(adofai::Camera& camera, const adofai::Timeline& timeline, bool playing,
                double timeInLevel, int highlightTile, float volumeAmplitude = 0.0f);

private:
    bool compileShaders();
    void updateTileVisibility(const adofai::Timeline& timeline, double t, bool playing);

    // Shaders (heap-allocated, freed on destruction)
    adofai::Shader* m_tileShader = nullptr;
    adofai::Shader* m_planetShader = nullptr;
    adofai::Shader* m_trailShader = nullptr;
    adofai::Shader* m_highlightShader = nullptr;
    adofai::Shader* m_flashShader = nullptr;   // Flash 全屏叠层
    unsigned int m_flashVao = 0;               // 空 VAO（全屏三角形，无 VBO）

    adofai::TileMesh* m_tileMesh = nullptr;
    const adofai::LevelData* m_level = nullptr;   // 砖色（ColorTrack）查询用；生命周期由调用方保证
    bool m_trackColorOverride = false;            // --fill/--stroke：整条轨道用同一套颜色
    std::unique_ptr<adofai::Planet> m_redPlanet;
    std::unique_ptr<adofai::Planet> m_bluePlanet;

    // Build-time config snapshots (config object outlives the scene, but the

    std::string m_fillColor = "FFFFFF";
    std::string m_strokeColor = "000000";
    float m_fillRGB[3] = {1, 1, 1}, m_strokeRGB[3] = {0, 0, 0};
    bool m_showTrail = true;
    float m_trailDuration = 0.4f;
    float m_trailSampleRate = 200.0f;
    // Trail sample-rate governor (see setMeasuredFrameMs)
    bool m_trailAdaptive = true;
    float m_trailTargetFps = 120.0f;
    float m_trailRateMin = 60.0f;
    float m_trailRateMax = 4000.0f;
    bool  m_trailPerTile = false;          // scale rate with the local tile speed
    float m_trailSamplesPerTile = 4.0f;
    bool  m_trailLengthInTiles = false;    // optional: length in tiles instead of seconds
    float m_trailTiles = 8.0f;
    double m_trailEmaMs = 0.0;

    // Track appear/disappear animation state
    // 砖块几何走 CPU 展开表（GL 3.3 回退）而不是 GLSL 4.10 的 fma 展开
    bool m_cpuGeometry = false;
    bool m_tileVisEnabled = false;
    int m_lastHiddenEnd = -1;
    bool m_sgVisibleLatch = false;

    bool m_meshReady = false;
    int m_viewportHpx = 720;
};
