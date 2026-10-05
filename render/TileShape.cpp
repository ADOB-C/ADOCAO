#include "TileShape.hpp"

#include <cmath>
#include <cstring>
#include <vector>

// 本文件是"形状 → part 记录"和"part 记录 → 顶点"的唯一 CPU 实现：
//   * `buildShape()` 把 `TileGeometryReference.cpp`（改造前那份）里的参数推导逐字搬过来，
//     只是把中间量**存下来**（cx/cy/rad/width/length、m11/m12/m21/m22、s0/c0/s1/c1、mx/my），
//     而不是继续展开成顶点。
//   * `expand()` 是 `assets/shaders/tile.vert` 的 CPU 镜像（同 part 分派、同融合结构），
//     给 L1 逐位对拍用（`tests/tile_expansion_test.cpp`）。
//
// 融合结构（为什么这里到处是 `std::fma`）：参考实现是 -O3 -march=native 编出来的，`a*b+c`
// 会被编成 `fmadd/fmsub/fnmul`（实测 TileGeometry.o 里 45 fmul + 45 fmadd + 16 fnmsub）。
// 要逐位相同，VS 必须复现同一个舍入序列：融合处用显式 `fma()`，需要单独舍入的乘积放进
// 独立变量（只被 fma 消费，编译器不会把它并进别的加法里）。
// 注意：`std::fma(a, b, 0.0f)` 就是"正确舍入的乘积"，与单独一条 `fmul` 同值。


