#include "TileMesh.hpp"
#include "glad/gl_core.hpp"
#include "render/CullSIMD.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"
#include "core/util/Profile.hpp"
#include "core/util/Scheduler.hpp"
#include "core/timeline/Timeline.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <atomic>
#include <limits>


namespace adofai {

namespace {

// A/B 钩子：`ADOCAO_CULL=scan` 强制走全扫（索引路径与两段式 LOD 都要认它）
inline bool cullForceScan() {
    static const bool f = [] {
        const char* v = std::getenv("ADOCAO_CULL");
        return v && std::strcmp(v, "scan") == 0;
    }();
    return f;
}

// 建表耗时（一次性）用；只在这里用，别扩散
double msSince() {
    using clk = std::chrono::steady_clock;
    static const clk::time_point t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
// 小于这个砖数就不建索引：全扫本来就便宜，而且省下小谱的内存与建表时间
constexpr int kCullIndexMinTiles = 4096;
// 用户可选粒度的合法范围（钳制而不是报错：这是个性能旋钮，不是正确性开关）
constexpr int kCullMinBlockFloors = 64;
constexpr int kCullMaxBlockFloors = 1 << 20;
}  // namespace

using TileShape::Shape;

namespace {

// 形状键：与改造前 `TileMesh::build` 逐字相同（0.01° 网格 + 中旋标志）
// trackStyle → 几何档位。**只保留给着色/图标用，不再改尺寸**（见下面 styleWidthOf 的说明）。
// 0 Standard / 1 Neon / 2 NeonLight / 3 Basic / 4 Gems / 5 Minimal / 6 其它（按 Standard）
inline uint8_t styleIdOf(const std::string& s) {
    if (s == "Neon") return 1;
    if (s == "NeonLight") return 2;
    if (s == "Basic") return 3;
    if (s == "Gems") return 4;
    if (s == "Minimal") return 5;
    return 0;
}
// **不做按风格的尺寸放大**（Re_ADOJAS 01bc128 的 +0.025 宽 / +0.02 长）。
// 它是**逐砖累加**的：每砖长 0.49→0.51 就是 +4%，几千砖之后整条轨道被拉长，
// 实测与录制版（9ac56ec）在 t=100 s 差 26,801 像素，关掉后逐字节相同。
// 用户 2026-10-21 复现"超级拉伸那段"不一致，判定这就是原因 → 保持基准尺寸。
inline bool styleScaleEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("ADOCAO_TILE_STYLE_SCALE");
        return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
    }();
    return on;
}
inline float styleWidthOf(uint8_t id) {
    if (styleScaleEnabled() && (id == 1 || id == 2 || id == 3)) return TILE_WIDTH + 0.025f;
    return TILE_WIDTH;
}
inline float styleLengthOf(uint8_t id) {
    if (styleScaleEnabled()) {
        if (id == 1 || id == 2 || id == 3) return TILE_LENGTH + 0.02f;
        if (id == 5) return TILE_LENGTH - 0.03f;
    }
    return TILE_LENGTH;
}
inline uint8_t styleOfTile(const LevelData& lv, int64_t i, const Timeline* timeline) {
    // 必须走 trackStyleAt：trackStyle 只出现在 RecolorTrack 上，而 colorEventAt 跳过
    // 所有 kColorIsRecolor → 直接用它会全谱退化成 Standard（2025 的 1793 条 Neon 全丢，
    // 接缝处露白 + 邻砖圆角啃掉一角）。形状表是静态的 → 取该砖的最终样式。
    return styleIdOf(lv.trackStyleAt(i, std::numeric_limits<double>::infinity(), timeline));
}

struct GeoKey { int sa, ea; bool mid; uint8_t style; };
struct GeoKeyHash {
    size_t operator()(const GeoKey& k) const {
        return (size_t)k.sa * 31 + (size_t)k.ea * 17 + (size_t)k.mid;
    }
};
bool operator==(const GeoKey& a, const GeoKey& b) {
    return a.sa == b.sa && a.ea == b.ea && a.mid == b.mid && a.style == b.style;
}

inline void pushAttr(std::vector<float>& v, float ox, float oy, float oz, uint32_t shape, uint8_t bits) {
    v.push_back(ox); v.push_back(oy); v.push_back(oz);
    v.push_back((float)shape); v.push_back((float)bits);
}

} // namespace

TileMesh::~TileMesh() { destroy(); }

void TileMesh::destroyStaticGL() {
    if (m_instVbo) glDeleteBuffers(1, &m_instVbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_recipeVbo) glDeleteBuffers(1, &m_recipeVbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_shapeTex) glDeleteTextures(1, &m_shapeTex);
    if (m_slotTex) glDeleteTextures(1, &m_slotTex);
    m_instVbo = m_ebo = m_recipeVbo = m_vao = m_shapeTex = m_slotTex = 0;
    m_instCapacity = 0;
}

// 位图区间置值（闭区间）：字节内用掩码，整字节用 memset —— TrackVis 每帧只动一小段
void TileMesh::visSetRange(size_t a, size_t b, bool v) {
    if (a > b || m_visible.empty()) return;
    const size_t lastBit = (size_t)m_nTiles ? (size_t)m_nTiles - 1 : 0;
    if (b > lastBit) b = lastBit;
    if (a > b) return;
    bitRangeSet(m_visible.data(), a, b, v);   // 纯逻辑在 render/BitSpan.hpp（有独立单测）
}

void TileMesh::destroy() {
    destroyStaticGL();
    m_shapes.clear(); m_shapeIndex.clear();
    m_level = nullptr;
    m_chunkX.clear(); m_chunkY.clear();
    m_visible.clear(); m_cullIndex.clear(); m_nTiles = 0;
    m_listTiles.clear(); m_listAttr.clear();
    m_listValid = false;
    m_listUploaded = false;
    m_nTiles = 0;
    m_lastDrawn = 0;
}

bool TileMesh::empty() const { return m_nTiles <= 0 || m_shapes.empty(); }

float TileMesh::tileZForIndex(int i, int n) {
    if (n <= 1) return kMaxTileZ * 0.5f;
    return kMaxTileZ * (1.0f - (float)i / (float)(n - 1));
}

void TileMesh::hexToColor3(const std::string& hex, float out[3]) {
    unsigned v = 0;
    for (char c : hex) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else break;
    }
    out[0] = ((v >> 16) & 0xFF) / 255.0f;
    out[1] = ((v >> 8) & 0xFF) / 255.0f;
    out[2] = (v & 0xFF) / 255.0f;
}

// ---- build -----------------------------------------------------------


