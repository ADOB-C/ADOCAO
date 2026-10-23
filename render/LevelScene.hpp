#pragma once

#include "render/SceneConfig.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/util/Scheduler.hpp"   // Handle 是**按值**成员 → 需要完整类型

#include <future>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace adofai {
class Scheduler; struct PlaybackFrame;
class Camera;
struct LevelData;
class Planet;
class Shader;
class TileMesh;
class Timeline;

// LevelScene owns the GL drawables of a loaded level — shaders, tile mesh,
// planets with trails — plus the per-frame scene state (track appear/
// disappear animation). Pure presentation layer: GameWindow feeds it the
// playback frame + camera and calls render(); the scene never touches
// windows, audio or input directly.
class LevelScene {
public:
    // 逐砖关键帧时间轴（MoveTrack+AnimateTrack）；没建帧时为 nullptr。
    const adofai::TileAnimation* animation() const { return m_tileAnim.get(); }

    LevelScene();
    ~LevelScene();

    LevelScene(const LevelScene&) = delete;
    LevelScene& operator=(const LevelScene&) = delete;

    // Compile shaders (with inline fallback), create planets and allocate the
    // tile mesh. A GL context must be current. Returns false on shader failure.
    bool init(const SceneConfig& cfg, const adofai::LevelData& level,
              const adofai::Timeline* timeline = nullptr);

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
    void buildTileAnimation(const adofai::Timeline& timeline);
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
    // 形状表要用它把 trackStyle 解析到"砖被踩到的那一刻"（见 TileMesh::build）；
    // 由 init() 传入、buildSync() 消费 —— 生命周期同样由调用方（GameWindow）保证。
    const adofai::Timeline* m_buildTimeline = nullptr;
    bool m_trackColorOverride = false;            // --fill/--stroke：整条轨道用同一套颜色
    std::unique_ptr<adofai::Planet> m_redPlanet;
    std::unique_ptr<adofai::Planet> m_bluePlanet;

    // Build-time config snapshots (config object outlives the scene, but the

    std::string m_fillColor = "FFFFFF";
    std::string m_strokeColor = "000000";
    float m_fillRGB[3] = {1, 1, 1}, m_strokeRGB[3] = {0, 0, 0};
    bool m_showTrail = true;
    float m_trailDuration = 0.4f;
    float m_trailSagitta  = 0.0f;   // >0 = 按"每样本偏离 ≤ N px"反推采样率（0 = 关闭）
    float m_lastZoom      = 100.0f; // 上一帧渲染时的相机放大倍数（applyFrame 算 pixelsPerTile 用）
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
    std::function<size_t()> m_residentBytes;   // 由 SceneConfig 注入；空 = 不打 RSS 日志
    // 逐砖关键帧时间轴（MoveTrack + AnimateTrack）；谱面没有动画时为空。
    std::unique_ptr<adofai::TileAnimation> m_tileAnim;
    bool m_tileAnimBuilt = false;
    bool m_animVisDirty = true;                    // 时间变了才重算"动画砖是否全透明"
    mutable adofai::TileAnimSample m_scratchAnim;
    std::vector<uint8_t> m_animVisState;          // 每块动画砖上一帧的"隐藏"状态（2 = 未知）
    bool m_visBuilt = false;
    bool m_timeChangedSinceVis = true;
};

}  // namespace adofai