namespace adofai {

namespace TileShape {

// ---- 静态表 ----------------------------------------------------------

static std::vector<Recipe> g_recipes;
static std::vector<uint16_t> g_indices;

static void buildTablesOnce() {
    if (!g_recipes.empty()) return;
    g_recipes.assign(kTotalSlots, Recipe{0, 0, 0, 0});
    // 砖：每层的 part 槽位。圆的环用 `createCircle` 里同一批单位圆常量
    // （`a = 2πi/res`，`std::cos/std::sin` 的 float 结果；槽 0 是圆心，常量不用）。
    for (int layer = 0; layer < kLayers; layer++) {
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            int base = partBase(layer, part);
            int n = partSlotCount(part);
            for (int i = 0; i < n; i++) {
                Recipe r;
                // 编码（与 tile_v2.vert / geom_probe 一致）：part = part id（0..5 砖，6..8 图标），
                // slot 字段装 layer*64 + 槽号 —— 因为砖 part 的"层"必须跟着槽位走。
                r.part = (float)part;
                r.slot = (float)(layer * 64 + i);
                r.k0 = r.k1 = 0.0f;
                if (part == P_CIRCLE && i > 0) {
                    float a = (2.0f * 3.14159265f * (float)(i - 1)) / 32.0f;
                    r.k0 = std::cos(a);
                    r.k1 = std::sin(a);
                }
                g_recipes[(size_t)(base + i)] = r;
            }
        }
    }
    // 图标：3 个 part，每个 17 槽（圆心 + 16 环），常量直接是圆上的局部坐标
    // （就是 `createCircle(0,0,IR,1.0,sc,IS)` 吐出来的那批 float）。
    for (int part = P_ICON_TWIRL; part <= P_ICON_SSDN; part++) {
        int base = iconBase(part);
        for (int i = 0; i < kIconSlots; i++) {
            Recipe r;
            r.part = (float)part;
            r.slot = (float)i;
            if (i == 0) { r.k0 = 0.0f; r.k1 = 0.0f; }
            else {
                float a = (2.0f * 3.14159265f * (float)(i - 1)) / (float)kIconSegments;
                r.k0 = std::cos(a) * kIconRadius;
                r.k1 = std::sin(a) * kIconRadius;
            }
            g_recipes[(size_t)(base + i)] = r;
        }
    }
    // 索引：按 part 顺序，镜像参考实现的三角形列表（层的顺序 = 描边再填充；
    // 同层内 part 的先后不影响像素——同色且同深度，LEQUAL 谁赢都是同一个颜色）。
    auto tri = [](uint16_t a, uint16_t b, uint16_t c) {
        g_indices.push_back(a); g_indices.push_back(b); g_indices.push_back(c);
    };
    for (int layer = 0; layer < kLayers; layer++) {
        int c = (uint16_t)partBase(layer, P_CIRCLE);
        for (int i = 1; i < 32; i++) tri((uint16_t)c, (uint16_t)(c + i), (uint16_t)(c + i + 1));
        tri((uint16_t)c, (uint16_t)(c + 32), (uint16_t)(c + 1));      // 32 个扇形三角形
        int w = (uint16_t)partBase(layer, P_WEDGE);
        tri((uint16_t)w, (uint16_t)(w + 1), (uint16_t)(w + 5)); tri((uint16_t)(w + 4), (uint16_t)(w + 1), (uint16_t)(w + 5));
        tri((uint16_t)(w + 2), (uint16_t)(w + 3), (uint16_t)(w + 4)); tri((uint16_t)(w + 1), (uint16_t)(w + 3), (uint16_t)(w + 4));
        int q = (uint16_t)partBase(layer, P_BIGQ);
        tri((uint16_t)q, (uint16_t)(q + 1), (uint16_t)(q + 2)); tri((uint16_t)(q + 2), (uint16_t)(q + 3), (uint16_t)q);
        int k = (uint16_t)partBase(layer, P_CAPS);
        tri((uint16_t)k, (uint16_t)(k + 1), (uint16_t)(k + 2)); tri((uint16_t)(k + 2), (uint16_t)(k + 3), (uint16_t)k);
        tri((uint16_t)(k + 4), (uint16_t)(k + 5), (uint16_t)(k + 6)); tri((uint16_t)(k + 6), (uint16_t)(k + 7), (uint16_t)(k + 4));
        int e = (uint16_t)partBase(layer, P_EXT4);
        tri((uint16_t)e, (uint16_t)(e + 1), (uint16_t)(e + 2)); tri((uint16_t)(e + 2), (uint16_t)(e + 3), (uint16_t)e);
        int p = (uint16_t)partBase(layer, P_PENT);
        tri((uint16_t)p, (uint16_t)(p + 1), (uint16_t)(p + 2));
        tri((uint16_t)(p + 2), (uint16_t)(p + 3), (uint16_t)p);
        tri((uint16_t)(p + 4), (uint16_t)(p + 5), (uint16_t)(p + 6));
    }
    for (int part = P_ICON_TWIRL; part <= P_ICON_SSDN; part++) {
        int c = (uint16_t)iconBase(part);
        for (int i = 1; i < kIconSegments; i++)
            tri((uint16_t)c, (uint16_t)(c + i), (uint16_t)(c + i + 1));
        tri((uint16_t)c, (uint16_t)(c + kIconSegments), (uint16_t)(c + 1));
    }
}

const Recipe* recipeTable() { buildTablesOnce(); return g_recipes.data(); }
const uint16_t* indexTable() { buildTablesOnce(); return g_indices.data(); }

bool partActive(int mode, int part) {
    switch (mode) {
        case 0: return part == P_CIRCLE || part == P_WEDGE || part == P_CAPS;   // ARC
        case 1: return part == P_BIGQ || part == P_CAPS;                        // BIG
        case 2: return part == P_CIRCLE || part == P_EXT4;                      // ZERO
        case 3: return part == P_PENT;                                          // MIDSPIN
    }
    return false;
}

// ---- 建形状 ----------------------------------------------------------

// 记录槽位约定（每个 part 固定 8 个 float）：
//   CIRCLE: cx, cy, r
//   WEDGE : cx, cy, r, w, s0, c0, s1, c1
//   BIGQ  : cx, cy, w, s0, c0, s1, c1
//   CAPS  : m11, m12, m21, m22, w, l
//   EXT4  : mx, my, m11, m12, w, l
//   PENT  : mx, my, m11, m12, w, l
static void setRec(PartRecord& r, float a0 = 0, float a1 = 0, float a2 = 0, float a3 = 0,
                   float a4 = 0, float a5 = 0, float a6 = 0, float a7 = 0) {
    r.p[0] = a0; r.p[1] = a1; r.p[2] = a2; r.p[3] = a3;
    r.p[4] = a4; r.p[5] = a5; r.p[6] = a6; r.p[7] = a7;
}
static PartRecord& rec(Shape& s, int part, int layer) { return s.rec[part * kLayers + layer]; }

Shape buildShape(float sa, float ea, bool mid) {
    constexpr float PI = 3.14159265f;
    Shape s;
    // 参考实现：m11 = cos(sa*PI/180), m12 = sin(...), m21 = cos(ea*PI/180), m22 = sin(...)
    float m11 = std::cos(sa * PI / 180.0f), m12 = std::sin(sa * PI / 180.0f);
    float m21 = std::cos(ea * PI / 180.0f), m22 = std::sin(ea * PI / 180.0f);

    if (mid) {
        // createMidSpinMesh(a1 = sa)：五边形
        s.mode = 3;
        float width = TILE_WIDTH, length = TILE_WIDTH;
        float m1 = m11, m2 = m12;
        float mx = -m1 * 0.04f, my = -m2 * 0.04f;
        float widthS = width + OUTLINE, lengthS = length + OUTLINE;
        float widthF = widthS - OUTLINE * 2, lengthF = lengthS - OUTLINE * 2;
        setRec(rec(s, P_PENT, 0), mx, my, m11, m12, widthS, lengthS);
        setRec(rec(s, P_PENT, 1), mx, my, m11, m12, widthF, lengthF);
        setRec(rec(s, P_CIRCLE, 0)); setRec(rec(s, P_CIRCLE, 1));   // 不活动（塌成一点）
        setRec(rec(s, P_WEDGE, 0));  setRec(rec(s, P_WEDGE, 1));
        setRec(rec(s, P_BIGQ, 0));   setRec(rec(s, P_BIGQ, 1));
        setRec(rec(s, P_CAPS, 0));   setRec(rec(s, P_CAPS, 1));
        setRec(rec(s, P_EXT4, 0));   setRec(rec(s, P_EXT4, 1));
    } else {
        float width = TILE_WIDTH, length = TILE_LENGTH;
        float a0, a1;
        if (fmodWrap(sa - ea, 360) >= fmodWrap(ea - sa, 360)) {
            a0 = fmodWrap(sa, 360) * PI / 180; a1 = a0 + fmodWrap(ea - sa, 360) * PI / 180;
        } else {
            a0 = fmodWrap(ea, 360) * PI / 180; a1 = a0 + fmodWrap(sa - ea, 360) * PI / 180;
        }
        float ang = a1 - a0, midA = a0 + ang / 2;
        float s0 = std::sin(a0), c0 = std::cos(a0), s1 = std::sin(a1), c1 = std::cos(a1);

        if (ang < 2.0943952f && ang > 0) {
            s.mode = 0;
            float x;
            if (ang < 0.08726646f) x = 1;
            else if (ang < 0.5235988f) x = lerp(1, 0.83f, std::pow((ang - 0.08726646f) / 0.43633235f, 0.5f));
            else if (ang < 0.7853982f) x = lerp(0.83f, 0.77f, std::pow((ang - 0.5235988f) / 0.2617994f, 1));
            else if (ang < 1.5707964f) x = lerp(0.77f, 0.15f, std::pow((ang - 0.7853982f) / 0.7853982f, 0.7f));
            else x = lerp(0.15f, 0, std::pow((ang - 1.5707964f) / 0.5235988f, 0.5f));

            float dist, rad;
            if (x == 1) { dist = 0; rad = width; }
            else { rad = lerp(0, width, x); dist = (width - rad) / std::sin(ang / 2); }
            float cx = -dist * std::cos(midA), cy = -dist * std::sin(midA);

            // 外层（描边）：宽度/长度/半径各 +OUTLINE
            float widthS = width + OUTLINE, lengthS = length + OUTLINE, radS = rad + OUTLINE;
            // 内层（填充）：再各 −2*OUTLINE；半径 <0 时参考实现会重算圆心
            float widthF = widthS - OUTLINE * 2, lengthF = lengthS - OUTLINE * 2, radF = radS - OUTLINE * 2;
            float cxF = cx, cyF = cy;
            if (radF < 0) {
                radF = 0;
                cxF = (-widthF / std::sin(ang / 2)) * std::cos(midA);
                cyF = (-widthF / std::sin(ang / 2)) * std::sin(midA);
            }
            setRec(rec(s, P_CIRCLE, 0), cx, cy, radS);
            setRec(rec(s, P_WEDGE, 0), cx, cy, radS, widthS, s0, c0, s1, c1);
            setRec(rec(s, P_CAPS, 0), m11, m12, m21, m22, widthS, lengthS);
            setRec(rec(s, P_CIRCLE, 1), cxF, cyF, radF);
            setRec(rec(s, P_WEDGE, 1), cxF, cyF, radF, widthF, s0, c0, s1, c1);
            setRec(rec(s, P_CAPS, 1), m11, m12, m21, m22, widthF, lengthF);
            setRec(rec(s, P_BIGQ, 0)); setRec(rec(s, P_BIGQ, 1));
            setRec(rec(s, P_EXT4, 0)); setRec(rec(s, P_EXT4, 1));
            setRec(rec(s, P_PENT, 0)); setRec(rec(s, P_PENT, 1));
        } else if (ang > 0) {
            s.mode = 1;
            float widthS = width + OUTLINE, lengthS = length + OUTLINE;
            float cx = (-widthS / std::sin(ang / 2)) * std::cos(midA);
            float cy = (-widthS / std::sin(ang / 2)) * std::sin(midA);
            float widthF = widthS - OUTLINE * 2, lengthF = lengthS - OUTLINE * 2;
            float cxF = (-widthF / std::sin(ang / 2)) * std::cos(midA);
            float cyF = (-widthF / std::sin(ang / 2)) * std::sin(midA);
            setRec(rec(s, P_BIGQ, 0), cx, cy, widthS, s0, c0, s1, c1);
            setRec(rec(s, P_CAPS, 0), m11, m12, m21, m22, widthS, lengthS);
            setRec(rec(s, P_BIGQ, 1), cxF, cyF, widthF, s0, c0, s1, c1);
            setRec(rec(s, P_CAPS, 1), m11, m12, m21, m22, widthF, lengthF);
            setRec(rec(s, P_CIRCLE, 0)); setRec(rec(s, P_CIRCLE, 1));
            setRec(rec(s, P_WEDGE, 0)); setRec(rec(s, P_WEDGE, 1));
            setRec(rec(s, P_EXT4, 0)); setRec(rec(s, P_EXT4, 1));
            setRec(rec(s, P_PENT, 0)); setRec(rec(s, P_PENT, 1));
        } else {
            // ang == 0：U 型。length = width；圆半径 = width；圆心 = (mx,my)
            s.mode = 2;
            float m1 = m11, m2 = m12;
            float mx = -m1 * 0.04f, my = -m2 * 0.04f;
            length = width;
            float widthS = width + OUTLINE, lengthS = length + OUTLINE;
            float widthF = widthS - OUTLINE * 2, lengthF = lengthS - OUTLINE * 2;
            setRec(rec(s, P_CIRCLE, 0), mx, my, widthS);
            setRec(rec(s, P_EXT4, 0), mx, my, m11, m12, widthS, lengthS);
            setRec(rec(s, P_CIRCLE, 1), mx, my, widthF);
            setRec(rec(s, P_EXT4, 1), mx, my, m11, m12, widthF, lengthF);
            setRec(rec(s, P_WEDGE, 0)); setRec(rec(s, P_WEDGE, 1));
            setRec(rec(s, P_BIGQ, 0)); setRec(rec(s, P_BIGQ, 1));
            setRec(rec(s, P_CAPS, 0)); setRec(rec(s, P_CAPS, 1));
            setRec(rec(s, P_PENT, 0)); setRec(rec(s, P_PENT, 1));
        }
    }

    // 局部包围盒：与今天的 `TileMesh::build` 同一算法（float 顶点 → double 逐点 min/max），
    // 只统计活动 part 的槽位（= 参考实现真正吐出来的那些顶点）。剔除结果因此逐位不变。
    Expanded ex;
    expand(s, ex);
    double mnX = 1e99, mnY = 1e99, mxX = -1e99, mxY = -1e99;
    for (int layer = 0; layer < kLayers; layer++) {
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            if (!partActive(s.mode, part)) continue;
            int base = partBase(layer, part);
            int n = partSlotCount(part);
            for (int i = 0; i < n; i++) {
                double lx = ex.pos[(size_t)(base + i) * 3], ly = ex.pos[(size_t)(base + i) * 3 + 1];
                if (lx < mnX) mnX = lx;
                if (lx > mxX) mxX = lx;
                if (ly < mnY) mnY = ly;
                if (ly > mxY) mxY = ly;
            }
        }
    }
    s.localMinX = mnX; s.localMinY = mnY; s.localMaxX = mxX; s.localMaxY = mxY;
    return s;
}