// ③ 切片 4：某层区间的每砖派生数据（口径与原来 build() 里的循环逐字一致）
void TileMesh::deriveRange(const LevelData& level, int64_t i0, int64_t i1, DerivedRange& out,
                          FloorWindowCache* wc) const {
    const int64_t n = (int64_t)m_nTiles;
    if (i0 < 0) i0 = 0;
    if (i1 > n) i1 = n;
    if (i1 <= i0) { out.first = i0; out.posX.clear(); out.posY.clear(); out.shape.clear(); out.icon.clear(); return; }
    const size_t cnt = (size_t)(i1 - i0);
    out.first = i0;
    out.posX.resize(cnt); out.posY.resize(cnt); out.shape.resize(cnt); out.icon.resize(cnt);
    // 样式按 **run** 复用：`trackStyleRunEnd()` 保证 [i, run.end) 内 trackStyleAt 逐位相同
    // （只覆盖"块内没有 RecolorTrack 触发"的区间，见 LevelData.cpp 的论证）。
    // 本窗口内 i 单调递增，所以一个游标就够；到边界重新查一次。
    uint8_t style = 0;
    int64_t styleEnd = i0;   // 有效期（不含）；= i0 → 第一块砖必定查询
    bool styleNoReuse = false;   // runEnd 返回负值：本块不可复用，逐砖查询
    for (int64_t i = i0; i < i1; ++i) {
        const size_t k = (size_t)(i - i0);
        const size_t ti = (size_t)i, c = ti >> kPosChunkShift;
        const auto p = level.tilePositionAt(i, wc);
        out.posX[k] = (float)(p[0] - m_chunkX[c]);
        out.posY[k] = (float)(p[1] - m_chunkY[c]);
        float sa, ea; bool mid;
        TileShape::keyForTile(level, (int)i, sa, ea, mid, wc);
        if (i >= styleEnd) {
            const LevelData::StyleRun r =
                level.trackStyleRunEnd(i, std::numeric_limits<double>::infinity(), m_timeline);
            if (!r.reusable) {               // 本块不可复用：逐砖查询到块尾，别再进本函数
                styleEnd = r.end;
                styleNoReuse = true;
            } else {
                style = styleOfTile(level, i, m_timeline);
                styleEnd = r.end;
                styleNoReuse = false;
            }
        }
        if (styleNoReuse) style = styleOfTile(level, i, m_timeline);
        const auto it = m_shapeIndex.find(packShapeKey(sa, ea, mid, style));
        if (it == m_shapeIndex.end()) {
            // **不能**静默退回形状 0：那会画出一块完全不相干的砖（同一族"为了安全其实错了"）。
            // 真出现说明 build 期与派生期的 (sa, ea, mid, style) 不一致 —— 喊出来（每次运行一次）。
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                LOG_E("TileMesh: 砖 %lld 的形状键在表里找不到（sa=%.2f ea=%.2f mid=%d style=%u）——"
                      "退回形状 0，画面会错；build 与 deriveRange 的键必须逐位一致",
                      (long long)i, sa, ea, (int)mid, (unsigned)styleOfTile(level, i, m_timeline));
            }
        }
        out.shape[k] = (it != m_shapeIndex.end()) ? it->second : 0u;
        // 图标位：与 buildIcons 的判据逐字相同（Twirl / SetSpeed 的 1.05、0.95 阈值）
        const bool ht = i < (int64_t)level.tileHasTwirl.size() && level.tileHasTwirl[(size_t)i];
        const bool hs = i < (int64_t)level.tileHasSetSpeed.size() && level.tileHasSetSpeed[(size_t)i];
        uint8_t bits = ht ? 1 : 0;
        if (hs && i > 0 && i < (int64_t)level.tileBPMs.size()) {
            const float r = level.tileBPMs[(size_t)i] / level.tileBPMs[(size_t)(i - 1)];
            if (r > 1.05f || r < 0.95f) bits |= (r > 1.05f) ? 2 : 4;
        }
        out.icon[k] = bits;
    }
}

