#include "LevelScene.hpp"

#include "LauncherWindow.hpp"
#include "glad/gl_core.hpp"
#include "render/Camera.hpp"
#include "render/Shader.hpp"
#include "render/Shaders.hpp"
#include "render/TileMesh.hpp"
#include "render/Planet.hpp"
#include "render/PlanetTrail.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/util/Logger.hpp"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif

namespace {

// Shader assets live in "assets/shaders/" relative to the working directory,
// the executable directory or up to 3 parent directories above it (Finder
// launches / .app bundles / nested build dirs).
static std::string executableDirectory() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::string dir(buf.data());
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir;
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    std::string dir(buf);
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir;
#else
    return {};
#endif
}

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static std::string assetPath(const std::string& relative) {
    std::vector<std::string> candidates;
    candidates.push_back(relative);

    const std::string exeDir = executableDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + "/" + relative);
        auto dir = exeDir;
        for (int i = 0; i < 3 && !dir.empty(); i++) {
            const auto slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) { dir.clear(); break; }
            dir = dir.substr(0, slash);
        }
        if (!dir.empty())
            candidates.push_back(dir + "/" + relative);
    }

    for (const auto& c : candidates) {
        if (fileExists(c)) return c;
    }
    return relative;
}

} // namespace

LevelScene::LevelScene() = default;

LevelScene::~LevelScene() {
    // Requires a current GL context (destructed before the window is destroyed).
    delete m_tileMesh;
    delete m_tileShader;
    delete m_planetShader;
    delete m_trailShader;
    delete m_highlightShader;
}

bool LevelScene::compileShaders() {
    auto compileShader = [](Shader& s, const char* vp, const char* fp,
                            const char* vs, const char* fs) -> bool {
        if (s.compileFile(vp, fp)) return true;
        LOG_W("Shader file loading failed, using inline fallback");
        return s.compile(vs, fs);
    };

    if (!compileShader(*m_tileShader, assetPath("assets/shaders/tile.vert").c_str(), assetPath("assets/shaders/tile.frag").c_str(), Shaders::kTileVertSrc, Shaders::kTileFragSrc)
     || !compileShader(*m_planetShader, assetPath("assets/shaders/planet.vert").c_str(), assetPath("assets/shaders/planet.frag").c_str(), Shaders::kPlanetVertSrc, Shaders::kPlanetFragSrc)
     || !compileShader(*m_trailShader, assetPath("assets/shaders/trail.vert").c_str(), assetPath("assets/shaders/trail.frag").c_str(), Shaders::kTrailVertSrc, Shaders::kTrailFragSrc)
     || !compileShader(*m_highlightShader, assetPath("assets/shaders/highlight.vert").c_str(), assetPath("assets/shaders/highlight.frag").c_str(), Shaders::kHighlightVertSrc, Shaders::kHighlightFragSrc)) {
        LOG_E("Shader compilation failed");
        return false;
    }
    return true;
}

bool LevelScene::init(const LauncherConfig& cfg, const LevelData& level) {
    m_fillColor = cfg.trackFillColor;
    m_strokeColor = cfg.trackStrokeColor;
    m_legacyCulling = cfg.legacyCulling;
    m_showTrail = cfg.showTrail;
    m_trailDuration = cfg.trailDuration;
    m_trailSampleRate = cfg.trailSampleRate;

    m_tileVisEnabled = (level.settings.trackDisappearAnimation != "None" ||
                        level.settings.trackAnimation != "None" ||
                        !level.atStates.empty());
    LOG_D("TrackVis: enabled=%d da=%s aa=%s atStates=%zu", m_tileVisEnabled,
          level.settings.trackDisappearAnimation.c_str(),
          level.settings.trackAnimation.c_str(),
          level.atStates.size());

    // Shaders (heap-allocated, freed on destruction)
    m_tileShader = new Shader();
    m_planetShader = new Shader();
    m_trailShader = new Shader();
    m_highlightShader = new Shader();
    if (!compileShaders()) return false;

    // Track
    m_tileMesh = new TileMesh();

    // Render-layer planets. The core timeline/clock only produce pure frame
    // data; the scene owns the actual GL drawable Planet objects.
    m_redPlanet = std::make_unique<Planet>(glm::vec3(1.0f, 0.0f, 0.0f), cfg.showTrail);
    m_bluePlanet = std::make_unique<Planet>(glm::vec3(0.0f, 0.0f, 1.0f), cfg.showTrail);
    return true;
}

void LevelScene::buildSync(const LevelData& level) {
    m_tileMesh->build(level, m_fillColor, m_strokeColor, m_legacyCulling);
    if (m_redPlanet) {
        m_redPlanet->buildGPU();
        m_bluePlanet->buildGPU();
    }
    m_meshReady = true;
}

bool LevelScene::beginAsyncBuild(const LevelData& level, GLFWwindow* sharedWindow) {
    if (!sharedWindow) return false;
    m_buildFuture = std::async(std::launch::async, [this, &level, sharedWindow]() {
        glfwMakeContextCurrent(sharedWindow);
        m_tileMesh->build(level, m_fillColor, m_strokeColor, m_legacyCulling);
        if (m_redPlanet) {
            m_redPlanet->buildGPU();
            m_bluePlanet->buildGPU();
        }
        glfwMakeContextCurrent(nullptr);
    });
    return true;
}