// `rp(a,b)` = "单独舍入的乘积"：`std::fma(a,b,0)` 就是正确舍入的 a*b，与参考实现里单独一条
// `fmul` 同值；而写成裸的 `a*b` 会被编译器（合法地）融合进旁边那次加法，差 1 ULP。
// 什么时候必须用 rp()：只有逐位对拍能回答 —— 同一个算式在不同 part 里可能被编成不同的融合
// 结构（PENT 与 EXT4 就是这样），所以每个 part 的结构都是实测钉死的。
static inline float rp(float a, float b) { return std::fma(a, b, 0.0f); }

// ---- CPU 镜像展开（= tile.vert 的几何部分） ---------------------------
// 塌陷点：不活动的 part 全部落在这里，三个顶点同值 → 零面积 → 不产生片元。
static constexpr float kCollapsed[3] = {0.0f, 0.0f, 0.0f};

void expand(const Shape& s, Expanded& out) {
    for (int i = 0; i < kTotalSlots; i++) {
        out.pos[(size_t)i * 3 + 0] = kCollapsed[0];
        out.pos[(size_t)i * 3 + 1] = kCollapsed[1];
        out.pos[(size_t)i * 3 + 2] = kCollapsed[2];
        // 类型按层给：0..61 描边、62..123 填充、图标恒为 1。不活动的槽位也照给 ——
        // 它们零面积不产生片元，但"描边整段排在填充前"这条不变量要保持结构上成立。
        out.type[i] = (i >= kTileSlots) ? 1.0f : ((i < kTileSlotsPerLayer) ? 0.0f : 1.0f);
    }
    const Recipe* rc = recipeTable();
    for (int layer = 0; layer < kLayers; layer++) {
        float type = (float)layer;   // 0 = 描边，1 = 填充（参考实现的 pushType 取值）
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            if (!partActive(s.mode, part)) continue;
            const PartRecord& r = s.rec[part * kLayers + layer];
            int base = partBase(layer, part);
            int n = partSlotCount(part);
            float cx = r.p[0], cy = r.p[1], r0 = r.p[2];
            for (int i = 0; i < n; i++) {
                float x = 0.0f, y = 0.0f;
                if (part == P_CIRCLE) {
                    if (i == 0) { x = cx; y = cy; }
                    else {
                        // createCircle：`cos(a)*radius + cx` → 单条 fmadd
                        const Recipe& g = rc[base + i];
                        x = std::fma(g.k0, r0, cx);
                        y = std::fma(g.k1, r0, cy);
                    }
                } else if (part == P_WEDGE) {
                    float w = r.p[3], s0 = r.p[4], c0 = r.p[5], s1 = r.p[6], c1 = r.p[7];
                    switch (i) {
                        case 0: x = std::fma(-r0, s1, cx); y = std::fma(r0, c1, cy); break;
                        case 1: x = cx; y = cy; break;
                        case 2: x = std::fma(r0, s0, cx); y = std::fma(-r0, c0, cy); break;
                        case 3: x = w * s0; y = -(w * c0); break;
                        case 4: x = 0.0f; y = 0.0f; break;
                        case 5: x = -(w * s1); y = w * c1; break;
                    }
                } else if (part == P_BIGQ) {
                    float w = r.p[2], s0 = r.p[3], c0 = r.p[4], s1 = r.p[5], c1 = r.p[6];
                    switch (i) {
                        case 0: x = cx; y = cy; break;
                        case 1: x = w * s0; y = -(w * c0); break;
                        case 2: x = 0.0f; y = 0.0f; break;
                        case 3: x = -(w * s1); y = w * c1; break;
                    }
                } else if (part == P_CAPS) {
                    float m11 = r.p[0], m12 = r.p[1], m21 = r.p[2], m22 = r.p[3], w = r.p[4], l = r.p[5];
                    switch (i) {
                        case 0: x = std::fma(l, m11, w * m12); y = std::fma(l, m12, -(w * m11)); break;
                        case 1: x = std::fma(l, m11, -(w * m12)); y = std::fma(l, m12, w * m11); break;
                        case 2: x = -(w * m12); y = w * m11; break;
                        case 3: x = w * m12; y = -(w * m11); break;
                        case 4: x = std::fma(l, m21, w * m22); y = std::fma(l, m22, -(w * m21)); break;
                        case 5: x = std::fma(l, m21, -(w * m22)); y = std::fma(l, m22, w * m21); break;
                        case 6: x = -(w * m22); y = w * m21; break;
                        case 7: x = w * m22; y = -(w * m21); break;
                    }
                } else if (part == P_EXT4) {
                    float mx = r.p[0], my = r.p[1], m1 = r.p[2], m2 = r.p[3], w = r.p[4], l = r.p[5];
                    // 参考实现（ang==0 分支）：`mx+length*m1±width*m2` / `my+length*m2∓width*m1`
                    // 左结合 = (mx + l*m1) ± w*m2，所以是"内层 fma 再把 w 那一项融合进去"。
                    switch (i) {
                        case 0: x = std::fma(w, m2, std::fma(l, m1, mx)); y = std::fma(-w, m1, std::fma(l, m2, my)); break;
                        case 1: x = std::fma(-w, m2, std::fma(l, m1, mx)); y = std::fma(w, m1, std::fma(l, m2, my)); break;
                        case 2: x = std::fma(-w, m2, mx); y = std::fma(w, m1, my); break;
                        case 3: x = std::fma(w, m2, mx); y = std::fma(-w, m1, my); break;
                    }
                } else {   // P_PENT
                    float mx = r.p[0], my = r.p[1], m1 = r.p[2], m2 = r.p[3], w = r.p[4], l = r.p[5];
                    // px[i] = l*m1 ± w*m2，py[i] = l*m2 ∓ w*m1；顶点 = (mx+px, my+py)
                    //
                    // 五边形的融合结构与 EXT4 **不同**（同样长相的算式，编译器在不同上下文里
                    // 选了不同的一步去融合）—— 这里是用 rp()/rmul 把每一步钉死，别"顺手统一"：
                    //   两项式：px = fma(l, m1, rp(w, m2))，再普通加 mx（实测候选 v6/v7 全同）
                    //   单项式：px = rp(w, m2)，再普通加 mx
                    // （rp = "单独舍入的乘积"，见文件头；顺手的 `w*m2` 会被编译器融合进外层加法 ✗）
                    switch (i) {
                        // 负系数一律写成"先乘积、再取负"（`-(rp(w,m))`）：与参考实现的 `fnmul`
                        // 一致，而且只有这个写法能保住 -0.0 的符号（`rp(-w,m)` 里那个 +0 加数会把
                        // 符号吃掉 —— 像素上看不出来，但逐位对拍会红，见 L1 的 slot4 y）。
                        case 0: x = std::fma(l, m1, rp(w, m2)) + mx;
                                y = std::fma(l, m2, -(rp(w, m1))) + my; break;
                        case 1: x = std::fma(l, m1, -(rp(w, m2))) + mx;
                                y = std::fma(l, m2, rp(w, m1)) + my; break;
                        case 2: x = -(rp(w, m2)) + mx; y = rp(w, m1) + my; break;
                        case 3: x = rp(w, m2) + mx;    y = -(rp(w, m1)) + my; break;
                        case 4: x = -(rp(w, m1)) + mx; y = -(rp(w, m2)) + my; break;
                        case 5: x = rp(w, m2) + mx;    y = -(rp(w, m1)) + my; break;
                        case 6: x = -(rp(w, m2)) + mx; y = rp(w, m1) + my; break;
                    }
                }
                out.pos[(size_t)(base + i) * 3 + 0] = x;
                out.pos[(size_t)(base + i) * 3 + 1] = y;
                out.pos[(size_t)(base + i) * 3 + 2] = 0.0f;
                out.type[base + i] = type;
            }
        }
    }
    // 图标：常量已在配方里（局部坐标），type 恒为 1（填充）；颜色由 part 决定
    for (int part = P_ICON_TWIRL; part <= P_ICON_SSDN; part++) {
        int base = iconBase(part);
        for (int i = 0; i < kIconSlots; i++) {
            out.pos[(size_t)(base + i) * 3 + 0] = rc[base + i].k0;
            out.pos[(size_t)(base + i) * 3 + 1] = rc[base + i].k1;
            out.pos[(size_t)(base + i) * 3 + 2] = 0.0f;
            out.type[base + i] = 1.0f;
        }
    }
}