void TileMesh::build(const LevelData& level, const std::string& fillColorHex,
                     const std::string& strokeColorHex, const Timeline* timeline) {
    destroy();
    m_timeline = timeline;
    hexToColor3(fillColorHex, m_fill);
    hexToColor3(strokeColorHex, m_stroke);

    if (level.tileCount() < 2) return;
    // ③ 切片 4：逐步改成顺序流式（现在是顺序访问密集数组，换 provider 时这里不用改）
    m_nTiles = (int)level.tileCount() - 1;
    const int n = m_nTiles;
    LOG_D("TileMesh::build start: %d tiles (1 实例/砖, GPU 展开)", n);

    // 形状分组：与改造前**同一个容器、同样的插入顺序** —— 迭代序就是绘制序（见类注释）
    // **不要 reserve()**：迭代序就是绘制序，而 reserve 会改变 libc++ 的桶增长序列 →
    // 迭代序变 → 深度量化后相邻砖重叠处的胜者变（实测 MYC t=30s 会翻 278 个像素，
    // 全部是 stroke/fill 谁在上面）。改造前没有 reserve，这里也不加。
    std::unordered_map<GeoKey, std::vector<int>, GeoKeyHash> shapeGroups;
    for (int i = 0; i < n; i++) {
        float sa, ea; bool mid;
        TileShape::keyForTile(level, i, sa, ea, mid);
        const uint8_t style = styleOfTile(level, i, m_timeline);
        GeoKey k{(int)std::round(sa * 100.0f), (int)std::round(ea * 100.0f), mid, style};
        shapeGroups[k].push_back(i);
    }
    LOG_D("TileMesh::build: %zu unique shapes", shapeGroups.size());

    m_shapes.reserve(shapeGroups.size());
    m_level = &level;

    // 每块一个 double 原点（取块内第一块砖的位置）—— 块内偏移用 float 才不丢精度（见 .hpp）
    const size_t chunkSize = (size_t)1 << kPosChunkShift;
    const size_t nChunks = ((size_t)n + chunkSize - 1) / chunkSize;
    m_chunkX.assign(nChunks, 0.0); m_chunkY.assign(nChunks, 0.0);
    for (size_t c = 0; c < nChunks; c++) {
        const size_t i0 = c * chunkSize;
        m_chunkX[c] = level.tilePositionAt(i0)[0];
        m_chunkY[c] = level.tilePositionAt(i0)[1];
    }

    // 剔除/上传都直接按下标取砖，所以这里没有任何"逐砖排列"数组了（见下面绘制顺序的说明）

    for (auto& kv : shapeGroups) {
        const GeoKey& key = kv.first;
        const uint32_t shapeIdx = (uint32_t)m_shapes.size();
        Shape sh = TileShape::buildShape(key.sa / 100.0f, key.ea / 100.0f, key.mid,
                                         styleWidthOf(key.style), styleLengthOf(key.style),
                                         key.style == 4);   // 4 = Gems
        m_shapes.push_back(sh);
        m_shapeIndex[packShapeKey(key.sa / 100.0f, key.ea / 100.0f, key.mid, key.style)] = shapeIdx;
    }
    // 绘制顺序 = **按砖号（= 层号）升序**（2026-10-15 起）：所以不再需要 `m_drawOrder` 那支
    // 4 B/砖 的排列数组（MYC 27 MB、2³¹−1 外推 **8.6 GB**）——剔除与上传**直接用下标**（下标即
    // 砖号）。依据（顺序对出图不可观测）：像素门槛 51/51 + 巨谱 16 帧 A/B（三种 Won't You、
    // TNR、primer-final 含"整张一帧"6,841 万块）**逐字节相同**，见 `docs/rendering.md`。
    // 旧的 A/B 钩子 `ADOCAO_DRAW_ORDER=grouped` 随数组一并删除（要重做就临时改提交顺序）。

    // ---- ④ 粗块 AABB 索引 ----
    // 保守半边长 = 所有形状 localBBox 的最大外扩（块 AABB 必须**包住**块内任何一块砖的
    // 真实 AABB，否则会漏砖）。n < kCullIndexMinTiles 时不建（省内存、且小谱全扫本来就便宜）。
    double half = 0.0;
    for (const TileShape::Shape& sh : m_shapes) {
        half = std::max(half, std::abs(sh.localMinX));
        half = std::max(half, std::abs(sh.localMaxX));
        half = std::max(half, std::abs(sh.localMinY));
        half = std::max(half, std::abs(sh.localMaxY));
    }
    // 粒度：设置值（`--cull-block` / wizard Graphics 页）→ env 钩子覆盖 → 合法范围钳制。
    // 0 或负数 = 关闭索引（走全扫）。
    int wantFloors = m_cullFloors;
    if (const char* e = std::getenv("ADOCAO_CULL_BLOCK")) {
        wantFloors = std::atoi(e);
        LOG_W("cull-index: ADOCAO_CULL_BLOCK=%d 覆盖了设置的粒度 %d（env 优先）", wantFloors, m_cullFloors);
    }
    if (wantFloors != 0) {
        const int clamped = wantFloors < kCullMinBlockFloors ? kCullMinBlockFloors
                          : (wantFloors > kCullMaxBlockFloors ? kCullMaxBlockFloors : wantFloors);
        if (clamped != wantFloors) {
            LOG_W("cull-index: 块粒度 %d 层超出范围，钳到 %d", wantFloors, clamped);
            wantFloors = clamped;
        }
    }
    if (wantFloors != 0 && (size_t)n >= kCullIndexMinTiles) {
        const double t0 = msSince();
        // 与剔除判定用**同一批值**（chunk 基点 + float 舍入后的块内偏移），否则块 AABB 差一丝、
        // 块选择就变、出图差几百个边缘像素（真踩过）。
        m_cullIndex.build((size_t)n, [&](size_t i, double& x, double& y) {
            const auto p = level.tilePositionAt((int64_t)i);
            const size_t c = i >> kPosChunkShift;
            x = m_chunkX[c] + (double)(float)(p[0] - m_chunkX[c]);
            y = m_chunkY[c] + (double)(float)(p[1] - m_chunkY[c]);
        }, half, (size_t)wantFloors);
        const size_t bytes = m_cullIndex.bytes();
        LOG_I("cull-index: %d 砖 → %zu 块（每 %zu 层），%zu B（%.4f B/砖；2³¹−1 外推 %.1f MB），"
              "半边长 %.3f，建表 %.1f ms",
              n, m_cullIndex.blockCount(), m_cullIndex.floorsPerBlock(), bytes,
              (double)bytes / (double)n, (double)bytes / (double)n * 2147483647.0 / 1048576.0,
              half, msSince() - t0);
    } else {
        m_cullIndex.clear();
        LOG_I("cull-index: 关闭（砖 %d < %d 或粒度为 0）→ 剔除走全扫", n, kCullIndexMinTiles);
    }

    // ③ 切片 4：四支每砖数组（原 13 B/砖）**已不常驻** —— 剔除时按 4096 层窗口从 provider 派生
    // （每窗 4096 × 13 B = 53 KB，128 槽环形 ≈ 6.8 MB 上限）。常驻的只剩形状表 + 形状索引 +
    // 每块 double 原点 + 剔除索引 + 可见位。
    LOG_I("TileMesh: 每砖**不常驻**（原 13 B/砖 = %.1f MB 已改为按窗口派生，每窗 53 KB）；"
          "常驻 %zu 形状 + 剔除索引 %.4f B/砖 + 可见位 %s",
          13.0 * (double)n / 1048576.0, m_shapes.size(),
          (double)m_cullIndex.bytes() / (double)(n ? n : 1),
          visAllocated() ? "1 bit/砖" : "未分配");

    buildStaticGL();
    m_listValid = false;
    m_listUploaded = false;
    LOG_D("TileMesh::build: draws=1 (tiles+icons 同一次), shapes=%zu, tiles=%d", m_shapes.size(), n);

    // 【诊断，env 门控】网格数据哈希：用来 A/B "按块物化 provider" 是否改变了装配结果
    if (env::flag("ADOCAO_MESH_DUMP")) {
        auto fnv = [](const void* p, size_t bytes) {
            const uint8_t* q = (const uint8_t*)p;
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < bytes; ++i) { h ^= q[i]; h *= 1099511628211ull; }
            return h;
        };
        DerivedRange all;
        deriveRange(level, 0, n, all);
        LOG_I("mesh-dump: n=%d shapes=%zu chunks=%zu tileShape=%016llx posX=%016llx posY=%016llx "
              "iconBits=%016llx chunkX=%016llx chunkY=%016llx",
              m_nTiles, m_shapes.size(), m_chunkX.size(),
              (unsigned long long)fnv(all.shape.data(), all.shape.size() * sizeof(uint32_t)),
              (unsigned long long)fnv(all.posX.data(), all.posX.size() * sizeof(float)),
              (unsigned long long)fnv(all.posY.data(), all.posY.size() * sizeof(float)),
              (unsigned long long)fnv(all.icon.data(), all.icon.size()),
              (unsigned long long)fnv(m_chunkX.data(), m_chunkX.size() * sizeof(double)),
              (unsigned long long)fnv(m_chunkY.data(), m_chunkY.size() * sizeof(double)));
    }
}

// CPU 几何回退：把每个形状的 canonical 槽位坐标展开成纹理（RG32F，一个 texel 一个槽位）。
// 值来自 `TileShape::expand()` —— 那正是 4.10 VS 的对拍基准，所以两条路画出来的顶点逐位相同，
// 差别只剩"算在 GPU 还是 CPU"。表大小 = 形状数 × 175 texel × 8 B（MYC 45 形状 ≈ 63 KB，
// 极端 8192 形状 ≈ 11 MB）。
void TileMesh::buildSlotTable() {
    const size_t slots = (size_t)TileShape::kTotalSlots;
    const size_t total = m_shapes.size() * slots;
    if (total == 0) { m_slotRows = 0; return; }
    const size_t rowTexels = (size_t)TileShape::kTexW;
    const size_t rows = (total + rowTexels - 1) / rowTexels;
    std::vector<float> texels(rows * rowTexels * 2, 0.0f);   // RG32F
    TileShape::Expanded ex;
    for (size_t si = 0; si < m_shapes.size(); si++) {
        TileShape::expand(m_shapes[si], ex);
        float* dst = texels.data() + si * slots * 2;
        for (size_t v = 0; v < slots; v++) {
            dst[v * 2 + 0] = ex.pos[v * 3 + 0];
            dst[v * 2 + 1] = ex.pos[v * 3 + 1];
        }
    }
    glGenTextures(1, &m_slotTex);
    glBindTexture(GL_TEXTURE_2D, m_slotTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, TileShape::kTexW, (GLsizei)rows, 0, GL_RG,
                 GL_FLOAT, texels.data());
    m_slotRows = (int)rows;
    LOG_D("TileMesh: CPU 展开表 %zu 形状 × %zu 槽位 → %dx%d RG32F（%zu KB）", m_shapes.size(), slots,
          TileShape::kTexW, m_slotRows, texels.size() * sizeof(float) / 1024);
}

