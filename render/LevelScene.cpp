#include "render/LevelScene.hpp"

#include <cstdio>
#include <cstdlib>   // getenv（诊断转储）

#include "core/util/AssetPaths.hpp"

#include "glad/gl_core.hpp"
#include "render/Camera.hpp"
#include "render/Shader.hpp"
#include "render/Shaders.hpp"
#include "render/TileMesh.hpp"
#include "render/Planet.hpp"
#include "render/PlanetTrail.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PlaybackClock.hpp"   // PlaybackFrame
#include "core/timeline/PositionSolver.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"
#include "core/util/Scheduler.hpp"
#include "core/util/Profile.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
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

namespace adofai {

// 资产查找（shader 的 "assets/shaders/..."）走库里的 adofai::resolveAsset：
// 搜索根由 app/AssetSetup.cpp 在启动时配置（CWD → exe 目录 → macOS bundle 的
// ../Resources → 上溯 3 级；见 docs/project-structure.md §2）。

LevelScene::LevelScene() = default;

LevelScene::~LevelScene() {
    // Requires a current GL context (destructed before the window is destroyed).
    delete m_tileMesh;
    delete m_tileShader;
    delete m_flashShader;
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
    // 验收/救急开关：强制 CPU 几何回退
    const bool forceCpu = env::flag("ADOCAO_CPU_GEOMETRY");
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
    // Flash 叠层
    m_flashShader = new Shader();
    {
        const std::string vp = resolveAsset("assets/shaders/flash.vert");
        const std::string fp = resolveAsset("assets/shaders/flash.frag");
        if (!m_flashShader->compileFile(vp.c_str(), fp.c_str())) {
            LOG_W("Shader file loading failed, using inline fallback (flash)");
            if (!m_flashShader->compile(Shaders::kFlashVertSrc, Shaders::kFlashFragSrc)) {
                LOG_E("Shader compilation failed (flash)");
                return false;
            }
        }
    }
    return true;
}

bool LevelScene::init(const SceneConfig& cfg, const LevelData& level, const Timeline* timeline) {
    m_residentBytes = cfg.residentBytes;
    m_buildTimeline = timeline;
    m_fillColor = cfg.trackFillColor;
    // autoStroke：描边 = 填充 ×0.5（与向导的 deriveStroke 同一规则）。CLI 的 --no-auto-stroke
    // 才用显式 trackStrokeColor；否则 --fill 会带着推导出的描边，不再是一个无效开关。
    if (cfg.autoStroke) {
        unsigned r = 0, g = 0, b = 0;
        if (std::sscanf(m_fillColor.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%02x%02x%02x", r / 2, g / 2, b / 2);
            m_strokeColor = buf;
        } else {
            m_strokeColor = cfg.trackStrokeColor;
        }
    } else {
        m_strokeColor = cfg.trackStrokeColor;
    }
    TileMesh::hexToColor3(m_fillColor, m_fillRGB);
    TileMesh::hexToColor3(m_strokeColor, m_strokeRGB);
    m_level = &level;
    m_trackColorOverride = cfg.trackColorOverride;
    m_showTrail = cfg.showTrail;
    m_trailDuration = cfg.trailDuration;
    m_trailSagitta  = cfg.trailSagitta;
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

    // **不再用"时间窗"隐藏砖**：Re_ADOJAS 的模型里砖一直在场景里，靠关键帧把 opacity/scale
    // 收到 0（Scatter/Shrink/Fade…）。时间窗会在动画**开始**就把砖删掉，整段消失动画看不见
    // （2026-10-21 Plum - R 结尾 Scatter_Far 就是这么丢的）。
    m_tileVisEnabled = false;
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
    {   // Flash：全屏三角形（无 VBO），一个空 VAO 就够
        glGenVertexArrays(1, &m_flashVao);
    }

    // Track
    m_tileMesh = new TileMesh();
    m_tileMesh->setCpuGeometry(m_cpuGeometry);   // 在 build() 之前：决定建哪张纹理
    // 抓帧 = 确定性剔除（LOD 步长只由视口 + 两段式决定，不用上一帧的反馈种子）
    m_tileMesh->setDeterministicCull(cfg.deterministicCull);
    m_tileMesh->setCullBlockFloors(cfg.cullBlockFloors);
    m_tileMesh->setMovePad(level.movePad());
    m_tileMesh->setAnimNeeded(level.animatesTiles());   // ④ 剔除粗块粒度（0 = 全扫）

    // Render-layer planets. The core timeline/clock only produce pure frame
    // data; the scene owns the actual GL drawable Planet objects.
    m_redPlanet = std::make_unique<Planet>(glm::vec3(1.0f, 0.0f, 0.0f), cfg.showTrail);
    m_bluePlanet = std::make_unique<Planet>(glm::vec3(0.0f, 0.0f, 1.0f), cfg.showTrail);
    return true;
}

int LevelScene::drawnInstances() const { return m_tileMesh ? m_tileMesh->lastDrawnInstances() : 0; }

// D1 的仪表盘：**常驻**口径（不是 peak RSS —— 大谱的峰值在加载期）。
// 数值由宿主注入（SceneConfig::residentBytes），render/ 不依赖 app/。
static void logResidentDelta(const std::function<size_t()>& provider, const char* what, size_t before) {
    if (!provider) return;
    const size_t after = provider();
    if (before == 0 || after == 0) {
        LOG_I("RSS: 建 %s 后 %.1f MB（本平台量不到前后差）", what, (double)after / 1048576.0);
        return;
    }
    LOG_I("RSS: 建 %s 前 %.1f MB → 后 %.1f MB（+%.1f MB，常驻口径）", what,
          (double)before / 1048576.0, (double)after / 1048576.0,
          (double)(after - before) / 1048576.0);
}

void LevelScene::buildSync(const LevelData& level) {
    const size_t rss0 = m_residentBytes ? m_residentBytes() : 0;
    m_tileMesh->build(level, m_fillColor, m_strokeColor, m_buildTimeline);
    logResidentDelta(m_residentBytes, "TileMesh", rss0);
    if (m_redPlanet) {
        m_redPlanet->buildGPU();
        m_bluePlanet->buildGPU();
    }
    m_meshReady = true;
}

void LevelScene::setViewportHeightPx(int h) { m_viewportHpx = h; }

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
// 一砖占多少像素 = 视口高 / 视野高（= 12 砖 × 100/zoom）
static float camera_pixelsPerTile(int fbHeight, float zoom) {
    return fbHeight > 0 ? (float)fbHeight * zoom / 1200.0f : 0.0f;
}

void LevelScene::buildTileAnimation(const Timeline& timeline) {
    if (m_tileAnimBuilt) return;
    m_tileAnimBuilt = true;
    if (!timeline.hasTrackVisTimes() && m_level->moveEvents().empty()) return;   // 没动画：不建
    m_tileAnim = std::make_unique<adofai::TileAnimation>();
    m_tileAnim->build(*m_level, timeline);
}

void LevelScene::applyFrame(const PlaybackFrame& frame, const Timeline& timeline) {
    buildTileAnimation(timeline);
    m_animVisDirty = true;   // 时间推进 → 重新判定"哪些动画砖已经全透明/缩到 0" 
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
    sc.sagittaPx      = m_trailSagitta;
    sc.pixelsPerTile  = m_viewportHpx > 0 ? camera_pixelsPerTile(m_viewportHpx, m_lastZoom) : 0.0f;

    // 拖尾按时间正常采样即可：关卡结束后是"移动星继续绕枢轴星转"（Infinite Rotation，
    // 见 PlaybackClock），positionAtTile 在末尾之外走的是**轨道角**而不是外推直线，
    // 所以不会再画出飞出屏幕的带子（2026-10-18 用户看到的彩色方块/环）。
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
    if (env::flag("ADOCAO_TRAIL_DUMP")) {
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
    if (!m_tileVisEnabled) {
        // 关键帧模型：砖一直在场景里；只把"完全透明且缩到 0"的动画砖标成不可见（纯性能）。
        // 成本 O(动画砖数)（Plum 730 / 2025 数千），并且只在时间变化时做。
        if (!m_tileAnim) return;
        if (!m_animVisDirty) return;
        m_animVisDirty = false;
        const double tt = (t < 0.0) ? 0.0 : t;
        const auto& entries = m_tileAnim->entries();
        if (m_animVisState.size() != entries.size()) m_animVisState.assign(entries.size(), 2);
        for (size_t i = 0; i < entries.size(); ++i) {
            const uint8_t hidden = m_tileAnim->hiddenAt(entries[i].tile, tt) ? 1 : 0;
            if (m_animVisState[i] == hidden) continue;   // 只在状态翻转时才动实例缓冲
            m_animVisState[i] = hidden;
            m_tileMesh->updateVisibleRange((int)entries[i].tile, (int)entries[i].tile, hidden == 0);
        }
        return;
    }
    // 倒计时（timeInLevel < 0）期间按 t=0 处理：原版在 countdown 里**看得到开头那段轨道**，
    // 而 TrackVis 的 appear 时间是以 0 为基准的，直接拿负时间去比会把砖全藏掉 → 开头黑屏
    // 直到 countdown 结束（2026-10-17 用户实测"快半秒"。）
    if (t < 0.0) t = 0.0;
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
struct TrackXformCtx { const LevelData* level; const Timeline* timeline; const adofai::TileAnimation* anim; double time; };
void trackXformAt(void* ctx, uint32_t tile, float out[8]) {
    static const long g_dbgTile = [] {
        const char* e = std::getenv("ADOCAO_XFORM_DEBUG");
        return e ? std::atol(e) : -1L;
    }();
    static const long g_dbgTileHi = [] {
        const char* e = std::getenv("ADOCAO_XFORM_DEBUG");
        if (!e) return -1L;
        const char* dash = std::strchr(e, '-');
        return dash ? std::atol(dash + 1) : std::atol(e);
    }();
    const auto* c = static_cast<const TrackXformCtx*>(ctx);
    const LevelData::MoveTransform tr = c->level->moveTransformAt((int64_t)tile, c->time);
    // 逐砖关键帧时间轴（Re_ADOJAS 模型）：MoveTrack + AnimateTrack 建在同一条时间轴上。
    adofai::TileAnimSample an;
    if (c->anim && c->anim->sample((int64_t)tile, c->time, an)) {
        out[0] = an.rot;  out[1] = an.scaleX; out[2] = an.scaleY;
        out[3] = an.opacity;
        out[4] = an.posX; out[5] = an.posY;
    } else {
        out[0] = tr.rotDelta; out[1] = tr.scaleX; out[2] = tr.scaleY;
        out[3] = tr.alpha;    out[4] = tr.moveX;  out[5] = tr.moveY;
    }
    if (g_dbgTile >= 0 && (long)tile >= g_dbgTile && (long)tile <= g_dbgTileHi) {
        const float dir = c->level->tileDirectionAt((int64_t)tile);
        const float sa = c->timeline ? c->timeline->startAngleAt((int64_t)tile) : 0.0f;
        LOG_I("xform[tile=%u t=%.4f] move=(%.4f,%.4f) rot=%.4f scale=(%.4f,%.4f) alpha=%.4f | dir=%.2f sa=%.2f",
              tile, c->time, out[4], out[5], out[0], out[1], out[2], out[3],
              (double)dir, (double)sa);
    }
    // 砖心相对砖起点的偏移（Re_ADOJAS 的几何以砖心为原点：旋转/缩放绕砖心）
    const double dir = (double)c->level->tileDirectionAt((int64_t)tile) * 3.14159265358979323846 / 180.0;
    out[6] = (float)(0.5 * std::cos(dir));
    out[7] = (float)(0.5 * std::sin(dir));
}
void trackColorAt(void* ctx, uint32_t tile, uint8_t fill[3], uint8_t stroke[3], float* alpha) {
    const auto* c = static_cast<const TrackColorCtx*>(ctx);
    const TileColors col = c->level->tileColorsAt((int64_t)tile, c->time, c->amplitude, c->timeline);
    if (env::flag("ADOCAO_COLOR_DEBUG") && tile < 4)
        LOG_I("color-debug: tile=%u t=%.3f fill=%02x%02x%02x stroke=%02x%02x%02x", tile, c->time,
              col.fill[0], col.fill[1], col.fill[2], col.stroke[0], col.stroke[1], col.stroke[2]);
    fill[0] = col.fill[0]; fill[1] = col.fill[1]; fill[2] = col.fill[2];
    stroke[0] = col.stroke[0]; stroke[1] = col.stroke[1]; stroke[2] = col.stroke[2];
    *alpha = col.opacity;
}
}  // namespace

void LevelScene::render(Camera& camera, const Timeline& timeline, bool playing,
                        double timeInLevel, int highlightTile, float volumeAmplitude) {
    m_lastZoom = camera.zoom();   // sagitta 判据下一帧用（applyFrame 在 render 之前跑）
    // Track appear/disappear animation
    { profile::Scoped _s("vis"); updateTileVisibility(timeline, timeInLevel, playing); }

    auto drawFlashPass = [&](uint8_t plane) {
        if (!m_flashShader || m_flashVao == 0) return;
        const LevelData::FlashState st = m_level->flashStateAt(plane, timeInLevel);
        if (!st.active || st.alpha <= 0.001f) return;
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_flashShader->use();
        m_flashShader->setVec3("uColor", st.color[0] / 255.0f, st.color[1] / 255.0f, st.color[2] / 255.0f);
        m_flashShader->setFloat("uAlpha", st.alpha);
        glBindVertexArray(m_flashVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    };
    if (!env::flag("ADOCAO_DEBUG_NO_FLASH")) drawFlashPass(1);   // Background：在砖之前（砖会盖住它）

    m_tileMesh->setViewportHeightPx(m_viewportHpx);
    double vl, vr, vb, vt;      // 世界坐标 → double（见 docs/rendering.md 坐标约定）
    camera.frustumBounds(vl, vr, vb, vt);

    // 轨道（砖 + 图标，一次 draw）。默认逐实例颜色（ColorTrack / trackStyle）；
    // `--fill/--stroke` 显式覆盖时退回 uniform，由 uUseInstColor 切换。
    // 砖块 pass 开混合：alpha=1 时结果不变；alpha<1（颜色 hex 的 AA / MoveTrack opacity）才透。
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);          // 同一 tile 内同 z 的 part：后者被丢弃，避免自透明双叠
    glDepthMask(GL_TRUE);
    m_tileShader->use();
    m_tileShader->setMat4("uVP", glm::value_ptr(camera.viewProj()));
    m_tileShader->setVec3("uFillColor", m_fillRGB[0], m_fillRGB[1], m_fillRGB[2]);
    m_tileShader->setVec3("uStrokeColor", m_strokeRGB[0], m_strokeRGB[1], m_strokeRGB[2]);
    m_tileShader->setFloat("uOpacity", 1.0f);
    TrackXformCtx xctx{m_level, &timeline, m_tileAnim.get(), timeInLevel};
    m_tileMesh->setTime(timeInLevel);   // 时间驱动颜色/变换：相机静止时也要重算
    if (m_trackColorOverride) {
        m_tileShader->setFloat("uUseInstColor", 0.0f);
        m_tileMesh->draw(vl, vr, vb, vt, camera.targetX(), camera.targetY(),
                         nullptr, nullptr, &trackXformAt, &xctx);
    } else {
        // 逐砖颜色（ColorTrack）与变换（MoveTrack）：只对可见列表里的砖算（每帧几百次）。
        TrackColorCtx ctx{m_level, &timeline, timeInLevel, volumeAmplitude};
        m_tileShader->setFloat("uUseInstColor", 1.0f);
        m_tileMesh->draw(vl, vr, vb, vt, camera.targetX(), camera.targetY(),
                         &trackColorAt, &ctx, &trackXformAt, &xctx);
    }
    glDisable(GL_BLEND);

    // 结尾定格后仍要画行星/拖尾（官方：球停在最后一格/传送门上）
    const int64_t tn = timeline.tileCount();
    const bool ended = tn >= 2 && timeInLevel >= timeline.startTimeAt(tn - 1);

    // Trails（RAII 段：别显式调析构，那会跟作用域结束时的析构重复累加）
    {
        profile::Scoped _s("trails");
        if (m_showTrail && (playing || ended) && m_redPlanet && m_redPlanet->trail) {
            m_redPlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
            m_bluePlanet->trail->draw(*m_trailShader, camera, camera.targetX(), camera.targetY());
        }
    }

    // Planets
    {
        profile::Scoped _s("planets");
        if ((playing || ended) && m_redPlanet && m_redPlanet->gpuBuilt()) {
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
        glDisable(GL_BLEND);
        m_tileMesh->drawHighlightedTile(highlightTile, camera.targetX(), camera.targetY());
        glEnable(GL_DEPTH_TEST);
    }

    if (!env::flag("ADOCAO_DEBUG_NO_FLASH")) drawFlashPass(0);   // Foreground：最上层
}

}  // namespace adofai