int packShapeTable(const Shape* shapes, int count, std::vector<float>& out) {
    // 注意单位：`out` 是**float** 流，一个 RGBA32F texel 占 4 个 float —— 所以每个
    // texel 下标都要 ×4（这个坑踩过一次：按 texel 下标写进 float 流，等于整体挪了 4 倍）。
    size_t texels = (size_t)count * kTexelsPerShape;
    size_t rows = (texels + kTexW - 1) / kTexW;
    out.assign(rows * kTexW * 4, 0.0f);
    for (int s = 0; s < count; s++) {
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            for (int layer = 0; layer < kLayers; layer++) {
                const PartRecord& r = shapes[s].rec[part * kLayers + layer];
                size_t base = ((size_t)s * kTexelsPerShape +
                               (size_t)(layer * kTileParts + part) * 2) * 4;
                for (int i = 0; i < 4; i++) out[base + (size_t)i] = r.p[i];
                for (int i = 0; i < 4; i++) out[base + 4 + (size_t)i] = r.p[4 + i];
            }
        }
        // 标志 texel：活动位掩码 + mode（VS 用它决定展开还是塌陷；与 partActive() 同源）
        float mask = 0.0f;
        for (int part = P_CIRCLE; part <= P_PENT; part++)
            if (partActive(shapes[s].mode, part)) mask += (float)(1 << part);
        size_t fb = ((size_t)s * kTexelsPerShape + kFlagTexel) * 4;
        out[fb] = mask;
        out[fb + 1] = (float)shapes[s].mode;
    }
    return (int)rows;
}

} // namespace TileShape

}  // namespace adofai