void TileMesh::buildStaticGL() {
    const TileShape::Recipe* rc = TileShape::recipeTable();
    std::vector<float> recipes;
    recipes.reserve((size_t)TileShape::kTotalSlots * 4);
    for (int i = 0; i < TileShape::kTotalSlots; i++) {
        recipes.push_back(rc[i].part);
        recipes.push_back(rc[i].slot);
        recipes.push_back(rc[i].k0);
        recipes.push_back(rc[i].k1);
    }

    // 纹理单元 0（两支持平）：410 读"参数记录"，3.3 回退读"CPU 展开后的槽位坐标"。
    // 回退路**不写**形状表：它的 VS 一次都不读那张表，白占 shapes×25 texel。
    if (m_cpuGeometry) {
        buildSlotTable();
    } else {
        std::vector<float> texels;
        m_shapeRows = TileShape::packShapeTable(m_shapes.data(), (int)m_shapes.size(), texels);
        glGenTextures(1, &m_shapeTex);
        glBindTexture(GL_TEXTURE_2D, m_shapeTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, TileShape::kTexW, m_shapeRows, 0, GL_RGBA,
                     GL_FLOAT, texels.data());
    }

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_recipeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_recipeVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(recipes.size() * sizeof(float)), recipes.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(TileShape::kIndexCount * sizeof(uint16_t)),
                 TileShape::indexTable(), GL_STATIC_DRAW);
    glGenBuffers(1, &m_instVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
    size_t cap = (size_t)std::min(m_nTiles, 65536);
    if (cap == 0) cap = 1;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * 19 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    m_instCapacity = cap;
    const GLsizei stride = 19 * sizeof(float);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    // 逐实例 fill / stroke（0..1）。uniform uUseInstColor=0 时 shader 忽略它们（高亮/旧路径）。
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
    glVertexAttribDivisor(4, 1);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    glVertexAttribDivisor(5, 1);
    // MoveTrack 的逐实例变换：rot, scaleX, scaleY, alpha | moveX, moveY
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(11 * sizeof(float)));
    glVertexAttribDivisor(6, 1);
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, stride, (void*)(15 * sizeof(float)));
    glVertexAttribDivisor(7, 1);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, stride, (void*)(17 * sizeof(float)));
    glVertexAttribDivisor(8, 1);
    glBindVertexArray(0);
    LOG_D("TileMesh::buildStaticGL: shapeTex %dx%d, %d 槽位, %d 索引", TileShape::kTexW, m_shapeRows,
          TileShape::kTotalSlots, TileShape::kIndexCount);
}

// ---- 剔除 + 压实 ------------------------------------------------------

// ③ 切片 4【二分实验】取/物化窗口；`ADOCAO_WINDOW_CHECK=1` 时与常驻数组**逐砖直比**
// ③ 切片 4 / 并行模型：剔除用的窗口改成**线程私有**（每线程一个 4096 层窗口，65 KB）。
// 以前是"共享 LRU + 主线程 pin"：并行段不能碰共享缓存（会被撕），于是每帧最多 36% 的时间
// （`sample`：materialize 824/2284）被串行化在主线程上。现在每个线程用自己的窗口 ——
// 派生是**纯函数**（同一批输入 → 逐位相同），所以既没有竞争、结果也没变。
const TileMesh::DerivedRange* TileMesh::windowFor(size_t tile) const {
    struct TL {
        DerivedRange buf;
        int64_t blk = -1;
    };
    static thread_local TL tl;          // 每线程一份（1 个窗口即可：剔除按砖号升序走）
    if (!m_level) return nullptr;
    const int64_t b = (int64_t)(tile / kWindowFloors);
    if (tl.blk != b) {
        const int64_t i0 = b * (int64_t)kWindowFloors;
        const int64_t i1 = std::min<int64_t>(i0 + (int64_t)kWindowFloors, (int64_t)m_nTiles);
        // 线程私有窗口用**本线程的 provider 克隆**（否则 deriveRange 里的 provider 缓存仍是共享的）
        deriveRange(*m_level, i0, i1, tl.buf, m_level->threadWindowCache());
        tl.blk = b;
    }
    return &tl.buf;
}

// ⑦ 诊断用：`ADOCAO_CULL_LIMIT=0` 关掉两道预算（会退回"整张一帧"的卡死风险，仅用于对比排查）
bool TileMesh::limitOffCache() {
    static const bool off = [] {
        const char* e = std::getenv("ADOCAO_CULL_LIMIT");
        const bool o = e && std::atoi(e) == 0;
        if (o) LOG_W("cull-limit: ADOCAO_CULL_LIMIT=0 → 预算关闭（整张一帧可能卡死，仅诊断用）");
        return o;
    }();
    return off;
}

// ⑦ 限频告警（每秒最多一条，避免极端视距下刷屏）
void TileMesh::limitWarn(const char* what, size_t n) const {
    const double now = (double)std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - m_limitWarnAt < 1.0) return;
    m_limitWarnAt = now;
    LOG_W("cull-limit: %s = %zu 超过预算（候选 %zu / 实例 %zu）→ 降级为最近块 / 截断实例（⑦ 容量边界）",
          what, n, kMaxCullCandidates, kMaxInstances);
}

