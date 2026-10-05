#pragma once


namespace adofai {

namespace Shaders {

// Instanced tile rendering. Vertex type (0=stroke, 1=fill) mixes per-instance colors.
// Camera-relative offsets computed on CPU side.
constexpr const char* kTileVertSrc = R"(#version 410 core
// 砖块几何在 GPU 上展开：每个实例（= 每砖）带形状号，几何由这里按形状表算出来。
// 位精确性由 tests/tile_expansion_test.cpp（CPU）与 tests/geom_probe_test.cpp（GPU，读本文件本身）钉住。
//
// 输入三样：
//   * **静态配方表**（顶点属性 aRecipe）：每个 canonical 槽位一项 `(part, layer*64+slot, k0, k1)`。
//     175 个槽位 = 6 个砖 part × 2 层（描边/填充）+ 3 个图标 part × 17；索引表 142 个三角形，
//     所有形状共用一份 —— 所以每帧 draw 从"每形状一次"变成 1 次。
//   * **形状表**（sampler2D + texelFetch，RGBA32F）：每个 (形状, part, 层) 一条 8 float 记录，
//     一条记录占 2 个 texel。记录内容与 render/TileShape.cpp 的 `setRec` 一一对应。
//   * **每实例**：aInstOffset（相机相对偏移，z 里已含砖的 Z）、aShape（形状号）、aIconBits。
//
// 位精确（本文件的存在理由）：这里**没有**超越函数 —— cos/sin/pow/fmod 的结果都在形状表里
// 由 CPU 算好；每条坐标公式与 `TileShape.cpp::expand()` **逐字同结构**：
//   * 融合步用显式 `fma()`（镜像编译器的 fmadd/fmsub/fnmul）；
//   * 需要单独舍入的乘积用 `rp()`（= `fma(a,b,0)`，就是正确舍入的乘积）；
//   * 负系数一律"先乘积再取负"（`-(rp(w,m))`），只有这个写法保住 -0.0 的符号。
// 改这里必须同步改 `expand()`，然后跑 `tests/tile_expansion_test.cpp`（CPU 逐位）和
// `tests/geom_probe_test.cpp`（GPU 逐位，用的是本文件本身）。
//
// 塌陷：不活动的 part（记录 p[7] < 0.5）或没点亮的图标，整块槽位都返回同一个点 (0,0,0)
// → 三角形零面积 → 不产生片元。所以"层/part 的先后"只影响同色同深度的重叠，不影响像素。

layout(location = 0) in vec4  aRecipe;      // (part, layer*64+slot, k0, k1)
layout(location = 1) in vec3  aInstOffset;  // 每实例：相机相对偏移
layout(location = 2) in float aShape;       // 每实例：形状索引（< 2^24，float 精确）
layout(location = 3) in float aIconBits;    // 每实例：bit0 twirl / bit1 ssUp / bit2 ssDown

uniform sampler2D uShapeTex;
// 形状表的行宽 = TileShape::kTexW（编译期常量：省一个 uniform，也不给驱动留优化余地）
const int kTexW = 1024;
uniform mat4  uVP;
uniform vec3  uFillColor;
uniform vec3  uStrokeColor;
uniform float uOpacity;

out vec3  vColor;
out float vOpacity;

#ifdef GEOM_PROBE
// 逐位对拍模式：把 (局部坐标, type) 写到像素上。见 tests/geom_probe_test.cpp ——
// 那个测试读的就是这个文件本身（前置 `#define GEOM_PROBE 1`），所以测的是生产代码路径。
out vec4 vProbe;
uniform vec2 uProbeSize;   // (W, H)：像素画布尺寸
#endif

// 图标（与 TileMesh.cpp 的 IR/IS/三种颜色/两个 z 偏移一致）
const float kIconZBase  = 0.002;
const float kIconZExtra = 0.003;
const vec3  kTwirlColor = vec3(0.502, 0.0, 0.502);
const vec3  kSSUpColor  = vec3(1.0, 0.0, 0.0);
const vec3  kSSDownColor= vec3(0.0, 0.0, 1.0);

// 单独舍入的乘积：`fma(a,b,0)` 就是正确舍入的 a*b（与参考实现里单独一条 fmul 同值），
// 而裸的 `a*b` 会被编译器（合法地）融合进旁边那次加法，差 1 ULP。
float rp(float a, float b) { return fma(a, b, 0.0); }

// 活动位掩码的 6 个 bit（与 TileShape::partActive 的 P_CIRCLE..P_PENT 对应）
const float maskbits[6] = float[6](1.0, 2.0, 4.0, 8.0, 16.0, 32.0);

// 取 (形状, code) 那条记录的 8 个 float（code = layer*6 + part；一个形状 25 个 texel）
void fetchRec(float shape, float code, out vec4 A, out vec4 B) {
    int t = int(shape) * 25 + int(code) * 2;
    ivec2 c0 = ivec2(t % kTexW, t / kTexW);
    int t1 = t + 1;
    ivec2 c1 = ivec2(t1 % kTexW, t1 / kTexW);
    A = texelFetch(uShapeTex, c0, 0);
    B = texelFetch(uShapeTex, c1, 0);
}

void main() {
    float part  = floor(aRecipe.x + 0.5);
    float lspot = aRecipe.y;
    float slot  = mod(lspot, 64.0);
    float layer = floor(lspot / 64.0);
    float k0 = aRecipe.z, k1 = aRecipe.w;
    int   s   = int(slot + 0.5);

    vec3  pos   = vec3(0.0);                  // 塌陷点
    float type  = layer;                      // 0 = 描边，1 = 填充
    vec3  color = mix(uStrokeColor, uFillColor, layer);

    if (part > 5.5) {
        // ---- 图标（颜色/ Z 只由 part + 是否同时有 twirl 决定）----
        bool tw = mod(aIconBits, 2.0) >= 1.0;
        bool up = mod(floor(aIconBits / 2.0), 2.0) >= 1.0;
        bool dn = floor(aIconBits / 4.0) >= 1.0;
        bool on = (part < 6.5) ? tw : ((part < 7.5) ? up : dn);
        if (on) {
            // 与 TileMesh::buildIcons 同式：twirl 用 kIconZBase，SetSpeed 用
            // kIconZBase + (有 twirl ? kIconZExtra : kIconZBase*0.5)；实例偏移的 z 里是砖的 Z，
            // 所以最终 z = 砖 Z + 这里的 zo（两个变量的相加，逐位等于参考的 tz + zo）。
            float zo = (part < 6.5) ? kIconZBase
                                    : (kIconZBase + (tw ? kIconZExtra : kIconZBase * 0.5));
            pos = vec3(k0, k1, zo);
            type = 1.0;
            color = (part < 6.5) ? kTwirlColor : ((part < 7.5) ? kSSUpColor : kSSDownColor);
        }
    } else {
        vec4 A, B;
        fetchRec(aShape, layer * 6.0 + part, A, B);
        // 活动位掩码（形状表每个形状的最后一个 texel；bit i = part i 活动）。与 CPU 的
        // TileShape::partActive() 同源 —— 这里写成查表而不是 mode 的 switch，是为了不引入分支。
        int t = int(aShape) * 25 + 24;
        float mask = texelFetch(uShapeTex, ivec2(t % kTexW, t / kTexW), 0).x;
        float bit = maskbits[int(part + 0.5)];
        if (mod(floor(mask / bit), 2.0) >= 0.5) {
            if (part < 0.5) {
                // CIRCLE：槽 0 是圆心，其余是 createCircle 的单位圆常量（单条 fmadd）
                if (s == 0) { pos = vec3(A.x, A.y, 0.0); }
                else        { pos = vec3(fma(k0, A.z, A.x), fma(k1, A.z, A.y), 0.0); }
            } else if (part < 1.5) {
                // WEDGE：圆弧与两个端帽之间的填充块
                float cx = A.x, cy = A.y, r = A.z, w = A.w;
                float s0 = B.x, c0 = B.y, s1 = B.z, c1 = B.w;
                if      (s == 0) pos = vec3(fma(-r, s1, cx), fma(r, c1, cy), 0.0);
                else if (s == 1) pos = vec3(cx, cy, 0.0);
                else if (s == 2) pos = vec3(fma(r, s0, cx), fma(-r, c0, cy), 0.0);
                else if (s == 3) pos = vec3(w * s0, -(w * c0), 0.0);
                else if (s == 4) pos = vec3(0.0, 0.0, 0.0);
                else             pos = vec3(-(w * s1), w * c1, 0.0);
            } else if (part < 2.5) {
                // BIGQ：大角度的梯形身体
                float cx = A.x, cy = A.y, w = A.z;
                float s0 = A.w, c0 = B.x, s1 = B.y, c1 = B.z;
                if      (s == 0) pos = vec3(cx, cy, 0.0);
                else if (s == 1) pos = vec3(w * s0, -(w * c0), 0.0);
                else if (s == 2) pos = vec3(0.0, 0.0, 0.0);
                else             pos = vec3(-(w * s1), w * c1, 0.0);
            } else if (part < 3.5) {
                // CAPS：两个端帽矩形（沿 m11/m12 与 m21/m22）
                float m11 = A.x, m12 = A.y, m21 = A.z, m22 = A.w;
                float w = B.x, l = B.y;
                if      (s == 0) pos = vec3(fma(l, m11, w * m12), fma(l, m12, -(w * m11)), 0.0);
                else if (s == 1) pos = vec3(fma(l, m11, -(w * m12)), fma(l, m12, w * m11), 0.0);
                else if (s == 2) pos = vec3(-(w * m12), w * m11, 0.0);
                else if (s == 3) pos = vec3(w * m12, -(w * m11), 0.0);
                else if (s == 4) pos = vec3(fma(l, m21, w * m22), fma(l, m22, -(w * m21)), 0.0);
                else if (s == 5) pos = vec3(fma(l, m21, -(w * m22)), fma(l, m22, w * m21), 0.0);
                else if (s == 6) pos = vec3(-(w * m22), w * m21, 0.0);
                else             pos = vec3(w * m22, -(w * m21), 0.0);
            } else if (part < 4.5) {
                // EXT4：U 型（ang==0）的方块。`mx+length*m1±width*m2` 左结合 = (mx + l*m1) ± w*m2
                float mx = A.x, my = A.y, m1 = A.z, m2 = A.w, w = B.x, l = B.y;
                if      (s == 0) pos = vec3(fma(w, m2, fma(l, m1, mx)), fma(-w, m1, fma(l, m2, my)), 0.0);
                else if (s == 1) pos = vec3(fma(-w, m2, fma(l, m1, mx)), fma(w, m1, fma(l, m2, my)), 0.0);
                else if (s == 2) pos = vec3(fma(-w, m2, mx), fma(w, m1, my), 0.0);
                else             pos = vec3(fma(w, m2, mx), fma(-w, m1, my), 0.0);
            } else {
                // PENT：中旋五边形。结构与 EXT4 **不同**（同长相的算式，编译器在不同上下文里
                // 选了不同的一步融合）—— 两项式 = fma(l,m1, rp(w,m2)) + mx，单项式 = rp(w,m) + mx。
                float mx = A.x, my = A.y, m1 = A.z, m2 = A.w, w = B.x, l = B.y;
                if      (s == 0) pos = vec3(fma(l, m1, rp(w, m2)) + mx, fma(l, m2, -(rp(w, m1))) + my, 0.0);
                else if (s == 1) pos = vec3(fma(l, m1, -(rp(w, m2))) + mx, fma(l, m2, rp(w, m1)) + my, 0.0);
                else if (s == 2) pos = vec3(-(rp(w, m2)) + mx, rp(w, m1) + my, 0.0);
                else if (s == 3) pos = vec3(rp(w, m2) + mx, -(rp(w, m1)) + my, 0.0);
                else if (s == 4) pos = vec3(-(rp(w, m1)) + mx, -(rp(w, m2)) + my, 0.0);
                else if (s == 5) pos = vec3(rp(w, m2) + mx, -(rp(w, m1)) + my, 0.0);
                else             pos = vec3(-(rp(w, m2)) + mx, rp(w, m1) + my, 0.0);
            }
        }
    }

    vColor = color;
    vOpacity = uOpacity;

#ifdef GEOM_PROBE
    // 一个"顶点"画在像素 (gl_VertexID, gl_InstanceID) 上：顶点号 = canonical 槽号，
    // 实例号 = 形状号。点图元没有插值，所以 varying 就是原值。
    vProbe = vec4(pos, type);
    gl_Position = vec4(((float(gl_VertexID) + 0.5) / uProbeSize.x) * 2.0 - 1.0,
                       ((float(gl_InstanceID) + 0.5) / uProbeSize.y) * 2.0 - 1.0,
                       0.0, 1.0);
#else
    gl_Position = uVP * vec4(pos + aInstOffset, 1.0);
#endif
}
)";

constexpr const char* kTileFragSrc = R"(#version 410 core
in vec3 vColor;
in float vOpacity;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, vOpacity);
}
)";

// Planet rendering: per-vertex position, uniform color and MVP
constexpr const char* kPlanetVertSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

constexpr const char* kPlanetFragSrc = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = uColor;
}
)";

// Trail rendering: per-vertex position, uniform color with alpha
constexpr const char* kTrailVertSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

constexpr const char* kTrailFragSrc = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = uColor;
}
)";

// Highlight shader：与砖**共用** kTileVertSrc（几何展开只有一份实现），只有 FS 不同
constexpr const char* kHighlightVertSrc = kTileVertSrc;

constexpr const char* kHighlightFragSrc = R"(#version 410 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0 - vColor, 1.0);  // inverted track color
}
)";


} // namespace Shaders

}  // namespace adofai
