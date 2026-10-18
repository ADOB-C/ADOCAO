#include "LevelScene.hpp"
#include <cstdlib>   // getenv（诊断转储）

#include "core/util/AssetPaths.hpp"

#include "LauncherWindow.hpp"
#include "ProcMem.hpp"
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
#include "core/util/Scheduler.hpp"
#include "core/util/Profile.hpp"
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
#include <limits.h>
#include <stdlib.h>
#endif

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

// 资产查找（shader 的 "assets/shaders/..."）走库里的 adofai::resolveAsset：
// 搜索根由 app/AssetSetup.cpp 在启动时配置（CWD → exe 目录 → macOS bundle 的
// ../Resources → 上溯 3 级；见 docs/project-structure.md §2）。

LevelScene::LevelScene() = default;

LevelScene::~LevelScene() {
    if (m_buildHandle.valid()) {       // 先取消、再等返回（不能让它碰已析构的 this ✗）
        m_buildHandle.cancel();
        m_buildHandle.wait();
        m_buildHandle.clear();
    }
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
    // 砖块 VS 有**两支**：410（用 `fma()` 逐字镜像 CPU 参考的融合结构）与 330 回退
    // （查 CPU 展开表，见 assets/shaders/tile_cpu.vert）。选哪支靠**试编**，不按版本号猜 ——
    // 同 `archive/LzmaMtCompat` 的教训：版本区间 ≠ 实际能力（有的驱动给 4.1 上下文但 GLSL
    // 编译器的 fma 行为不同，反过来也有）。高亮与砖共用同一支 VS，只有 FS 不同。
    auto compileTile = [&](const char* vsFile, const char* vsInline) -> bool {
        // ⚠️ 路径必须存成 std::string：`resolveAsset(...).c_str()` 绑到 `const char*` 上时，
        // 那个临时 string 在本行结束时就被销毁 → 指针悬空 → 文件永远"加载失败"、静默退回内嵌串。
        // （2026-10-17 真踩过：一路走内嵌串渲染，改 assets/shaders/*.vert 全无效果，日志里只有
        //  一条 "Shader file loading failed, using inline fallback"。）
        const std::string vp = resolveAsset(vsFile);
        const std::string fpTile = resolveAsset("assets/shaders/tile.frag");
        const std::string fpHighlight = resolveAsset("assets/shaders/highlight.frag");
        return compileShader(*m_tileShader, vp.c_str(), fpTile.c_str(), vsInline, Shaders::kTileFragSrc)
            && compileShader(*m_highlightShader, vp.c_str(), fpHighlight.c_str(), vsInline,
                             Shaders::kHighlightFragSrc);
    };
    const bool forceCpu = [] {
        const char* e = std::getenv("ADOCAO_CPU_GEOMETRY");   // 验收/救急开关
        return e && std::atoi(e) != 0;
    }();
    if (forceCpu) {
        LOG_I("砖块几何：**强制 CPU 回退**（ADOCAO_CPU_GEOMETRY）");
        if (!compileTile("assets/shaders/tile_cpu.vert", Shaders::kTileCpuVertSrc)) {
            LOG_E("Shader compilation failed (CPU 回退)");
            return false;
        }
        m_cpuGeometry = true;
    } else if (compileTile("assets/shaders/tile.vert", Shaders::kTileVertSrc)) {
        m_cpuGeometry = false;
    } else {
        LOG_W("410 砖块管线编不过（GL 上下文没有 4.x？）→ 切 CPU 几何回退（GL 3.3 + 查表）");
        if (!compileTile("assets/shaders/tile_cpu.vert", Shaders::kTileCpuVertSrc)) {
            LOG_E("Shader compilation failed (两支砖块 VS 都编不过)");
            return false;
        }
        m_cpuGeometry = true;
    }
    LOG_I("砖块几何：%s", m_cpuGeometry ? "CPU 展开表（GL 3.3 回退）" : "GPU 按形状表展开（GLSL 4.10, fma）");

    if (!compileShader(*m_planetShader, resolveAsset("assets/shaders/planet.vert").c_str(), resolveAsset("assets/shaders/planet.frag").c_str(), Shaders::kPlanetVertSrc, Shaders::kPlanetFragSrc)
     || !compileShader(*m_trailShader, resolveAsset("assets/shaders/trail.vert").c_str(), resolveAsset("assets/shaders/trail.frag").c_str(), Shaders::kTrailVertSrc, Shaders::kTrailFragSrc)) {
        LOG_E("Shader compilation failed");
        return false;
    }
    return true;
}

