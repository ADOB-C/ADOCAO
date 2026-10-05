#pragma once

#include "glad/gl_core.hpp"
#include "core/level/LevelData.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <tuple>

struct TileInstance {
    double offX, offY; float offZ;
    float fillR, fillG, fillB, strokeR, strokeG, strokeB, opacity;
    double minX, minY, maxX, maxY;
};

struct ShapeGroup {
    GLuint vao = 0, vbo = 0, ebo = 0, instVbo = 0, colorVbo = 0;
    unsigned indexCount = 0;
    unsigned strokeIndexCount = 0, fillIndexCount = 0, fillIndexByteOffset = 0;

    size_t instanceCount = 0;
    // 逐实例 AABB（原来的 cullMinX/… 四个 double，32 B/实例）是**可推导的缓存**：
    // 世界 AABB = 砖位置（`level.tiles[i].position`，double）+ 组内几何的局部包围盒
    // （每组只有一份）。所以只留每组这一份，剔除时按 SIMD 批现算进 scratch —— 同样的
    // double 加法、同样喂给 `CullSIMD::test4`，因此剔除结果与像素**逐位不变**，
    // 省下 32 B/实例（677 万层 ≈ 217 MB）。`posX/Y/Z` 是每帧上传用的，保留。
    double localMinX = 0, localMinY = 0, localMaxX = 0, localMaxY = 0;
    float* posX = nullptr;
    float* posY = nullptr;
    float* posZ = nullptr;

    double groupMinX = 1e99, groupMinY = 1e99;
    double groupMaxX = -1e99, groupMaxY = -1e99;

    // Legacy culling: keep AoS instance data (only used when legacyCulling=true)
    std::vector<TileInstance> instances;
};

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
               const std::string& strokeColorHex = "000000",
               bool legacyCulling = false);
    void draw(float viewL, float viewR, float viewB, float viewT, double camX, double camY) const;
    void drawIcons(float viewL, float viewR, float viewB, float viewT, double camX, double camY) const;
    void drawHighlightedTile(int tileIdx, double camX, double camY) const;
    void setVisibleThreshold(int lastVisible);
    void updateVisibleRange(int startTile, int endTile, bool visible);
    bool empty() const;

    struct VisibilityCache {
        std::vector<int> indices;
        std::vector<float> offsets;
        double vl=0, vr=0, vb=0, vt=0, prevCamX=0, prevCamY=0;
        bool valid=false, offsetsValid=false;
    };

    static bool frustumCheck(const VisibilityCache& c, float vl, float vr, float vb, float vt) {
        if (!c.valid) return true;
        return std::abs((float)c.vl-vl)>0.5f || std::abs((float)c.vr-vr)>0.5f
            || std::abs((float)c.vb-vb)>0.5f || std::abs((float)c.vt-vt)>0.5f;
    }
    static constexpr float kMaxTileZ = 9.0f, kIconZBase = 0.002f, kIconZExtra = 0.003f;
    static float tileZForIndex(int i, int n);

private:
    std::vector<ShapeGroup> m_shapes;
    std::vector<ShapeGroup> m_iconGroups;
    mutable std::vector<VisibilityCache> m_visCaches;
    mutable std::vector<VisibilityCache> m_iconVisCaches;
    std::vector<int> m_tileToShape, m_tileToInstance;
    // 剔除时要现算世界 AABB，所以留一个砖位置数组的指针（build 时取，随关卡存活；
    // tiles 在加载后不会再被 resize，releaseMemory() 也不动它）。
    const LevelData::Tile* m_tilePtr = nullptr;

    bool m_legacyCulling = false;
    mutable int m_visibleThreshold = 0x7fffffff;
    std::vector<std::vector<int>> m_sgTileIndices;   // per-group, per-instance → global tile index
    std::vector<std::vector<int>> m_sgIconTileIndices; // same for icon groups
    std::vector<std::vector<uint8_t>> m_sgVisible;    // per-group, per-instance visibility (1=vis)
    std::vector<std::vector<uint8_t>> m_sgIconVisible; // same for icons

    // CSR icon index: icon instances bucketed by their tile. m_iconEntryFirst is
    // size (nTiles+1); icons of tile t are m_iconEntries[first[t] .. first[t+1]).
    // Each entry packs (iconGroupIndex << 20) | localInstanceIndex.
    // Lets updateVisibleRange() flip only icons on the affected tiles instead of
    // scanning every icon group (was O(totalIcons) per changed tile, which froze
    // playback on huge charts like 6.7M tiles / 3M twirl icons).
    std::vector<uint32_t> m_iconEntryFirst;
    std::vector<uint32_t> m_iconEntries;

    void destroy();
    void buildIcons(const LevelData& level);
    static unsigned int hexToUInt(const std::string& hex);
    static void freeSoA(ShapeGroup& sg);
    static void allocSoA(ShapeGroup& sg, size_t n);
};