void TileMesh::cullRange(size_t begin, size_t end, double vl, double vr, double vb, double vt,
                         double camX, double camY, std::vector<uint32_t>& outTiles,
                         std::vector<float>& outAttr, size_t stride) const {
    const size_t n = (size_t)m_nTiles;
    if (end > n) end = n;
    if (begin >= end) return;
    outTiles.reserve(outTiles.size() + (end - begin));
    outAttr.reserve(outAttr.size() + (end - begin) * 5);
    alignas(32) double sMinX[CullSIMD::WIDTH], sMaxX[CullSIMD::WIDTH];
    alignas(32) double sMinY[CullSIMD::WIDTH], sMaxY[CullSIMD::WIDTH];
    const bool haveVis = visAllocated();
    constexpr size_t W = CullSIMD::WIDTH;
    // 按**窗口块**分段：每段取一次窗口（4096 层），块内保持原来的两段循环结构。
    // 调用方若已 pin（并行路径），首段可直接命中缓存、不产生写。
    size_t w0 = begin;
    while (w0 < end) {
    const DerivedRange* d = windowFor(w0);
    if (!d) return;
    const size_t base = (size_t)d->first;
    size_t w1 = std::min(w0 + kWindowFloors, end);
    const size_t cover = base + d->posX.size();
    if (w1 > cover) w1 = cover;
    if (w1 <= w0) break;
    auto worldX = [&](size_t t) { return m_chunkX[t >> kPosChunkShift] + (double)d->posX[t - base]; };
    auto worldY = [&](size_t t) { return m_chunkY[t >> kPosChunkShift] + (double)d->posY[t - base]; };
    size_t i = w0;
    if (stride > 1) {
        // LOD 路径：只测"砖号是 stride 倍数"的砖（确定性、与分块无关）→ 每 ~1 px 抽一块。
        // 这里用标量（SIMD 4 宽留给 stride==1 的密集情形）：LOD 是"拉远"的场景，测的砖少得多。
        const size_t first = ((w0 + stride - 1) / stride) * stride;
        for (size_t t = first; t < w1; t += stride) {
            if (haveVis && !visAt(t)) continue;
            const Shape& sh = m_shapes[d->shape[t - base]];
            const double mnX = sh.localMinX + worldX(t), mxX = sh.localMaxX + worldX(t);
            const double mnY = sh.localMinY + worldY(t), mxY = sh.localMaxY + worldY(t);
            if (mxX < vl || mnX > vr || mxY < vb || mnY > vt) continue;
            outTiles.push_back((uint32_t)t);
            pushAttr(outAttr, (float)(worldX(t) - camX), (float)(worldY(t) - camY),
                     tileZForIndex((int)t, m_nTiles), d->shape[t - base], d->icon[t - base]);
        }
        w0 = w1;
        continue;
    }
    for (; i + (W - 1) < w1; i += W) {
        for (size_t b = 0; b < W; b++) {
            const uint32_t t = (uint32_t)(i + b);   // 下标即砖号
            const Shape& sh = m_shapes[d->shape[t - base]];
            sMinX[b] = sh.localMinX + worldX(t);
            sMaxX[b] = sh.localMaxX + worldX(t);
            sMinY[b] = sh.localMinY + worldY(t);
            sMaxY[b] = sh.localMaxY + worldY(t);
        }
        int mask = CullSIMD::test4(sMinX, sMaxX, sMinY, sMaxY, vl, vr, vb, vt);
        for (size_t b = 0; b < W; b++) {
            if (!(mask & (1 << (int)b))) continue;
            const uint32_t t = (uint32_t)(i + b);
            if (haveVis && !visAt((size_t)t)) continue;
            outTiles.push_back(t);
            // 相机相对偏移在 **double** 里算完再窄化一次：分开窄化（pos 与 cam 各自 cast）会
            // 在超大坐标上把两者的舍入误差叠加成半个格子（primer 上就是好几块砖）
            pushAttr(outAttr, (float)(worldX(t) - camX), (float)(worldY(t) - camY),
                     tileZForIndex((int)t, m_nTiles), d->shape[t - base], d->icon[t - base]);
        }
    }
    for (; i < w1; i++) {
        const uint32_t t = (uint32_t)i;
        const Shape& sh = m_shapes[d->shape[t - base]];
        double mnX = sh.localMinX + worldX(t), mxX = sh.localMaxX + worldX(t);
        double mnY = sh.localMinY + worldY(t), mxY = sh.localMaxY + worldY(t);
        if (mxX < vl || mnX > vr || mxY < vb || mnY > vt) continue;
        if (haveVis && !visAt((size_t)t)) continue;
        outTiles.push_back(t);
        pushAttr(outAttr, (float)(worldX(t) - camX), (float)(worldY(t) - camY),
                 tileZForIndex((int)t, m_nTiles), d->shape[t - base], d->icon[t - base]);
    }
    w0 = w1;
    }
}