bool LevelScene::init(const LauncherConfig& cfg, const LevelData& level) {
    m_fillColor = cfg.trackFillColor;
    m_strokeColor = cfg.trackStrokeColor;
    TileMesh::hexToColor3(m_fillColor, m_fillRGB);
    TileMesh::hexToColor3(m_strokeColor, m_strokeRGB);
    m_level = &level;
    m_trackColorOverride = cfg.trackColorOverride;
    m_showTrail = cfg.showTrail;
    m_trailDuration = cfg.trailDuration;
    m_trailSampleRate = cfg.trailSampleRate;
    m_trailAdaptive = cfg.trailAdaptive;
    m_trailTargetFps = cfg.trailTargetFps;
    m_trailRateMin = cfg.trailRateMin;
    m_trailRateMax = cfg.trailRateMax;
    m_trailPerTile = cfg.trailPerTile;
    m_trailSamplesPerTile = cfg.trailSamplesPerTile;
    m_trailLengthInTiles = cfg.trailLengthInTiles;
    m_trailTiles = cfg.trailTiles;
    if (m_trailRateMax < m_trailRateMin) m_trailRateMax = m_trailRateMin;

    m_tileVisEnabled = (level.settings.trackDisappearAnimation != "None" ||
                        level.settings.trackAnimation != "None" ||
                        level.hasAtStates);
    LOG_D("TrackVis: enabled=%d da=%s aa=%s atStates=%d(size=%zu)", m_tileVisEnabled,
          level.settings.trackDisappearAnimation.c_str(),
          level.settings.trackAnimation.c_str(),
          (int)level.hasAtStates, level.atStates.size());

    // Shaders (heap-allocated, freed on destruction)
    m_tileShader = new Shader();
    m_planetShader = new Shader();
    m_trailShader = new Shader();
    m_highlightShader = new Shader();
    if (!compileShaders()) return false;

    // Track
    m_tileMesh = new TileMesh();
    m_tileMesh->setCpuGeometry(m_cpuGeometry);   // 在 build() 之前：决定建哪张纹理
    // 抓帧 = 确定性剔除（LOD 步长只由视口 + 两段式决定，不用上一帧的反馈种子）
    m_tileMesh->setDeterministicCull(!cfg.capturePath.empty());
    m_tileMesh->setCullBlockFloors(cfg.cullBlockFloors);   // ④ 剔除粗块粒度（0 = 全扫）

    // Render-layer planets. The core timeline/clock only produce pure frame
    // data; the scene owns the actual GL drawable Planet objects.
    m_redPlanet = std::make_unique<Planet>(glm::vec3(1.0f, 0.0f, 0.0f), cfg.showTrail);
    m_bluePlanet = std::make_unique<Planet>(glm::vec3(0.0f, 0.0f, 1.0f), cfg.showTrail);
    return true;
}

int LevelScene::drawnInstances() const { return m_tileMesh ? m_tileMesh->lastDrawnInstances() : 0; }

// D1 的仪表盘：**常驻**口径（不是 peak RSS —— 大谱的峰值在加载期，见 ProcMem.hpp）
static void logResidentDelta(const char* what, size_t before) {
    const size_t after = appmem::currentResidentBytes();
    if (before == 0 || after == 0) {
        LOG_I("RSS: 建 %s 后 %.1f MB（本平台量不到前后差）", what, (double)after / 1048576.0);
        return;
    }
    LOG_I("RSS: 建 %s 前 %.1f MB → 后 %.1f MB（+%.1f MB，常驻口径）", what,
          (double)before / 1048576.0, (double)after / 1048576.0,
          (double)(after - before) / 1048576.0);
}

void LevelScene::buildSync(const LevelData& level) {
    const size_t rss0 = appmem::currentResidentBytes();
    m_tileMesh->build(level, m_fillColor, m_strokeColor);
    logResidentDelta("TileMesh", rss0);
    if (m_redPlanet) {
        m_redPlanet->buildGPU();
        m_bluePlanet->buildGPU();
    }
    m_meshReady = true;
}

void LevelScene::setViewportHeightPx(int h) { m_viewportHpx = h; }

