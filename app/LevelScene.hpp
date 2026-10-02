#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;
struct LauncherConfig;
struct PlaybackFrame;
class Camera;
class LevelData;
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
    LevelScene();
    ~LevelScene();

    LevelScene(const LevelScene&) = delete;
    LevelScene& operator=(const LevelScene&) = delete;

    // Compile shaders (with inline fallback), create planets and allocate the
    // tile mesh. A GL context must be current. Returns false on shader failure.
    bool init(const LauncherConfig& cfg, const LevelData& level);

    // Build tile mesh + planet GPU buffers synchronously (mesh not built yet).
    // After this (and after any async build) the caller must release the
    // LevelData mesh temporaries (angleData / tileBPMs / ...).
    void buildSync(const LevelData& level);

    // Start the mesh + planet GPU build on a background thread sharing the GL
    // context via sharedWindow (context must be current on this thread).
    // Returns false when no background build could be started — fall back to
    // buildSync() in that case. Poll with pollAsyncBuild().
    bool beginAsyncBuild(const LevelData& level, GLFWwindow* sharedWindow);

    // Returns true once when the background build just finished; false
    // afterwards (and when no async build is running).
    bool pollAsyncBuild();

    bool meshReady() const { return m_meshReady; }

    // Apply the latest playback frame to planet positions and trail samples
    // (call only while playing).
    void applyFrame(const PlaybackFrame& frame, const Timeline& timeline);

    // GameWindow reports the measured work time (ms, excluding vsync/sleep) of
    // the previous frame. Used by the trail sample-rate governor.
    void setMeasuredFrameMs(double workMs);

    // Draw the whole scene into the current viewport using the given camera.
    // highlightTile: selected tile index to outline, or -1 for none (only
    // drawn while stopped).
    void render(Camera& camera, const Timeline& timeline, bool playing,
                double timeInLevel, int highlightTile);

private:
    bool compileShaders();
    void updateTileVisibility(const Timeline& timeline, double t, bool playing);

    // Shaders (heap-allocated, freed on destruction)
    Shader* m_tileShader = nullptr;
    Shader* m_planetShader = nullptr;
    Shader* m_trailShader = nullptr;
    Shader* m_highlightShader = nullptr;

    TileMesh* m_tileMesh = nullptr;
    std::unique_ptr<Planet> m_redPlanet;
    std::unique_ptr<Planet> m_bluePlanet;

    // Build-time config snapshots (config object outlives the scene, but the
    // async build thread must not touch it).
    std::string m_fillColor = "FFFFFF";
    std::string m_strokeColor = "000000";
    bool m_legacyCulling = false;
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
    bool m_tileVisEnabled = false;
    int m_lastHiddenEnd = -1;
    bool m_sgVisibleLatch = false;

    std::future<void> m_buildFuture;
    bool m_meshReady = false;
};
