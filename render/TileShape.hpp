#pragma once

// 形状几何的"规范布局"（canonical layout）+ 参数表。
//
// 背景：`render/TileGeometryReference.cpp`（改造前的 `TileGeometry.cpp`，现在只进测试）把每个形状
// 展开成一份 CPU 顶点汤、每个形状一份 VBO/EBO/一次 draw。这里改成：
//
//   * **静态 canonical 顶点表**：175 个槽位 = 6 个砖 part × 2 层（描边/填充）+ 3 个图标 part × 17。
//     所有形状共用这一份（连索引表都共用），靠"每个 part 在每个模式下是否活动"来开关；
//     不活动的 part 整块塌成一个点 → 三角形零面积 → 不产生片元。
//   * **形状表**：每个形状只存 6×2 条 8 float 的 part 记录（参数），VS 按 part 公式展开。
//     于是每帧 draw 从"每形状一次"变成 1 次，形状数不再决定 draw 次数。
//
// 位精确（本任务真正的风险，见 AGENTS.md）：VS 里**没有**超越函数 —— cos/sin/pow/fmod 的结果
// 都由这里在 CPU 上算好放进记录；VS 只做与参考实现**逐字同结构**的乘加，融合步用显式 `fma()`
// 镜像编译器的 `fmadd/fmsub/fnmul`，需要单独舍入的乘积写成 `fma(a,b,0)`（= 正确舍入的乘积）。
//
// part 槽位（每层）：CIRCLE 33（圆心 + 32 环）、WEDGE 6、BIGQ 4、CAPS 8、EXT4 4、PENT 7 = 62
// 活动 part 与模式（`render/TileGeometryReference.cpp` 的分支）：
//   ARC(0<ang<120°)   → CIRCLE + WEDGE + CAPS
//   BIG(ang>=120°)    → BIGQ  + CAPS
//   ZERO(ang==0，U 型) → CIRCLE + EXT4
//   MIDSPIN(angleData=999) → PENT
// 三角形数：每层 32+4+2+4+2+3 = 47 → 砖 94 + 图标 3×16 = 142 个三角形（426 个索引）。

#include "TileGeometry.hpp"   // TILE_WIDTH / TILE_LENGTH / OUTLINE / fmodWrap / lerp
#include "core/level/LevelData.hpp"
#include <cstdint>
#include <vector>