void TileMesh::ensureDrawList(double viewL, double viewR, double viewB, double viewT,
                              double camX, double camY) const {
    const double m = 20.0 + (double)m_movePad;   // 外扩含 MoveTrack 最大位移/放大
    const double vl = viewL - m, vr = viewR + m, vb = viewB - m, vt = viewT + m;
    // 一律 double 比：float 在 x≈6.8e7 上 ulp 就是 8，`> 0.5f` 会退化成"只要变了就重建"
    // （虽然结果碰巧对，但边界与被比较的量都该是 double —— 见 docs/rendering.md 坐标约定）
    //
    // 容差**按视野比例**给（≈1.25 px @1280 宽），不是固定的 0.5 世界单位：
    // 实例属性是 chunk 相对的（与相机无关），所以复用旧列表只是"集合略旧 ≤1 px"（看不见）；
    // 而固定的 0.5 世界单位在拉远时等于 0.01 px（每帧都重建 ✗）、在拉近时又过大 —— 实测
    // 饱和视距下浸泡 p50 因此从 0.17 涨到 19~31 ms（每帧都在做被预算限制的活）。
    // LOD：砖投影尺寸 < 1 px 时，按砖号**等距抽稀**（确定性、与剔除分块无关）。
    // 这不是"少画"—— 而是把"亚像素密度"换成"每像素一个采样点"：可视集合**完整且均匀**
    // （对比：预算顶住时的降级是"按距离挑最近块"，那是有偏的局部 ✗）。
    const double pxPerUnit = (viewT > viewB) ? (double)m_viewportH / (viewT - viewB) : 1.0;
    size_t lodStride = 1;
    if (std::getenv("ADOCAO_LOD") && std::atoi(std::getenv("ADOCAO_LOD")) == 0) {
        lodStride = 1;                                     // A/B：强制关掉
    } else if (pxPerUnit < 1.0) {
        lodStride = (size_t)std::ceil(1.0 / pxPerUnit);    // 每 ~1 px 抽一块
    }
    // live 下再叠加"上一帧收敛出的步长"：饱和视距每帧要扫 ~百万候选，第一遍太贵（p50 12~21 ms ✗）。
    // 反馈式种子让第一遍直接按收敛步长走 → 不再需要第二遍。**抓帧不看种子**（`m_detCull`）= 出图
    // 与历史无关 ✅（门槛/对拍都跑抓帧路径）。
    if (!m_detCull && m_lodSeed > lodStride) lodStride = m_lodSeed;
    if (lodStride != m_lodStride) {
        LOG_I("lod: 每单位 %.3f px → 抽稀步长 %zu（此前 %zu）", pxPerUnit, lodStride, m_lodStride);
        m_listValid = false;                               // 步长一变，列表作废
    }
    m_lodStride = lodStride;
    const double tolX = (viewR - viewL) / 1024.0, tolY = (viewT - viewB) / 1024.0;
    const bool frustumChanged =
        !m_listValid || std::abs(m_vl - viewL) > tolX ||
        std::abs(m_vr - viewR) > tolX || std::abs(m_vb - viewB) > tolY ||
        std::abs(m_vt - viewT) > tolY;
    if (frustumChanged) {
        m_listTiles.clear();
        m_listAttr.clear();
        const size_t n = (size_t)m_nTiles;
        constexpr size_t CHUNK = 32768;
        // A/B 钩子：ADOCAO_CULL=scan 强制走原来的全扫（同一份二进制上对比真实渲染耗时）
        const bool forceScan = cullForceScan();
        if (m_cullIndex.valid() && !forceScan) {
            // ④：**先按块筛**（块 AABB 是块内砖的保守包围盒），再在命中块内做与全扫**同一个**
            // `cullRange` 判定 —— 所以输出与全扫**逐元素相同**（`tests/cull_index_test.cpp` 对拍）。
            // 块按层号升序、块内按砖号升序 ⇒ 逐块拼接天然升序，不需要排序（顺序对出图不可观测，
            // 见类注释；像素门槛 51/51 + 巨谱 16 帧 A/B 已验证）。
            std::vector<std::pair<size_t, size_t>> ranges;   // [begin, end)
            size_t candidates = 0;
            m_cullIndex.forEachHitBlock(vl, vr, vb, vt, [&](uint32_t f, uint32_t c) {
                ranges.emplace_back((size_t)f, (size_t)f + (size_t)c);
                candidates += c;
            });
            // ⑦ 闸①：候选太多**降级**为"只画离相机最近的块"（累计候选 ≤ 预算）。
            // 不能直接不画 —— 那会整帧黑屏（只剩行星/拖尾），实测被用户一眼看出来 ✗。
            if (!limitOffCache() && candidates > kMaxCullCandidates * m_lodStride) {
                limitWarn("cull-candidates", candidates);
                const size_t before = candidates;
                // 按层号顺序边剔边收，**可见数够 kMaxInstances 就停** —— 工作量由输出封顶。
                // （不能用"按块 AABB 距离挑最近块"：块 AABB 是一整圈的包围盒（~1300 单位），
                //   小视锥会被几百个块"包住"（距离全为 0），挑出来的块里可能一块砖都不在视锥内
                //   → 实测 drawn=0、轨道整个消失 ✗。）
                // **细块粒度**上按离相机的距离挑（不是按层号顺序！按顺序收满就永远画谱开头那一段，
                // 相机在几百万层处 → 屏幕空白，用户实测截图就是那样 ✗）。
                ranges.clear();
                m_cullIndex.collectHitSubNearest(vl, vr, vb, vt, camX, camY, kMaxCullCandidates, ranges);
                // 选出来的集合不变，只把**处理顺序**改成砖号升序：窗口是 1 槽线程私有缓存，
                // 按距离的顺序会在块之间跳 → 每跳一次重推一个窗口（局部性差）
                std::sort(ranges.begin(), ranges.end());
                for (const auto& r : ranges) {
                    if (m_listTiles.size() >= kMaxInstances) break;
                    cullRange(r.first, r.second, vl, vr, vb, vt, camX, camY, m_listTiles, m_listAttr, m_lodStride);
                }
                if (m_listTiles.size() > kMaxInstances) {
                    m_listTiles.resize(kMaxInstances);
                    m_listAttr.resize(kMaxInstances * 5);
                }
                LOG_W("cull-limit: 降级为就近细块 —— 候选 %zu → %zu 块 / 可见 %zu（预算 %zu）",
                      before, ranges.size(), m_listTiles.size(), kMaxInstances);
                ranges.clear();          // 让下面的并行段变成空操作（列表已经填好了）
            }

            // 把命中块按"约 CHUNK 块砖"切成并行任务（保持块序）
            std::vector<std::pair<size_t, size_t>> jobs;     // ranges 的下标区间
            for (size_t i = 0; i < ranges.size();) {
                size_t j = i, acc = 0;
                while (j < ranges.size() &&
                       (j == i || acc + (ranges[j].second - ranges[j].first) <= CHUNK)) {
                    acc += ranges[j].second - ranges[j].first;
                    ++j;
                }
                jobs.emplace_back(i, j);
                i = j;
            }
            // 候选少就别开线程（唤醒 + 每任务两个 vector 的分配比串行做还贵）。
            // 以前这个阈值被迫定得很高（26.2 万）：并行段不能碰共享窗口缓存，只能主线程先 pin，
            // 候选一多 pin 本身就要 100 ms+ ✗。现在窗口是**线程私有**（纯函数派生）→ 阈值可以放开，
            // 让"候选多"的帧真正吃到多核。
            constexpr size_t kCullSerialMax = 1u << 14;   // 16,384 块砖
            if (jobs.size() > 1 && candidates > kCullSerialMax) {
                std::vector<std::vector<uint32_t>> tiles(jobs.size());
                std::vector<std::vector<float>> attr(jobs.size());
                auto jobBody = [&](size_t a, size_t b) {
                    for (size_t k = a; k < b; ++k) {
                        for (size_t r = jobs[k].first; r < jobs[k].second; ++r)
                            cullRange(ranges[r].first, ranges[r].second, vl, vr, vb, vt, camX, camY,
                                      tiles[k], attr[k], m_lodStride);
                    }
                };
                Scheduler::instance().parallelFor(jobs.size(), 1, Lane::Frame, jobBody);
                size_t totalT = 0, totalA = 0;
                for (size_t k = 0; k < jobs.size(); k++) { totalT += tiles[k].size(); totalA += attr[k].size(); }
                m_listTiles.reserve(totalT);
                m_listAttr.reserve(totalA);
                for (size_t k = 0; k < jobs.size(); k++) {   // 按块序拼接 = 升序
                    m_listTiles.insert(m_listTiles.end(), tiles[k].begin(), tiles[k].end());
                    m_listAttr.insert(m_listAttr.end(), attr[k].begin(), attr[k].end());
                }
            } else {
                for (const auto& r : ranges)
                    cullRange(r.first, r.second, vl, vr, vb, vt, camX, camY, m_listTiles, m_listAttr, m_lodStride);
            }
        } else if ((n + CHUNK - 1) / CHUNK > 1) {
            const size_t chunks = (n + CHUNK - 1) / CHUNK;
            std::vector<std::vector<uint32_t>> tiles(chunks);
            std::vector<std::vector<float>> attr(chunks);
            Scheduler::instance().parallelFor(chunks, 1, Lane::Frame, [&](size_t a, size_t b) {
                for (size_t c = a; c < b; c++) {
                    size_t lo = c * CHUNK, hi = std::min(lo + CHUNK, n);
                    cullRange(lo, hi, vl, vr, vb, vt, camX, camY, tiles[c], attr[c], m_lodStride);
                }
            });
            size_t totalT = 0, totalA = 0;
            for (size_t c = 0; c < chunks; c++) { totalT += tiles[c].size(); totalA += attr[c].size(); }
            m_listTiles.reserve(totalT);
            m_listAttr.reserve(totalA);
            for (size_t c = 0; c < chunks; c++) {   // 按块序拼接 = 保持绘制序
                m_listTiles.insert(m_listTiles.end(), tiles[c].begin(), tiles[c].end());
                m_listAttr.insert(m_listAttr.end(), attr[c].begin(), attr[c].end());
            }
        } else {
            cullRange(0, n, vl, vr, vb, vt, camX, camY, m_listTiles, m_listAttr, m_lodStride);
        }

        // 窗口相对 Z（2026-10，用户指出）：全局归一化 `kMaxTileZ*(1-i/(n-1))` 的每层间距是
        // 9/(n-1) —— MYC 677 万层只有 **1.33e-6**，而 24 bit 深度在正交 0.1→200 上的分辨率是
        // **1.19e-5**：间距只有分辨率的 1/9，于是"相邻砖在深度上等于同一个值"，谁盖谁由
        // 提交顺序决定，**而不是层号** —— 屏幕只画 3 块砖也一样（Z 只看全局层号与总层数）。
        // 现在改成按**本次可见窗口** [i0,i1] 归一化：可见几百砖时每层间距 0.01~0.05
        // （分辨率的 1000 倍），深度测试真正生效、前后由层号决定；并列基本消失（要跨度 >75 万层
        // 才重新出现，那时胜负也是确定的：提交序已改为按层号升序）。
        // 只在重建列表时算（平移分支不碰 Z），且不增加任何上传 —— offZ 本来就每帧上传。
        if (!m_listTiles.empty()) {
            int i0 = (int)m_listTiles[0], i1 = i0;
            for (uint32_t t : m_listTiles) {
                const int v = (int)t;
                if (v < i0) i0 = v;
                if (v > i1) i1 = v;
            }
            const float span = (i1 > i0) ? (float)(i1 - i0) : 1.0f;
            for (size_t k = 0; k < m_listTiles.size(); k++) {
                const float u = (float)((int)m_listTiles[k] - i0) / span;   // 0 = 窗口内最早（最近）
                m_listAttr[k * 5 + 2] = kMaxTileZ * (1.0f - u);
            }
        }
        m_vl = viewL; m_vr = viewR; m_vb = viewB; m_vt = viewT;
        if (const char* dp = std::getenv("ADOCAO_CULL_DUMP")) {   // 诊断：把剔除列表 dump 出来对差集
            FILE* f = std::fopen(dp, "w");
            if (f) {
                for (uint32_t t : m_listTiles) std::fprintf(f, "%u\n", t);
                std::fclose(f);
                LOG_I("cull-dump: %zu 块 → %s", m_listTiles.size(), dp);
            }
        }
        // ⑦ 闸②：可见实例封顶（确定性截断，按砖号取前 N）—— 宁可少画，不能卡死
        // 两段式 LOD：第一遍按当前步长剔完，若**真实可见数**超预算 → 用
        // `ceil(可见/预算)` 重剔一遍（确定性、无历史依赖；第二遍只测 可见/步长 ≈ 预算 块砖，
        // 所以很便宜）。这比"截断列表"好：截断丢掉的是整片区域，抽稀是均匀采样 ✓。
        if (m_listTiles.size() > kMaxInstances && !limitOffCache()) {
            const size_t visible1 = m_listTiles.size();
            const size_t need = (visible1 + kMaxInstances - 1) / kMaxInstances;
            if (need > m_lodStride) {
                LOG_I("lod: 可见 %zu > 预算 %zu → 重剔一遍，步长 %zu → %zu", visible1,
                      (size_t)kMaxInstances, m_lodStride, need);
                m_lodStride = need;
                m_listTiles.clear();
                m_listAttr.clear();
                if (m_cullIndex.valid() && !cullForceScan()) {   // 索引路径
                    m_cullIndex.forEachHitBlock(vl, vr, vb, vt, [&](uint32_t f, uint32_t c) {
                        cullRange((size_t)f, (size_t)f + (size_t)c, vl, vr, vb, vt, camX, camY,
                                  m_listTiles, m_listAttr, m_lodStride);
                    });
                } else {                                    // 全扫路径
                    cullRange(0, (size_t)m_nTiles, vl, vr, vb, vt, camX, camY, m_listTiles,
                              m_listAttr, m_lodStride);
                }
            }
        }
        // 记录收敛步长：这一帧"要不要抽"就是下一帧的种子（收紧视距时自动回落到 1 ✓）
        if (!m_detCull) m_lodSeed = (m_listTiles.size() > kMaxInstances) ? m_lodStride : 1;
        if (m_listTiles.size() > kMaxInstances && !limitOffCache()) {
            limitWarn("visible-instances", m_listTiles.size());
            m_listTiles.resize(kMaxInstances);
            m_listAttr.resize(kMaxInstances * 5);
        }
        // 逐实例砖色：只对**可见列表**算（每帧几百砖）。颜色与相机无关，只在重建列表时算；
        // 平移分支不动颜色。fn=nullptr → 全 0，shader 走 uniform（高亮/旧行为）。
        fillInstanceAnim();
        m_animTime = m_time;
        m_listValid = true;
        m_listUploaded = false;
    } else {
        // 只平移：与改造前同一条表达式（off = 世界坐标 - 相机），逐位相同
        const float dx = (float)(m_prevCamX - camX), dy = (float)(m_prevCamY - camY);
        for (size_t k = 0; k < m_listTiles.size(); k++) {
            m_listAttr[k * 5 + 0] += dx;
            m_listAttr[k * 5 + 1] += dy;
        }
        m_listUploaded = false;
    }
    m_prevCamX = camX; m_prevCamY = camY;
}

