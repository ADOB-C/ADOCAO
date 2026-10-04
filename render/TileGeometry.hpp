#pragma once

#include <vector>
#include <cmath>

// Scratch buffer for geometry generation (owned by the caller — was a global
// `extern Scratch g_sc` before P5; local instances make future parallel mesh
// builds safe).
struct Scratch {
    std::vector<float> verts;      // local xyz
    std::vector<float> types;      // 0.0=stroke, 1.0=fill per vertex
    std::vector<unsigned> indices;

    void clear() { verts.clear(); types.clear(); indices.clear(); }
};

// Constants matching re_adojas
constexpr float TILE_WIDTH = 0.275f;
constexpr float TILE_LENGTH = 0.5f;
constexpr float OUTLINE = 0.025f;

// Helpers
inline float fmodWrap(float x, float y) { return x >= 0 ? std::fmod(x, y) : std::fmod(x, y) + y; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

void pushType(std::vector<float>& c, float type, int n);

// Geometry generators (local-space, origin-centered)
void createCircle(float cx, float cy, float radius, float type, Scratch& sc, int res = 32);
void createTileMesh(float startAngle, float endAngle, Scratch& sc);
// 中旋砖（angleData=999）：五边形——“方块身体 + 朝来路的尖角”，沿入砖方向 a1 摆放。
// 不是 createTileMesh(a,a)（那是圆+方块，像发卡弯）。
void createMidSpinMesh(float a1, Scratch& sc);