namespace adofai {

namespace TileShape {

// ---- 布局常量 --------------------------------------------------------
enum Part {
    P_CIRCLE = 0,   // 圆弧的外/内圆盘（ARC、ZERO 用）
    P_WEDGE  = 1,   // 圆弧与两个端帽之间的填充块（ARC 用）
    P_BIGQ   = 2,   // 大角度的梯形身体（BIG 用）
    P_CAPS   = 3,   // 两个端帽矩形（ARC、BIG 用）
    P_EXT4   = 4,   // U 型（ang==0）的方块（ZERO 用）
    P_PENT   = 5,   // 中旋五边形（MIDSPIN 用）
    P_ICON_TWIRL = 6,
    P_ICON_SSUP  = 7,
    P_ICON_SSDN  = 8,
    P_COUNT
};
constexpr int kTileParts = 6;                    // P_CIRCLE..P_PENT
constexpr int kLayers = 2;                       // 0 = 描边(stroke)，1 = 填充(fill)
constexpr int kTileSlotsPerLayer = 33 + 6 + 4 + 8 + 4 + 7;   // 62
constexpr int kTileSlots = kTileSlotsPerLayer * kLayers;     // 124
constexpr int kIconSlots = 17;                               // 圆心 + 16 环
constexpr int kTotalSlots = kTileSlots + 3 * kIconSlots;     // 175
constexpr int kTileTris = 47 * kLayers;                      // 94（描边 + 填充）
constexpr int kIconTris = 3 * 16;                            // 48（三个图标 part）
constexpr int kIndexCount = (kTileTris + kIconTris) * 3;     // 426
constexpr int kTileIndexCount = kTileTris * 3;               // 282：图标 pass 的起始偏移
constexpr int kTexelsPerShape = kTileParts * kLayers * 2 + 1;   // 12 条记录 × 2 texel + 1 条标志
// 最后那个 texel = (活动位掩码, mode, 0, 0)：`p[7]` 放不下标志 —— WEDGE 的记录 8 个 float
// 全用满（s1/c1 在 p[6]/p[7]），所以标志单独给一个 texel。掩码的 bit i = P_CIRCLE+i 是否活动。
constexpr int kFlagTexel = kTexelsPerShape - 1;

// 图标（`TileMesh.cpp` 的 IR / IS / 三种颜色）
constexpr float kIconRadius = 0.11f;
constexpr int   kIconSegments = 16;
constexpr float kIconZBase = 0.002f, kIconZExtra = 0.003f;
constexpr float kTwirlColor[3] = {0.502f, 0.0f, 0.502f};
constexpr float kSSUpColor[3]  = {1.0f, 0.0f, 0.0f};
constexpr float kSSDownColor[3]= {0.0f, 0.0f, 1.0f};

// ---- 规范表 ----------------------------------------------------------
// 每个槽位一项：part（含层）、part 内槽号、以及该槽位的常量（圆环是单位圆 (cos,sin)，
// 图标槽位是圆上的实际坐标 (x,y)，其余 part 不用）。
struct Recipe { float part; float slot; float k0; float k1; };
const Recipe*  recipeTable();     // kTotalSlots 项
const uint16_t* indexTable();     // kIndexCount 项

// 每 (shape, part, 层) 一条 8 float 记录，布局按 part 固定（见 TileShape.cpp 的注释）。
// `p[7]` 是**活动标志**（1 = 该 part 在这个模式下存在）：VS 拿它决定"展开"还是"塌成一点"，
// 于是 shader 不需要知道 mode —— 与 `partActive()` 同源（buildShape 里由它填）。
struct PartRecord { float p[8]; };

// 某一层里某个 part 在给定模式下是否活动（定义在 TileShape.cpp；记录里的 p[7] 由它填）
bool partActive(int mode, int part);

// 每个 part 在同一层里占的槽位数，以及它在层内的起点（层内顺序：CIRCLE WEDGE BIGQ CAPS EXT4 PENT）
inline int partSlotCount(int part) {
    switch (part) {
        case P_CIRCLE: return 33;
        case P_WEDGE:  return 6;
        case P_BIGQ:   return 4;
        case P_CAPS:   return 8;
        case P_EXT4:   return 4;
        case P_PENT:   return 7;
    }
    return 0;
}
inline int layerBase(int layer) { return layer * kTileSlotsPerLayer; }
inline int partBase(int layer, int part) {
    int b = layerBase(layer);
    switch (part) {
        case P_CIRCLE: return b + 0;
        case P_WEDGE:  return b + 33;
        case P_BIGQ:   return b + 39;
        case P_CAPS:   return b + 43;
        case P_EXT4:   return b + 51;
        case P_PENT:   return b + 55;
    }
    return b;
}
inline int iconBase(int part) { return kTileSlots + (part - P_ICON_TWIRL) * kIconSlots; }

// 砖 i 的形状键（与 `TileMesh.cpp` 里那段逐字相同；放在这里让渲染与测试共用一份公式）。
inline void keyForTile(const LevelData& lv, int i, float& sa, float& ea, bool& mid) {
    const auto& t = lv.tiles;
    sa = (i == 0) ? -180.0f : t[(size_t)(i - 1)].direction - 180.0f;
    ea = t[(size_t)i].direction;
    mid = (i < (int)lv.angleData.size() && lv.angleData[(size_t)i] == 999.0);
}

struct Shape {
    uint8_t mode = 0;             // 0 ARC / 1 BIG / 2 ZERO / 3 MIDSPIN
    double localMinX = 0, localMinY = 0, localMaxX = 0, localMaxY = 0;   // 剔除用（与今天同算法）
    PartRecord rec[kTileParts * kLayers];
};

// 形状键（与 `TileMesh.cpp` 的 GeoKey 一致）：把 sa/ea 量化到 0.01°
inline int quantizeAngle(float deg) { return (int)std::round(deg * 100.0f); }

// 建一个形状：参数、part 记录、局部包围盒（double，逐字同今天的组包围盒算法）。
Shape buildShape(float sa, float ea, bool mid);

// ---- 形状表（GPU 端 = RGBA32F 纹理，texelFetch 读） --------------------
// 布局：一行 kTexW 个 texel，每个形状 kTexelsPerShape 个 texel，第 (layer*kTileParts + part)
// 条记录占 2 个 texel（8 float）—— 与 shader 里的索引算式必须一致。
constexpr int kTexW = 1024;
// 打包成 texel 流；返回需要的行数 H（纹理尺寸 kTexW × H）。out 会被 resize。
int packShapeTable(const Shape* shapes, int count, std::vector<float>& out);

// CPU 模型（与 `assets/shaders/tile.vert` 的展开逐字同结构；L1 用它和参考实现逐位对拍）。
struct Expanded {
    float pos[kTotalSlots * 3];   // 局部坐标
    float type[kTotalSlots];      // 0 = 描边，1 = 填充
};
void expand(const Shape& s, Expanded& out);

// 某一层里某个 part 在给定模式下是否活动（VS 里同一张表）
bool partActive(int mode, int part);

} // namespace TileShape

}  // namespace adofai