bool LevelScene::beginAsyncBuild(const LevelData& level, GLFWwindow* sharedWindow) {
    if (!sharedWindow) return false;
    // ④ 后台车道：网格构建走 `Scheduler`（Background），不再自起 `std::async` ✗。
    // **生命周期**：任务体碰 `this`，所以析构时必须"先取消、再等它真的返回"（等价于原来
    // `std::future` 析构的 join 语义 ✓，但**可以提前退出** —— `build()` 在块边界查令牌 ✓）。
    m_buildHandle = Scheduler::instance().submit(Lane::Background, [this, &level, sharedWindow](
        Scheduler::CancelToken tok) {
        glfwMakeContextCurrent(sharedWindow);
        const size_t rss0 = appmem::currentResidentBytes();
        const std::function<bool()> shouldCancel = [tok]() { return tok.cancelled(); };
        m_tileMesh->build(level, m_fillColor, m_strokeColor, &shouldCancel);   // 块边界查令牌 → 可提前退
        if (!tok.cancelled()) {
            logResidentDelta("TileMesh(async)", rss0);
            if (m_redPlanet) {
                m_redPlanet->buildGPU();
                m_bluePlanet->buildGPU();
            }
        }
        glfwMakeContextCurrent(nullptr);
    });
    return true;
}

bool LevelScene::pollAsyncBuild() {
    if (!m_buildHandle.valid()) return false;
    if (m_buildHandle.ready()) {
        m_buildHandle.clear();
        m_meshReady = true;
        return true;
    }
    return false;
}

void LevelScene::setMeasuredFrameMs(double workMs) {
    if (workMs <= 0.0) return;
    m_trailEmaMs = (m_trailEmaMs <= 0.0) ? workMs : m_trailEmaMs * 0.85 + workMs * 0.15;
    // Per-tile mode derives its rate from the chart's speed every frame, so the
    // budget governor must not fight it.
    if (!m_trailAdaptive || m_trailPerTile) return;

    // Governor: try to keep the whole frame under 1000/trailTargetFps ms and
    // spend the leftover budget on a higher trail sample rate. Drop hard when
    // over budget, raise only with headroom, clamp to [min, max].
    const double budgetMs = 1000.0 / std::max(1.0, (double)m_trailTargetFps);
    const double ema = m_trailEmaMs;
    double target = m_trailSampleRate;
    if (ema > budgetMs) {
        target = (double)m_trailSampleRate * (budgetMs * 0.9 / ema);
        if (target < (double)m_trailSampleRate * 0.25)   // shed at most 75%/frame
            target = (double)m_trailSampleRate * 0.25;
    } else if (ema < budgetMs * 0.85) {
        target = (double)m_trailSampleRate * (budgetMs * 0.85 / ema);
        if (target > (double)m_trailSampleRate * 1.5)    // grow at most +50%/frame
            target = (double)m_trailSampleRate * 1.5;
    }
    target = std::clamp(target, (double)m_trailRateMin, (double)m_trailRateMax);
    m_trailSampleRate = (float)std::floor(target + 0.5);
    if (m_trailSampleRate < 1.0f) m_trailSampleRate = 1.0f;
}