bool LevelScene::pollAsyncBuild() {
    if (!m_buildFuture.valid()) return false;
    if (m_buildFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        m_buildFuture.get();
        m_meshReady = true;
        return true;
    }
    return false;
}

void LevelScene::applyFrame(const PlaybackFrame& frame, const Timeline& timeline) {
    if (!m_redPlanet || !m_bluePlanet) return;

    m_redPlanet->position = glm::vec3((float)frame.redPosition.x, (float)frame.redPosition.y, 9.5f);
    m_bluePlanet->position = glm::vec3((float)frame.bluePosition.x, (float)frame.bluePosition.y, 9.5f);

    if (!m_showTrail || !m_redPlanet->trail || !m_bluePlanet->trail) return;

    std::vector<glm::dvec2> redPts, bluePts;
    PositionSolver::sampleTrail(timeline, frame.timeInLevel,
                                m_trailDuration, m_trailSampleRate,
                                frame.redPosition, frame.bluePosition,
                                redPts, bluePts);
    if (redPts.empty() || bluePts.empty()) return;

    const int maxPoints = (int)std::ceil(m_trailDuration * m_trailSampleRate) + 1;
    std::vector<double> redXY(redPts.size() * 2), blueXY(bluePts.size() * 2);
    for (size_t i = 0; i < redPts.size(); i++) {
        redXY[i*2] = redPts[i].x;
        redXY[i*2+1] = redPts[i].y;
        blueXY[i*2] = bluePts[i].x;
        blueXY[i*2+1] = bluePts[i].y;
    }
    m_redPlanet->setTrailPoints(redXY.data(), (int)redPts.size(), maxPoints);
    m_bluePlanet->setTrailPoints(blueXY.data(), (int)bluePts.size(), maxPoints);
}

void LevelScene::updateTileVisibility(const Timeline& timeline, double t, bool playing) {
    if (!m_tileVisEnabled) return;
    const auto& dt = timeline.tileDisappearTimes();
    const auto& at = timeline.tileAppearTimes();
    int n = (int)dt.size();
    if (n > 0 && playing) {
        // Find approximate range: last tile with finite disappearTime <= t
        int lo = 0, hi = n - 1, rangeEnd = -1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (dt[m] <= t) { rangeEnd = m; lo = m + 1; }
            else hi = m - 1;
        }
        if (rangeEnd != m_lastHiddenEnd) {
            int start = std::min(m_lastHiddenEnd, rangeEnd) + 1;
            int end   = std::max(m_lastHiddenEnd, rangeEnd);
            for (int i = start; i <= end; i++) {
                bool hide = (dt[i] <= t) || (at[i] > t);
                m_tileMesh->updateVisibleRange(i, i, !hide);
            }
            m_tileMesh->setVisibleThreshold(rangeEnd);
            m_lastHiddenEnd = rangeEnd;
        }
        if (rangeEnd >= 0) m_sgVisibleLatch = true;
    }
    // On pause/stop: restore all hidden tiles to visible
    if (!playing && m_sgVisibleLatch) {
        m_tileMesh->updateVisibleRange(0, std::max(0, m_lastHiddenEnd), true);
        m_tileMesh->setVisibleThreshold(-1);
        m_lastHiddenEnd = -1;
        m_sgVisibleLatch = false;
    }
}

void LevelScene::render(Camera& camera, const Timeline& timeline, bool playing,
                        double timeInLevel, int highlightTile) {
    // Track appear/disappear animation
    updateTileVisibility(timeline, timeInLevel, playing);

    float vl, vr, vb, vt;
    camera.frustumBounds(vl, vr, vb, vt);

    // Tiles
    m_tileShader->use();
    m_tileShader->setMat4("uVP", glm::value_ptr(camera.viewProj()));
    m_tileMesh->draw(vl, vr, vb, vt, camera.targetX(), camera.targetY());

    // Trails
    if (m_showTrail && playing && m_redPlanet && m_redPlanet->trail) {
        m_redPlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
        m_bluePlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
    }

    // Planets
    if (playing && m_redPlanet && m_redPlanet->gpuBuilt()) {
        m_redPlanet->draw(*m_planetShader, camera, camera.targetX(), camera.targetY());
        m_bluePlanet->draw(*m_planetShader, camera, camera.targetX(), camera.targetY());
    }

    // Icons
    m_tileShader->use();
    m_tileMesh->drawIcons(vl, vr, vb, vt, camera.targetX(), camera.targetY());

    // Highlight
    if (!playing && highlightTile >= 0) {
        glDisable(GL_DEPTH_TEST);
        m_highlightShader->use();
        m_highlightShader->setMat4("uVP", glm::value_ptr(camera.viewProj()));
        m_tileMesh->drawHighlightedTile(highlightTile, camera.targetX(), camera.targetY());
        glEnable(GL_DEPTH_TEST);
    }
}
