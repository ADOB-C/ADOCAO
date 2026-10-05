#pragma once

#include "glad/gl_core.hpp"
#include "core/level/LevelData.hpp"
#include "render/TileShape.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// 轨道渲染：**1 实例 = 1 砖**，几何在 VS 里按"形状表"展开（见 render/TileShape.hpp 与
// assets/shaders/tile.vert）。2026-10 之前是"每个形状一份 CPU 顶点汤 + 每形状一次 draw"。
//
// 一个实例画完整块砖 + 它的事件图标：canonical 顶点表 175 槽（砖 124 + 图标 51）、
// 142 个三角形共用一份索引表，所以整条轨道**一次 draw** 画完（形状数不再决定 draw 次数：
// 之前 MYC 294 次、Singularity 8192 次、ADR 型全角度谱面会到十万次量级）。
//
// 常驻（每砖）≈ 14 B：形状号 4 + 世界坐标 8 + 图标位 1 + 可见位 1（TrackVis 才分配）；
// 之前是"砖 24 B + 每实例 pos/color/tileIdx/可见 ≈ 57 B（含图标实例 616 万）"。
// 每帧只上传**可见**实例（20 B/实例：相机相对偏移 3 + 形状号 1 + 图标位 1）。
//
// 绘制顺序：与改造前**逐字相同**（`unordered_map<GeoKey>` 的迭代序 + 组内下标降序）。
// 这条不是洁癖：深度 24 bit 下相邻砖的深度会量化到同一个值，重叠处谁赢由先后决定 ——
// 实测把组内顺序反过来，MYC t=30s 那一帧会有 102 个像素不同（maxdelta 111）。

namespace adofai {

class TileMesh {
public:
    TileMesh() = default;
    ~TileMesh();
    TileMesh(const TileMesh&) = delete;
    TileMesh& operator=(const TileMesh&) = delete;
    TileMesh(TileMesh&&) noexcept;
    TileMesh& operator=(TileMesh&&) noexcept;

    void build(const LevelData& level,
               const std::string& fillColorHex = "FFFFFF",
               const std::string& strokeColorHex = "000000");
    // **一次 draw** 画完砖与图标（图标 part 紧跟在自己那块砖的填充之后）—— 指数的索引表
    // 顺序就是"描边 → 填充 → 三个图标 part"。
    //
    // 顺序上的一个**故意修复**：改造前是"砖 → 拖尾 → 行星 → 图标"，图标画在拖尾之后。图标是
    // 不透明的、拖尾是半透明混合，于是图标会把拖尾"擦"掉一块 —— 那是 bug（用户 2026-10 指出）。
    // 现在图标跟着砖走，在拖尾之前，拖尾正常盖在图标上。这个修复会让"图标与拖尾重叠"的像素
    // 与旧版不同（The Moon t=1s / MYC t=30s 等），是**预期内**的差异，基线已按新行为重存。
    void draw(float viewL, float viewR, float viewB, float viewT, double camX, double camY) const;
    void drawHighlightedTile(int tileIdx, double camX, double camY) const;
    void setVisibleThreshold(int lastVisible);
    void updateVisibleRange(int startTile, int endTile, bool visible);
    bool empty() const;

    static constexpr float kMaxTileZ = 9.0f;
    static float tileZForIndex(int i, int n);
    static void hexToColor3(const std::string& hex, float out[3]);

    // 验收/诊断用：上次 draw 实际画了多少个实例、形状数
    int lastDrawnInstances() const { return m_lastDrawn; }
    size_t shapeCount() const { return m_shapes.size(); }

private:
    void destroy();
    void buildStaticGL();
    void destroyStaticGL();
    void ensureDrawList(float vl, float vr, float vb, float vt, double camX, double camY) const;
    void uploadIfNeeded() const;
    void cullRange(size_t begin, size_t end, double vl, double vr, double vb, double vt,
                   double camX, double camY, std::vector<uint32_t>& outTiles,
                   std::vector<float>& outAttr) const;

    // ---- 静态 GL（所有实例共用）----
    GLuint m_vao = 0, m_recipeVbo = 0, m_ebo = 0, m_instVbo = 0, m_shapeTex = 0;
    mutable size_t m_instCapacity = 0;   // 实例缓冲容量（draw() 里会按需增长）
    int m_shapeRows = 0;
    float m_fill[3] = {1, 1, 1}, m_stroke[3] = {0, 0, 0};

    // ---- 每砖常驻 ----
    std::vector<TileShape::Shape> m_shapes;   // 形状（去重；含 localBBox）
    std::vector<uint32_t> m_tileShape;        // 砖 → 形状号
    std::vector<float> m_posX, m_posY;        // 砖的世界坐标（float，与改造前同精度）
    std::vector<uint8_t> m_iconBits;          // bit0 twirl / bit1 ssUp / bit2 ssDown
    std::vector<uint8_t> m_visible;           // lazy：只有 TrackVis 需要
    std::vector<uint32_t> m_drawOrder;        // 改造前的绘制顺序（见类注释）
    int m_nTiles = 0;

    // ---- 每帧可见实例（compaction 结果；draw 时上传）----
    mutable std::vector<uint32_t> m_listTiles;
    mutable std::vector<float> m_listAttr;    // 5 float/实例：offX offY offZ shape iconBits
    mutable bool m_listValid = false;
    mutable bool m_listUploaded = false;   // 实例缓冲里是不是这一帧的列表
    mutable double m_vl = 0, m_vr = 0, m_vb = 0, m_vt = 0;
    mutable int m_visibleThreshold = 0x7fffffff;
    mutable int m_lastDrawn = 0;
    mutable double m_prevCamX = 0, m_prevCamY = 0;
};

}  // namespace adofai