void TileMesh::fillInstanceAnim() const {
    const size_t count = m_listTiles.size();
    m_listXform.assign(count * 8, 0.0f);
    m_listColors.assign(count * 6, 0.0f);
    for (size_t k = 0; k < count; k++) {
        float* x = &m_listXform[k * 8];
        x[1] = x[2] = x[3] = 1.0f;   // scaleX/Y = 1, alpha = 1
    }
    if (m_xformFn) {
        profile::Scoped _x("animXform");
        for (size_t k = 0; k < count; k++) {
            float out[8] = {0, 1, 1, 1, 0, 0, 0, 0};
            m_xformFn(m_xformCtx, m_listTiles[k], out);
            float* x = &m_listXform[k * 8];
            for (int i = 0; i < 8; i++) x[i] = out[i];
        }
    }
    if (m_colorFn) {
        profile::Scoped _c("animColor");
        for (size_t k = 0; k < count; k++) {
            uint8_t fill[3] = {0, 0, 0}, stroke[3] = {0, 0, 0};
            float alpha = 1.0f;
            m_colorFn(m_colorCtx, m_listTiles[k], fill, stroke, &alpha);
            float* c = &m_listColors[k * 6];
            c[0] = fill[0] / 255.0f; c[1] = fill[1] / 255.0f; c[2] = fill[2] / 255.0f;
            c[3] = stroke[0] / 255.0f; c[4] = stroke[1] / 255.0f; c[5] = stroke[2] / 255.0f;
            m_listXform[k * 8 + 3] *= alpha;   // 颜色 alpha × MoveTrack opacity
        }
    }
    if (env::flag("ADOCAO_ANIM_DEBUG") && (m_colorFn || m_xformFn))
        LOG_I("anim-debug: t=%.3f n=%zu", m_time, count);
}

// ---- draw ------------------------------------------------------------

