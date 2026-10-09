#include "TileMesh.hpp"
#include "glad/gl_core.hpp"
#include "render/CullSIMD.hpp"
#include "core/util/Logger.hpp"
#include "core/util/ThreadPool.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>


namespace adofai {

using TileShape::Shape;

namespace {

// 形状键：与改造前 `TileMesh::build` 逐字相同（0.01° 网格 + 中旋标志）
struct GeoKey { int sa, ea; bool mid; };
struct GeoKeyHash {
    size_t operator()(const GeoKey& k) const {
        return (size_t)k.sa * 31 + (size_t)k.ea * 17 + (size_t)k.mid;
    }
};
bool operator==(const GeoKey& a, const GeoKey& b) {
    return a.sa == b.sa && a.ea == b.ea && a.mid == b.mid;
}

ThreadPool& getPool() { static ThreadPool pool; return pool; }

inline void pushAttr(std::vector<float>& v, float ox, float oy, float oz, uint32_t shape, uint8_t bits) {
    v.push_back(ox); v.push_back(oy); v.push_back(oz);
    v.push_back((float)shape); v.push_back((float)bits);
}

} // namespace

TileMesh::~TileMesh() { destroy(); }

TileMesh::TileMesh(TileMesh&& o) noexcept
    : m_vao(o.m_vao), m_recipeVbo(o.m_recipeVbo), m_ebo(o.m_ebo), m_instVbo(o.m_instVbo),
      m_shapeTex(o.m_shapeTex), m_instCapacity(o.m_instCapacity), m_shapeRows(o.m_shapeRows),
      m_shapes(std::move(o.m_shapes)), m_tileShape(std::move(o.m_tileShape)),
      m_posX(std::move(o.m_posX)), m_posY(std::move(o.m_posY)),
      m_chunkX(std::move(o.m_chunkX)), m_chunkY(std::move(o.m_chunkY)),
      m_iconBits(std::move(o.m_iconBits)), m_visible(std::move(o.m_visible)),
      m_drawOrder(std::move(o.m_drawOrder)), m_nTiles(o.m_nTiles) {
    std::memcpy(m_fill, o.m_fill, sizeof m_fill);
    std::memcpy(m_stroke, o.m_stroke, sizeof m_stroke);
    o.m_vao = o.m_recipeVbo = o.m_ebo = o.m_instVbo = o.m_shapeTex = 0;
    o.m_nTiles = 0;
}

TileMesh& TileMesh::operator=(TileMesh&& o) noexcept {
    if (this != &o) {
        destroy();
        m_vao = o.m_vao; m_recipeVbo = o.m_recipeVbo; m_ebo = o.m_ebo;
        m_instVbo = o.m_instVbo; m_shapeTex = o.m_shapeTex;
        m_instCapacity = o.m_instCapacity; m_shapeRows = o.m_shapeRows;
        m_shapes = std::move(o.m_shapes); m_tileShape = std::move(o.m_tileShape);
        m_posX = std::move(o.m_posX); m_posY = std::move(o.m_posY);
        m_chunkX = std::move(o.m_chunkX); m_chunkY = std::move(o.m_chunkY);
        m_iconBits = std::move(o.m_iconBits); m_visible = std::move(o.m_visible);
        m_drawOrder = std::move(o.m_drawOrder); m_nTiles = o.m_nTiles;
        std::memcpy(m_fill, o.m_fill, sizeof m_fill);
        std::memcpy(m_stroke, o.m_stroke, sizeof m_stroke);
        o.m_vao = o.m_recipeVbo = o.m_ebo = o.m_instVbo = o.m_shapeTex = 0;
        o.m_nTiles = 0;
    }
    return *this;
}

void TileMesh::destroyStaticGL() {
    if (m_instVbo) glDeleteBuffers(1, &m_instVbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_recipeVbo) glDeleteBuffers(1, &m_recipeVbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_shapeTex) glDeleteTextures(1, &m_shapeTex);
    m_instVbo = m_ebo = m_recipeVbo = m_vao = m_shapeTex = 0;
    m_instCapacity = 0;
}

void TileMesh::destroy() {
    destroyStaticGL();
    m_shapes.clear(); m_tileShape.clear(); m_posX.clear(); m_posY.clear();
    m_chunkX.clear(); m_chunkY.clear();
    m_iconBits.clear(); m_visible.clear(); m_drawOrder.clear();
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

void TileMesh::build(const LevelData& level, const std::string& fillColorHex,
                     const std::string& strokeColorHex) {
    destroy();
    hexToColor3(fillColorHex, m_fill);
    hexToColor3(strokeColorHex, m_stroke);

    const auto& tiles = level.tiles;
    if (tiles.size() < 2) return;
    m_nTiles = (int)tiles.size() - 1;
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
        GeoKey k{(int)std::round(sa * 100.0f), (int)std::round(ea * 100.0f), mid};
        shapeGroups[k].push_back(i);
    }
    LOG_D("TileMesh::build: %zu unique shapes", shapeGroups.size());

    m_shapes.reserve(shapeGroups.size());
    m_tileShape.assign((size_t)n, 0);
    m_posX.assign((size_t)n, 0.0f); m_posY.assign((size_t)n, 0.0f);
    // 每块一个 double 原点（取块内第一块砖的位置）—— 块内偏移用 float 才不丢精度（见 .hpp）
    const size_t chunkSize = (size_t)1 << kPosChunkShift;
    const size_t nChunks = ((size_t)n + chunkSize - 1) / chunkSize;
    m_chunkX.assign(nChunks, 0.0); m_chunkY.assign(nChunks, 0.0);
    for (size_t c = 0; c < nChunks; c++) {
        const size_t i0 = c * chunkSize;
        m_chunkX[c] = tiles[i0].position[0];
        m_chunkY[c] = tiles[i0].position[1];
    }
    m_iconBits.assign((size_t)n, 0);
    m_drawOrder.clear(); m_drawOrder.reserve((size_t)n);

    for (auto& kv : shapeGroups) {
        const GeoKey& key = kv.first;
        std::vector<int>& tileIndices = kv.second;
        std::sort(tileIndices.begin(), tileIndices.end(), std::greater<int>());
        const uint32_t shapeIdx = (uint32_t)m_shapes.size();
        m_shapes.push_back(TileShape::buildShape(key.sa / 100.0f, key.ea / 100.0f, key.mid));
        for (int i : tileIndices) {
            m_tileShape[(size_t)i] = shapeIdx;
            const size_t ti = (size_t)i, c = ti >> kPosChunkShift;
            m_posX[ti] = (float)(tiles[ti].position[0] - m_chunkX[c]);
            m_posY[ti] = (float)(tiles[ti].position[1] - m_chunkY[c]);
            m_drawOrder.push_back((uint32_t)i);
        }
    }

    // 图标位：与改造前 `buildIcons` 的判据逐字相同（Twirl / SetSpeed 的 1.05、0.95 阈值）
    for (int i = 0; i < n; i++) {
        bool ht = i < (int)level.tileHasTwirl.size() && level.tileHasTwirl[(size_t)i];
        bool hs = i < (int)level.tileHasSetSpeed.size() && level.tileHasSetSpeed[(size_t)i];
        uint8_t bits = ht ? 1 : 0;
        if (hs && i > 0 && i < (int)level.tileBPMs.size()) {
            float r = level.tileBPMs[(size_t)i] / level.tileBPMs[(size_t)i - 1];
            if (r > 1.05f || r < 0.95f) bits |= (r > 1.05f) ? 2 : 4;
        }
        m_iconBits[(size_t)i] = bits;
    }

    buildStaticGL();
    m_listValid = false;
    m_listUploaded = false;
    LOG_D("TileMesh::build: draws=1 (tiles+icons 同一次), shapes=%zu, tiles=%d", m_shapes.size(), n);
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
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * 5 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    m_instCapacity = cap;
    const GLsizei stride = 5 * sizeof(float);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glBindVertexArray(0);
    LOG_D("TileMesh::buildStaticGL: shapeTex %dx%d, %d 槽位, %d 索引", TileShape::kTexW, m_shapeRows,
          TileShape::kTotalSlots, TileShape::kIndexCount);
}

// ---- 剔除 + 压实 ------------------------------------------------------

void TileMesh::cullRange(size_t begin, size_t end, double vl, double vr, double vb, double vt,
                         double camX, double camY, std::vector<uint32_t>& outTiles,
                         std::vector<float>& outAttr) const {
    const size_t n = m_drawOrder.size();
    if (end > n) end = n;
    if (begin >= end) return;
    outTiles.reserve(outTiles.size() + (end - begin));
    outAttr.reserve(outAttr.size() + (end - begin) * 5);
    alignas(32) double sMinX[CullSIMD::WIDTH], sMaxX[CullSIMD::WIDTH];
    alignas(32) double sMinY[CullSIMD::WIDTH], sMaxY[CullSIMD::WIDTH];
    const bool haveVis = m_visible.size() == (size_t)m_nTiles;
    constexpr size_t W = CullSIMD::WIDTH;
    size_t i = begin;
    for (; i + (W - 1) < end; i += W) {
        for (size_t b = 0; b < W; b++) {
            uint32_t t = m_drawOrder[i + b];
            const Shape& sh = m_shapes[m_tileShape[t]];
            sMinX[b] = sh.localMinX + tileX(t);
            sMaxX[b] = sh.localMaxX + tileX(t);
            sMinY[b] = sh.localMinY + tileY(t);
            sMaxY[b] = sh.localMaxY + tileY(t);
        }
        int mask = CullSIMD::test4(sMinX, sMaxX, sMinY, sMaxY, vl, vr, vb, vt);
        for (size_t b = 0; b < W; b++) {
            if (!(mask & (1 << (int)b))) continue;
            uint32_t t = m_drawOrder[i + b];
            if (haveVis && !m_visible[t]) continue;
            outTiles.push_back(t);
            // 相机相对偏移在 **double** 里算完再窄化一次：分开窄化（pos 与 cam 各自 cast）会
            // 在超大坐标上把两者的舍入误差叠加成半个格子（primer 上就是好几块砖）
            pushAttr(outAttr, (float)(tileX(t) - camX), (float)(tileY(t) - camY),
                     tileZForIndex((int)t, m_nTiles), m_tileShape[t], m_iconBits[t]);
        }
    }
    for (; i < end; i++) {
        uint32_t t = m_drawOrder[i];
        const Shape& sh = m_shapes[m_tileShape[t]];
        double mnX = sh.localMinX + tileX(t), mxX = sh.localMaxX + tileX(t);
        double mnY = sh.localMinY + tileY(t), mxY = sh.localMaxY + tileY(t);
        if (mxX < vl || mnX > vr || mxY < vb || mnY > vt) continue;
        if (haveVis && !m_visible[t]) continue;
        outTiles.push_back(t);
        pushAttr(outAttr, (float)(tileX(t) - camX), (float)(tileY(t) - camY),
                 tileZForIndex((int)t, m_nTiles), m_tileShape[t], m_iconBits[t]);
    }
}

void TileMesh::ensureDrawList(double viewL, double viewR, double viewB, double viewT,
                              double camX, double camY) const {
    const double m = 20.0;   // 与改造前同一个视锥外扩
    const double vl = viewL - m, vr = viewR + m, vb = viewB - m, vt = viewT + m;
    // 一律 double 比：float 在 x≈6.8e7 上 ulp 就是 8，`> 0.5f` 会退化成"只要变了就重建"
    // （虽然结果碰巧对，但边界与被比较的量都该是 double —— 见 docs/rendering.md 坐标约定）
    const bool frustumChanged =
        !m_listValid || std::abs(m_vl - viewL) > 0.5 ||
        std::abs(m_vr - viewR) > 0.5 || std::abs(m_vb - viewB) > 0.5 ||
        std::abs(m_vt - viewT) > 0.5;
    if (frustumChanged) {
        m_listTiles.clear();
        m_listAttr.clear();
        const size_t n = m_drawOrder.size();
        constexpr size_t CHUNK = 32768;
        const size_t chunks = (n + CHUNK - 1) / CHUNK;
        if (chunks > 1) {
            std::vector<std::vector<uint32_t>> tiles(chunks);
            std::vector<std::vector<float>> attr(chunks);
            auto& pool = getPool();
            pool.parallelFor(0, chunks, [&](size_t a, size_t b) {
                for (size_t c = a; c < b; c++) {
                    size_t lo = c * CHUNK, hi = std::min(lo + CHUNK, n);
                    cullRange(lo, hi, vl, vr, vb, vt, camX, camY, tiles[c], attr[c]);
                }
            }, 1);
            size_t totalT = 0, totalA = 0;
            for (size_t c = 0; c < chunks; c++) { totalT += tiles[c].size(); totalA += attr[c].size(); }
            m_listTiles.reserve(totalT);
            m_listAttr.reserve(totalA);
            for (size_t c = 0; c < chunks; c++) {   // 按块序拼接 = 保持绘制序
                m_listTiles.insert(m_listTiles.end(), tiles[c].begin(), tiles[c].end());
                m_listAttr.insert(m_listAttr.end(), attr[c].begin(), attr[c].end());
            }
        } else {
            cullRange(0, n, vl, vr, vb, vt, camX, camY, m_listTiles, m_listAttr);
        }

        // 窗口相对 Z（2026-10，用户指出）：全局归一化 `kMaxTileZ*(1-i/(n-1))` 的每层间距是
        // 9/(n-1) —— MYC 677 万层只有 **1.33e-6**，而 24 bit 深度在正交 0.1→200 上的分辨率是
        // **1.19e-5**：间距只有分辨率的 1/9，于是"相邻砖在深度上等于同一个值"，谁盖谁由
        // m_drawOrder（unordered_map 迭代序 + 组内降序）决定，**而不是层号** —— 屏幕只画 3 块砖
        // 也一样（Z 只看全局层号与总层数）。现在改成按**本次可见窗口** [i0,i1] 归一化：可见几百砖
        // 时每层间距 0.01~0.05（分辨率的 1000 倍），深度测试真正生效、前后由层号决定。
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

// ---- draw ------------------------------------------------------------

void TileMesh::uploadIfNeeded() const {
    const size_t count = m_listTiles.size();
    if (m_listUploaded || count == 0) return;
    if (count > m_instCapacity) {   // 可见数由屏幕决定，容量按需增长
        size_t cap = std::max(count, m_instCapacity * 2);
        glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * 5 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
        m_instCapacity = cap;
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * 5 * sizeof(float)), m_listAttr.data());
    m_listUploaded = true;
}

void TileMesh::draw(double viewL, double viewR, double viewB, double viewT,
                    double camX, double camY) const {
    m_lastDrawn = 0;
    if (empty() || !m_vao) return;
    ensureDrawList(viewL, viewR, viewB, viewT, camX, camY);
    const size_t count = m_listTiles.size();
    m_lastDrawn = (int)count;
    if (count == 0) return;
    uploadIfNeeded();
    glBindVertexArray(m_vao);
    // 形状表绑到纹理单元 0；`uShapeTex` 的默认值就是 0，所以不用设 uniform
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_shapeTex);
    // 一次画完：砖三角形 + 紧跟着的图标三角形（索引表布局，kIndexCount = 426）
    glDrawElementsInstanced(GL_TRIANGLES, TileShape::kIndexCount, GL_UNSIGNED_SHORT, nullptr,
                            (GLsizei)count);
    glBindVertexArray(0);
}

void TileMesh::drawHighlightedTile(int ti, double cX, double cY) const {
    if (ti < 0 || ti >= m_nTiles || !m_vao) return;
    float attr[5];
    attr[0] = (float)(tileX((size_t)ti) - cX);
    attr[1] = (float)(tileY((size_t)ti) - cY);
    attr[2] = tileZForIndex(ti, m_nTiles);
    attr[3] = (float)m_tileShape[(size_t)ti];
    attr[4] = (float)m_iconBits[(size_t)ti];
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_instVbo);
    if (m_instCapacity < 1) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(5 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
        m_instCapacity = 1;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(attr), attr);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_shapeTex);
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
    if (m_visible.size() != (size_t)m_nTiles) m_visible.assign((size_t)m_nTiles, 1);
    if (startTile < 0) startTile = 0;
    if (endTile >= m_nTiles) endTile = m_nTiles - 1;
    if (endTile < startTile) return;
    std::memset(m_visible.data() + startTile, visible ? 1 : 0, (size_t)(endTile - startTile + 1));
}

}  // namespace adofai
