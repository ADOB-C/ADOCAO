#version 330 core
// 砖块几何的 **GL 3.3 回退版**（与 `tile.vert` 的顶点输出逐位相同）。
//
// 为什么需要它：`tile.vert` 用 `fma()`（GLSL 4.00+）逐字镜像 CPU 参考实现的融合结构。
// 拿不到 4.x 上下文（真 3.3 core）时唯一的办法是**别在 GPU 上算**：`TileShape::expand()`
// 已经在 CPU 上算出每个 canonical 槽位的局部坐标（那份实现正是 `tile.vert` 的对拍基准，
// 见 `tests/tile_expansion_test.cpp` / `tests/geom_probe_test.cpp`），这里用 RG32F 纹理把
// 结果查出来 —— 于是本文件**一次乘加都没有**，几何值就是 CPU 参考值本身。
//
// 与 `tile.vert` 的差别**只有**"砖的局部坐标从哪来"：输入（同一个 VAO、同一份实例流）、
// 图标算式、索引表、绘制序、剔除/LOD 全部共用 —— 所以两个程序的出图必须逐字节相同
// （验收：`ADOCAO_CPU_GEOMETRY=1 scripts/capture-gate.sh check` 51/51 逐字节）。
//
// 输入（与 tile.vert 一致）：
//   * aRecipe：静态配方表，每个 canonical 槽位一项 (part, layer*64+slot, k0, k1)；
//   * aInstOffset / aShape / aIconBits：每实例（相机相对偏移 / 形状号 / 图标位）；
//   * uSlotTex：每形状 `kSlots`(142) 个 texel 的 CPU 展开表 —— texel i = 槽位 i 的 (x, y)，
//     行宽 kTexW（= `TileShape::kTexW`，写成常量省一个 uniform）。槽位号 = gl_VertexID
//     （索引表里的值就是 canonical 槽位号，aRecipe 也按它排列）。

layout(location = 0) in vec4  aRecipe;      // (part, layer*64+slot, k0, k1)
layout(location = 1) in vec3  aInstOffset;  // 每实例：相机相对偏移（z 已含砖的 Z）
layout(location = 2) in float aShape;       // 每实例：形状索引
layout(location = 3) in float aIconBits;    // 每实例：bit0 twirl / bit1 ssUp / bit2 ssDown

uniform sampler2D uSlotTex;
const int kTexW = 1024;         // TileShape::kTexW
const int kSlots = 142;         // TileShape::kTotalSlots（7 part × 2 层；图标已改在片元里画）
const int kTileSlots = 142;     // 描边 71 + 填充 71

uniform mat4  uVP;
uniform vec3  uFillColor;
uniform vec3  uStrokeColor;
uniform float uOpacity;
// 逐实例砖色：uUseInstColor=1 时用实例色（ColorTrack），0 时用上面的 uniform（高亮/旧行为）。
layout(location = 4) in vec3 aFillColor;
layout(location = 5) in vec3 aStrokeColor;
// MoveTrack：rot(弧度) / scaleX / scaleY / alpha | moveXY（所有砖默认 identity）
layout(location = 6) in vec4 aXform;
layout(location = 7) in vec2 aMove;
layout(location = 8) in vec2 aPivot;   // 砖心相对砖起点（旋转/缩放枢轴）
uniform float uUseInstColor;

out vec3  vColor;
out float vOpacity;
out vec2  vIconLocal;   // 砖心局部坐标（图标在 FS 里画）
out float vIconBits;

// 图标（与 TileMesh.cpp 的 IR/IS/三种颜色一致；z 与砖共面，谁盖谁由索引表顺序决定）
const vec3  kTwirlColor = vec3(0.502, 0.0, 0.502);
const vec3  kSSUpColor  = vec3(1.0, 0.0, 0.0);
const vec3  kSSDownColor= vec3(0.0, 0.0, 1.0);

// 槽位 slot 的局部坐标：查 CPU 展开表（表里不活动的 part 已经被塌成 (0,0)，三角形零面积）
vec2 slotPos(float shape, int slot) {
    int t = int(shape) * kSlots + slot;
    return texelFetch(uSlotTex, ivec2(t % kTexW, t / kTexW), 0).xy;
}

void main() {
    float part = floor(aRecipe.x + 0.5);
    float k0 = aRecipe.z, k1 = aRecipe.w;

    // 槽位号 = gl_VertexID；类型只看槽位：0..61 描边、62..123 填充、124.. 图标恒为填充
    // （与 TileShape::expand() 写出的 out.type 一致）
    int   v     = int(gl_VertexID);
    vec3  pos   = vec3(0.0);                  // 塌陷点
    float type  = (v < 71) ? 0.0 : 1.0;
    vec3  color = mix(mix(uStrokeColor, uFillColor, type),
                      mix(aStrokeColor, aFillColor, type), uUseInstColor);

    {
        // ---- 砖：CPU 展开表 ----
        pos = vec3(slotPos(aShape, v), 0.0);
    }

    vColor = color;
    vIconLocal = pos.xy;   // 砖的局部原点就是砖心（形状以砖心为原点）
    vIconBits  = aIconBits;
    // stroke 层（z 更小=更近）压过 fill；同层同 z 的 part 由 GL_LESS 丢弃后画的那个。
    pos.z -= (1.0 - type) * 2e-4;
    // 图标与 fill **共面**（z=0）：不靠 z 偏移，靠第二趟 draw + GL_LEQUAL 压过 fill。
    // 固定 z 偏移在"几百万砖同屏"时一定会穿透（可见窗口的 z 间距只有 1e-7 量级）。
    if (aXform.x == 0.0 && aXform.y == 1.0 && aXform.z == 1.0) {
        pos = vec3(pos.xy + aMove, pos.z);
    } else {
        vec2 q = pos.xy - aPivot;
        q *= vec2(aXform.y, aXform.z);
        float xc = cos(aXform.x), xs = sin(aXform.x);
        q = vec2(q.x * xc - q.y * xs, q.x * xs + q.y * xc);
        pos = vec3(q + aPivot + aMove, pos.z);
    }
    vOpacity = uOpacity * aXform.w;
    gl_Position = uVP * vec4(pos + aInstOffset, 1.0);
}