void TileMesh::uploadIfNeeded() const {
    const size_t count = m_listTiles.size();
    if (m_listUploaded || count == 0) return;
    if (count > m_instCapacity) {   // 可见数由屏幕决定，容量按需增长
        size_t cap = std::max(count, m_instCapacity * 2);
        glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * 19 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
        m_instCapacity = cap;
    }
    // 交错：5 float 几何/形状 + 6 float 颜色（颜色数组与可见列表同序）
    m_instData.resize(count * 19);
    // 开关：`ADOCAO_DRAW_ORDER=ascending`（或 `asc`）回到旧升序（A/B 用）。**默认 = 倒序**。
    // 倒序 = Re_ADOJAS 的 `renderOrder = -tileIndex`：楼层大的先画、早的后画压在邻居上面，
    // Neon 那种"黑填充 + 亮描边"在接缝处才不会断成一段段（用户说的"被狗啃"）。
    static const bool revOrder = [] {
        const char* e = std::getenv("ADOCAO_DRAW_ORDER");
        return !(e && (std::strcmp(e, "ascending") == 0 || std::strcmp(e, "asc") == 0));
    }();
    // 默认**倒序提交**（砖号大 → 小），与 Re_ADOJAS 的 `instancedMesh.renderOrder = -tileIndex` 一致：
    // 早的砖后画、压在晚的砖上面，Neon 接缝的描边才连续（升序会让后画的黑填充盖掉前一块的亮描边，
    // 就是"被狗啃"）。代价是与 2026-10-18 的录制版在重叠接缝处差一圈描边（t=118 s 逐像素 2,207），
    // 但那是旧版行为、不是参考语义 —— 裁决权在 Re_ADOJAS。
    // 升序保留为 A/B：`ADOCAO_DRAW_ORDER=ascending`。
    for (size_t k = 0; k < count; k++) {
        const size_t src = revOrder ? (count - 1 - k) : k;
        float* d = &m_instData[k * 19];
        const float* g = &m_listAttr[src * 5];
        for (int i = 0; i < 5; i++) d[i] = g[i];
        const float* c = &m_listColors[src * 6];
        for (int i = 0; i < 6; i++) d[5 + i] = c[i];
        const float* x = &m_listXform[src * 8];
        for (int i = 0; i < 8; i++) d[11 + i] = x[i];
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * 19 * sizeof(float)), m_instData.data());
    if (env::flag("ADOCAO_COLOR_DEBUG") && count > 0)
        LOG_I("inst-debug: n=%zu colors0=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f inst0=%.2f,%.2f,%.2f,%.0f,%.0f,%.3f,%.3f,%.3f",
              count, m_listColors[0], m_listColors[1], m_listColors[2], m_listColors[3],
              m_listColors[4], m_listColors[5], m_instData[0], m_instData[1], m_instData[2],
              m_instData[3], m_instData[4], m_instData[5], m_instData[6], m_instData[7]);
    m_listUploaded = true;
}

void TileMesh::draw(double viewL, double viewR, double viewB, double viewT,
                    double camX, double camY, InstanceColorFn colorFn, void* colorCtx,
                    InstanceXformFn xformFn, void* xformCtx) const {
    m_lastDrawn = 0;
    if (empty() || !m_vao) return;
    m_colorFn = colorFn;
    m_colorCtx = colorCtx;
    m_xformFn = xformFn;
    m_xformCtx = xformCtx;
    { profile::Scoped _s("cull"); ensureDrawList(viewL, viewR, viewB, viewT, camX, camY); }
    // 相机静止时列表不重建，但颜色/变换是时间驱动的（ColorTrack/MoveTrack）→ 时间变了就重算+重传。
    if (m_animNeeded && (m_colorFn || m_xformFn) && m_animTime != m_time) {
        profile::Scoped _anim("anim");
        fillInstanceAnim();
        m_animTime = m_time;
        m_listUploaded = false;
    }
    const size_t count = m_listTiles.size();
    m_lastDrawn = (int)count;
    if (count == 0) return;
    { profile::Scoped _s("upload"); uploadIfNeeded(); }
    profile::Scoped _s("draw");
    glBindVertexArray(m_vao);
    // 纹理单元 0：410 读形状表、3.3 回退读 CPU 展开表。两支 VS 的 sampler 默认值都是 0，
    // 所以不用设 uniform —— 只要保证绑的是**当前程序要的那张**。
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_cpuGeometry ? m_slotTex : m_shapeTex);
    // 一次画完：砖三角形 + 紧跟着的图标三角形（同一份索引表、同一个 draw）。
    // 图标与 fill 共面（z=0），靠 **GL_LEQUAL**（同深度后画的赢）压过 fill；
    // 不靠固定 z 偏移 —— 几百万砖同屏时可见窗口的 z 间距只有 1e-7 量级，任何常量都会穿透。
    glDrawElementsInstanced(GL_TRIANGLES, TileShape::kIndexCount, GL_UNSIGNED_SHORT, nullptr,
                            (GLsizei)count);
    glBindVertexArray(0);
}

void TileMesh::drawHighlightedTile(int ti, double cX, double cY) const {
    if (ti < 0 || ti >= m_nTiles || !m_vao) return;
    const DerivedRange* d = windowFor((size_t)ti);
    if (!d) return;
    const size_t base = (size_t)d->first, k = (size_t)ti - base;
    const double wx = m_chunkX[(size_t)ti >> kPosChunkShift] + (double)d->posX[k];
    const double wy = m_chunkY[(size_t)ti >> kPosChunkShift] + (double)d->posY[k];
    float attr[19] = {0};
    attr[12] = attr[13] = attr[14] = 1.0f;   // scaleX/Y, alpha（rot/move/pivot 保持 0）
    attr[0] = (float)(wx - cX);
    attr[1] = (float)(wy - cY);
    attr[2] = tileZForIndex(ti, m_nTiles);
    attr[3] = (float)d->shape[k];
    attr[4] = (float)d->icon[k];
    // 后 6 个（fill/stroke）留给 shader：高亮用 uniform（uUseInstColor=0），这里全 0。
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
    if (m_instCapacity < 1) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(19 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
        m_instCapacity = 1;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(attr), attr);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_cpuGeometry ? m_slotTex : m_shapeTex);   // 与 draw 同一支 VS
    glDrawElementsInstanced(GL_TRIANGLES, TileShape::kTileIndexCount, GL_UNSIGNED_SHORT, nullptr, 1);
    glBindVertexArray(0);
    m_listUploaded = false;   // 实例缓冲被这条高亮记录覆盖了（列表本身还有效）
}

// ---- TrackVis --------------------------------------------------------

void TileMesh::setVisibleThreshold(int lastVisible) {
    if (lastVisible != m_visibleThreshold) {
        m_visibleThreshold = lastVisible;
        m_listValid = false;   // 与改造前一样：阈值一变就作废剔除缓存
    }
}

void TileMesh::updateVisibleRange(int startTile, int endTile, bool visible) {
    if (m_nTiles <= 0) return;
    if (!visAllocated()) m_visible.assign((size_t)(((size_t)m_nTiles + 7) / 8), 0xFF);
    if (startTile < 0) startTile = 0;
    if (endTile >= m_nTiles) endTile = m_nTiles - 1;
    if (endTile < startTile) return;
    visSetRange((size_t)startTile, (size_t)endTile, visible);
}

}  // namespace adofai