// Trail window for this frame. Two independent options (both off by default, so
// the classic "last N seconds at a fixed Hz rate" behaviour is unchanged):
//  - lengthInTiles: the trail covers the last `trailTiles` tiles instead of the
//    last `trailDuration` seconds. A time window is unusable on charts whose BPM
//    spans orders of magnitude — 0.4s is 6,800 tiles at BPM 1.5M, so the trail
//    sweeps across the map and can lie mostly in front of the planet whenever the
//    path doubles back inside the window.
//  - speedAware: raise the rate with the track covered per second (steps + the
//    arc of the tile's relative angle) so the polyline still follows the track.
// The window/rate maths lives in core (PositionSolver::trailWindow) so it can be
// exercised headlessly.
void LevelScene::applyFrame(const PlaybackFrame& frame, const Timeline& timeline) {
    if (!m_redPlanet || !m_bluePlanet) return;

    // **不要**在这里窄化成 float：绝对世界坐标到 6.84e7 时 float ulp = 8 砖（primer-final），
    // 行星会被量化到与砖块不同的格点上（用户实测"行星也不对了"）。Planet 现在存 double，
    // 相机相对化在 Planet::draw 里用 double 做完再窄化。
    m_redPlanet->position = glm::dvec3(frame.redPosition.x, frame.redPosition.y, 9.5);
    m_bluePlanet->position = glm::dvec3(frame.bluePosition.x, frame.bluePosition.y, 9.5);

    if (!m_showTrail || !m_redPlanet->trail || !m_bluePlanet->trail) return;

    PositionSolver::TrailSamplingConfig sc;
    sc.lengthInTiles  = m_trailLengthInTiles;
    sc.duration       = m_trailDuration;
    sc.tiles          = m_trailTiles;
    sc.speedAware     = m_trailPerTile;
    sc.samplesPerTile = m_trailSamplesPerTile;
    sc.fixedRate      = m_trailSampleRate;      // possibly governor-adjusted
    sc.adaptive       = m_trailAdaptive;        // true = 受 rateMax 夹；false = 用户显式固定率
    sc.rateMin        = m_trailRateMin;
    sc.rateMax        = m_trailRateMax;

    PositionSolver::TrailWindow win;
    {
        profile::Scoped _s("trailWindow");
        win = PositionSolver::trailWindow(timeline, frame.timeInLevel, sc);
    }
    std::vector<glm::dvec2> redPts, bluePts;
    {
        profile::Scoped _s("trailSample");
        PositionSolver::sampleTrailRange(timeline, win.startTime, frame.timeInLevel, win.sampleRate,
                                         frame.redPosition, frame.bluePosition,
                                         redPts, bluePts);
    }

    // 【诊断，env 门控】拖尾点哈希：A/B "按块物化 provider" 是否改变了拖尾
    if (std::getenv("ADOCAO_TRAIL_DUMP")) {
        auto fnv = [](const void* p, size_t bytes) {
            const uint8_t* q = (const uint8_t*)p;
            uint64_t h = 1469598103934665603ull;
            for (size_t k = 0; k < bytes; ++k) { h ^= q[k]; h *= 1099511628211ull; }
            return h;
        };
        LOG_I("trail-dump: startTime=%.17g rate=%.17g t=%.17g n=%zu/%zu red=%016llx blue=%016llx",
              win.startTime, win.sampleRate, frame.timeInLevel, redPts.size(), bluePts.size(),
              (unsigned long long)fnv(redPts.data(), redPts.size() * sizeof(glm::dvec2)),
              (unsigned long long)fnv(bluePts.data(), bluePts.size() * sizeof(glm::dvec2)));
        for (size_t k = 0; k < bluePts.size() && k < 6; ++k)
            LOG_I("trail-blue[%zu]=%.17g,%.17g", k, bluePts[k].x, bluePts[k].y);
        if (!bluePts.empty()) {
            const size_t L = bluePts.size() - 1;
            LOG_I("trail-blue[last]=%.17g,%.17g", bluePts[L].x, bluePts[L].y);
        }
    }

    if (redPts.empty() || bluePts.empty()) return;

    const int maxPoints =
        (int)std::ceil((frame.timeInLevel - win.startTime) * win.sampleRate) + 1;
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
    if (!timeline.hasTrackVisTimes()) return;      // 这张谱不需要 TrackVis
    const int64_t n = timeline.tileCount();
    if (n > 0 && playing) {
        // "最后一个 disappearTime <= t 的层"：**先在粗索引上二分**（每 kAnchorStride 层一份，
        // 26 KB）→ 再物化那一块做块内细搜。等价性前提：粗索引单调（`trackVisCoarseMonotone()`，
        // 不单调时 Timeline 不窗口化、这里读到的仍是密集数组的同一批值）。
        const auto& cda = timeline.coarseDisappearTimes();
        const int64_t stride = Timeline::kAnchorStride;
        int64_t lo = 0, hi = (int64_t)cda.size() - 1, aEnd = -1;
        while (lo <= hi) {
            const int64_t m = (lo + hi) / 2;
            if (cda[(size_t)m] <= t) { aEnd = m; lo = m + 1; } else hi = m - 1;
        }
        int64_t rangeEnd = -1;
        if (aEnd >= 0) {
            const int64_t from = aEnd * stride;
            const int64_t to = std::min(from + stride, n);
            for (int64_t i = from; i < to; ++i)
                if (timeline.disappearTimeAt(i) <= t) rangeEnd = i;   // 取块内最后一个满足的
        }
        if (rangeEnd != m_lastHiddenEnd) {
            const int64_t start = std::min<int64_t>(m_lastHiddenEnd, rangeEnd) + 1;
            const int64_t end   = std::max<int64_t>(m_lastHiddenEnd, rangeEnd);
            for (int64_t i = start; i <= end; i++) {
                const bool hide = (timeline.disappearTimeAt(i) <= t) || (timeline.appearTimeAt(i) > t);
                m_tileMesh->updateVisibleRange((int)i, (int)i, !hide);
            }
            m_tileMesh->setVisibleThreshold((int)rangeEnd);
            m_lastHiddenEnd = (int)rangeEnd;
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

namespace {
struct TrackColorCtx {
    const LevelData* level;
    const Timeline* timeline;   // TweenColor 的 startTime/beat 来源
    double time;
    float amplitude;   // Volume 类型用（音乐 RMS）
};
void trackColorAt(void* ctx, uint32_t tile, uint8_t fill[3], uint8_t stroke[3]) {
    const auto* c = static_cast<const TrackColorCtx*>(ctx);
    const TileColors col = c->level->tileColorsAt((int64_t)tile, c->time, c->amplitude, c->timeline);
    if (std::getenv("ADOCAO_COLOR_DEBUG") && tile < 4)
        LOG_I("color-debug: tile=%u t=%.3f fill=%02x%02x%02x stroke=%02x%02x%02x", tile, c->time,
              col.fill[0], col.fill[1], col.fill[2], col.stroke[0], col.stroke[1], col.stroke[2]);
    fill[0] = col.fill[0]; fill[1] = col.fill[1]; fill[2] = col.fill[2];
    stroke[0] = col.stroke[0]; stroke[1] = col.stroke[1]; stroke[2] = col.stroke[2];
}
}  // namespace

void LevelScene::render(Camera& camera, const Timeline& timeline, bool playing,
                        double timeInLevel, int highlightTile, float volumeAmplitude) {
    // Track appear/disappear animation
    { profile::Scoped _s("vis"); updateTileVisibility(timeline, timeInLevel, playing); }

    m_tileMesh->setViewportHeightPx(m_viewportHpx);
    double vl, vr, vb, vt;      // 世界坐标 → double（见 docs/rendering.md 坐标约定）
    camera.frustumBounds(vl, vr, vb, vt);

    // 轨道（砖 + 图标，一次 draw）。默认逐实例颜色（ColorTrack / trackStyle）；
    // `--fill/--stroke` 显式覆盖时退回 uniform，由 uUseInstColor 切换。
    m_tileShader->use();
    m_tileShader->setMat4("uVP", glm::value_ptr(camera.viewProj()));
    m_tileShader->setVec3("uFillColor", m_fillRGB[0], m_fillRGB[1], m_fillRGB[2]);
    m_tileShader->setVec3("uStrokeColor", m_strokeRGB[0], m_strokeRGB[1], m_strokeRGB[2]);
    m_tileShader->setFloat("uOpacity", 1.0f);
    if (m_trackColorOverride || !m_level) {
        m_tileShader->setFloat("uUseInstColor", 0.0f);
        m_tileMesh->draw(vl, vr, vb, vt, camera.targetX(), camera.targetY());
    } else {
        // 逐砖颜色（ColorTrack）：只对可见列表里的砖算（每帧几百次），见 TileMesh::InstanceColorFn。
        TrackColorCtx ctx{m_level, &timeline, timeInLevel, volumeAmplitude};
        m_tileShader->setFloat("uUseInstColor", 1.0f);
        m_tileMesh->draw(vl, vr, vb, vt, camera.targetX(), camera.targetY(), &trackColorAt, &ctx);
    }

    // Trails（RAII 段：别显式调析构，那会跟作用域结束时的析构重复累加）
    {
        profile::Scoped _s("trails");
        if (m_showTrail && playing && m_redPlanet && m_redPlanet->trail) {
            m_redPlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
            m_bluePlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
        }
    }

    // Planets
    {
        profile::Scoped _s("planets");
        if (playing && m_redPlanet && m_redPlanet->gpuBuilt()) {
            m_redPlanet->draw(*m_planetShader, camera, camera.targetX(), camera.targetY());
            m_bluePlanet->draw(*m_planetShader, camera, camera.targetX(), camera.targetY());
        }
    }

    // Highlight
    if (!playing && highlightTile >= 0) {
        glDisable(GL_DEPTH_TEST);
        m_highlightShader->use();
        m_highlightShader->setMat4("uVP", glm::value_ptr(camera.viewProj()));
        m_highlightShader->setVec3("uFillColor", m_fillRGB[0], m_fillRGB[1], m_fillRGB[2]);
        m_highlightShader->setVec3("uStrokeColor", m_strokeRGB[0], m_strokeRGB[1], m_strokeRGB[2]);
        m_highlightShader->setFloat("uUseInstColor", 0.0f);   // 高亮沿用 uniform 颜色
        m_tileMesh->drawHighlightedTile(highlightTile, camera.targetX(), camera.targetY());
        glEnable(GL_DEPTH_TEST);
    }
}
