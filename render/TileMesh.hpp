#pragma once

#include "BitSpan.hpp"
#include <cmath>

#include "glad/gl_core.hpp"
#include "core/level/LevelData.hpp"
#include "render/CullIndex.hpp"
#include "render/TileShape.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// 轨道渲染：**1 实例 = 1 砖**，几何在 VS 里按"形状表"展开（见 render/TileShape.hpp 与
// assets/shaders/tile.vert）。2026-10 之前是"每个形状一份 CPU 顶点汤 + 每形状一次 draw"。
//
// 一个实例画完整块砖 + 它的事件图标：canonical 顶点表 193 槽（砖 142 + 图标 51）、
// 158 个三角形（474 索引）共用一份索引表，所以整条轨道**一次 draw** 画完（形状数不再决定 draw 次数：
// 之前 MYC 294 次、Singularity 8192 次、ADR 型全角度谱面会到十万次量级）。
//
// 常驻（每砖）≈ 14 B：形状号 4 + 世界坐标 8 + 图标位 1 + 可见位 1（TrackVis 才分配）；
// 之前是"砖 24 B + 每实例 pos/color/tileIdx/可见 ≈ 57 B（含图标实例 616 万）"。
// 每帧只上传**可见**实例（20 B/实例：相机相对偏移 3 + 形状号 1 + 图标位 1）。
//
// 绘制顺序：**按层号升序**（2026-10-15 改；原来是"按形状分组 + 组内降序"，那是实例化之前
// "同形状一起画省状态切换"的遗留）。旧注释记过"把组内顺序反过来，MYC t=30s 那帧有 102 个
// 像素不同" —— 那发生在**旧的全局 Z 归一化**下：相邻砖的深度被量化到同一个值，重叠处谁赢
// 由先后决定。改成**按可见窗口归一化 Z** 之后（见 `tileZForIndex` 的说明）窗口内步长
// ≈0.018 ≫ 深度分辨率 1.19e-5，并列基本消失。实测：像素门槛 51/51 逐字节不变，另在
// 三种 Won't You + TNR + primer-final 上做了 16 帧 A/B（含"整张一帧"6,841 万块）
// **全部逐字节相同**（要重做这个 A/B 就临时改提交顺序；旧钩子已随数组删除）。
// 残留的理论情形：极端广角 + 屏幕上真的重合但颜色/图标不同的砖（那 16 帧没造出来）；
// 但那时胜负也已是**确定**的（低层号在上），不再是 `unordered_map` 的迭代序。
//
// ⇒ 推论（④ 的设计前提）：**绘制顺序可以随便排**，所以剔除改成"先按块筛"之后，
// 块按层号升序、块内按砖号升序输出即可，不需要为"恢复旧绘制序"付排序代价。

namespace adofai {
class Timeline;

class FloorWindowCache;

class TileMesh {
public:
    TileMesh() = default;
    ~TileMesh();
    TileMesh(const TileMesh&) = delete;
    TileMesh& operator=(const TileMesh&) = delete;

    // `timeline` 只用于把 trackStyle 解析到"该砖被踩到的那一刻"（精确判据）。可空：
    // 为空时退回按层号的估计（测试/无 Timeline 路径），正式渲染必须传。
    void build(const LevelData& level, const std::string& fillColorHex = "FFFFFF",
               const std::string& strokeColorHex = "000000",
               const Timeline* timeline = nullptr);
    // **一次 draw** 画完砖与图标（图标 part 紧跟在自己那块砖的填充之后）—— 指数的索引表
    // 顺序就是"描边 → 填充 → 三个图标 part"。
    //
    // 顺序上的一个**故意修复**：改造前是"砖 → 拖尾 → 行星 → 图标"，图标画在拖尾之后。图标是
    // 不透明的、拖尾是半透明混合，于是图标会把拖尾"擦"掉一块 —— 那是 bug（用户 2026-10 指出）。
    // 现在图标跟着砖走，在拖尾之前，拖尾正常盖在图标上。这个修复会让"图标与拖尾重叠"的像素
    // 与旧版不同（The Moon t=1s / MYC t=30s 等），是**预期内**的差异，基线已按新行为重存。
    // 视锥边界是**世界坐标** → double（float 在 x≈6.8e7 上会被量化到 8 砖，剔除边界跟着错）
    // 逐实例砖色（fill/stroke，0..255）。fn = nullptr 时实例色全 0，由 uniform 决定颜色
    // （高亮/旧行为）。回调只对**可见列表**里的砖调用（每帧几百次），ctx 由调用方保证生命周期。
    // 逐实例颜色：fill/stroke 为 RGB，alpha = 颜色自带的不透明度（8 位 hex 的 AA/255）。
    using InstanceColorFn = void (*)(void* ctx, uint32_t tile, uint8_t fill[3], uint8_t stroke[3],
                                     float* alpha);
    // 逐实例变换（MoveTrack）：out = rot(弧度), scaleX, scaleY, alpha, moveX, moveY, pivotX, pivotY。
    // pivot = 砖心相对砖起点的偏移（Re_ADOJAS 的几何以砖心为原点：旋转/缩放绕砖心）。
    // fn=nullptr → 全部 identity（rot 0 / scale 1 / alpha 1 / move 0 / pivot 0）。
    using InstanceXformFn = void (*)(void* ctx, uint32_t tile, float out[8]);
    void draw(double viewL, double viewR, double viewB, double viewT, double camX, double camY,
              InstanceColorFn colorFn = nullptr, void* colorCtx = nullptr,
              InstanceXformFn xformFn = nullptr, void* xformCtx = nullptr) const;
    // MoveTrack 的剔除外扩（最大位移 + 缩放已由 LevelData 折进 pad）。
    void setMovePad(float pad) { m_movePad = pad; }
    // 关卡时间：颜色/变换是时间驱动的（ColorTrack/RecolorTrack/MoveTrack），
    // 相机静止时绘制列表不重建，必须在这里告诉 TileMesh"时间变了"。
    void setTime(double t) { m_time = t; }
    // 关卡里有没有"随时间变"的逐砖视觉（ColorTrack/RecolorTrack/MoveTrack）。
    // 没有的话时间变化不用重算颜色/变换（省掉整帧的逐砖开销）。
    void setAnimNeeded(bool v) { m_animNeeded = v; }
    // 视口像素高度（每帧由 app 侧设入；LOD 用它算"每世界单位多少像素"）
    void setViewportHeightPx(int h) const { m_viewportH = h; }
    // 抓帧（确定性）模式：LOD 步长只能由"当前视口 + 两段式"决定，**不许用上一帧的反馈种子**
    // （否则同一帧在不同运行/不同历史下步长可能不同 → 出图不再逐字节可复现 ✗）。
    void setDeterministicCull(bool b) const { m_detCull = b; }
    // **GL 3.3 回退**（见 assets/shaders/tile_cpu.vert）：几何在 CPU 上按 `TileShape::expand()`
    // 展开成"槽位坐标表"，VS 只查表 —— 于是不需要 GLSL 4.00 的 `fma()`。与 GPU 路共用
    // 实例流/索引表/剔除/LOD/绘制序，**只有坐标来源不同**，出图必须逐字节相同
    // （验收：`ADOCAO_CPU_GEOMETRY=1 scripts/capture-gate.sh check` 51/51）。
    // 必须在 build() 之前设（buildStaticGL 里据此决定建哪张纹理）。
    void setCpuGeometry(bool b) { m_cpuGeometry = b; }
    void drawHighlightedTile(int tileIdx, double camX, double camY) const;

    void setVisibleThreshold(int lastVisible);
    // ④ 剔除粗块粒度（层/块；0 = 关闭索引走全扫）。必须在 build() 之前设。
    // 默认 4096；`ADOCAO_CULL_BLOCK=<n>` 环境变量可覆盖（测试/A-B 钩子）。
    void setCullBlockFloors(int floors) { m_cullFloors = floors; }

    // ③ 切片 4：**某层区间的每砖派生数据**（相对本块原点的位置、形状号、图标位）。
    // 口径与 `build()` 里原来那几个循环**逐字一致**，`build()` 现在也调用它 —— 于是"按块派生"
    // 与"整段派生"的一致性由构造保证。将来渲染侧按可见块物化时复用它（省 13 B/砖）。
    struct DerivedRange {
        int64_t first = 0;
        std::vector<float> posX, posY;
        std::vector<uint32_t> shape;
        std::vector<uint8_t> icon;
    };
    // 形状键打包：sa/ea 各 20 位（角度 ×100，范围 ±18000），mid 1 位 —— 无冲突
    static uint64_t packShapeKey(float sa, float ea, bool mid, uint8_t style = 0) {
        const uint32_t qa = (uint32_t)(int32_t)std::lround(sa * 100.0f) & 0xFFFFFu;
        const uint32_t qe = (uint32_t)(int32_t)std::lround(ea * 100.0f) & 0xFFFFFu;
        return (uint64_t)qa | ((uint64_t)qe << 20) | (mid ? (1ull << 40) : 0ull) |
               ((uint64_t)(style & 0x7u) << 41);
    }
    // `wc`（可选）= 调用线程自己的 provider 窗口克隆（并行剔除用；nullptr = 共享缓存）
    void deriveRange(const LevelData& level, int64_t i0, int64_t i1, DerivedRange& out,
                     FloorWindowCache* wc = nullptr) const;
    void updateVisibleRange(int startTile, int endTile, bool visible);
    bool empty() const;

    static constexpr float kMaxTileZ = 9.0f;
    static float tileZForIndex(int i, int n);
    static void hexToColor3(const std::string& hex, float out[3]);

    // 验收/诊断用：上次 draw 实际画了多少个实例、形状数
    int lastDrawnInstances() const { return m_lastDrawn; }

private:
    void destroy();
    void buildStaticGL();
    void buildSlotTable();          // CPU 回退：每形状 193 个槽位坐标 → RG32F 纹理
    void destroyStaticGL();
    void ensureDrawList(double vl, double vr, double vb, double vt, double camX, double camY) const;
    void uploadIfNeeded() const;
    // 按 m_time 重算 m_listColors / m_listXform（列表本身不变）。
    void fillInstanceAnim() const;
    void cullRange(size_t begin, size_t end, double vl, double vr, double vb, double vt,
                   double camX, double camY, std::vector<uint32_t>& outTiles,
                   std::vector<float>& outAttr, size_t stride = 1) const;

    // ---- 静态 GL（所有实例共用）----
    GLuint m_vao = 0, m_recipeVbo = 0, m_ebo = 0, m_instVbo = 0, m_shapeTex = 0;
    // CPU 回退路用的槽位坐标表（RG32F；两支持平，同一纹理单元 0）
    GLuint m_slotTex = 0;
    int m_slotRows = 0;
    bool m_cpuGeometry = false;
    mutable size_t m_instCapacity = 0;   // 实例缓冲容量（draw() 里会按需增长）
    int m_shapeRows = 0;
    float m_fill[3] = {1, 1, 1}, m_stroke[3] = {0, 0, 0};

    // ---- 每砖常驻 ----
    std::vector<TileShape::Shape> m_shapes;   // 形状（去重；含 localBBox）
    std::unordered_map<uint64_t, uint32_t> m_shapeIndex;   // ③ 切片 4：形状键 → 形状号（派生用）
    // ③ 切片 4【二分实验】：数组**保留**，额外建"按块窗口"，只让 cullRange 从窗口读 ——
    // 用来判定"派生值不对"还是"读路径不等价"。
    const LevelData* m_level = nullptr;
    const Timeline*  m_timeline = nullptr;   // 只在解析 trackStyle 时用（见 build）
    // ⑦ 容量边界（2026-10-16）：极端视距下"整张一帧"会让主线程卡死几十秒
    // （实测：1055 万砖全进视锥 → 逐块剔除 + 211 MB 实例上传 = **一帧 36.2 s**，macOS 转沙滩球）。
    // 两道闸：① 候选砖超过 kMaxCullCandidates 就**跳过本帧重建**（保留上一帧的列表）；
    // ② 可见实例超过 kMaxInstances 就**确定性截断**（按砖号取前 N）。宁可少画，不能卡死。
    // 门槛里最宽的状态只抽 5,407 个实例，所以这两道闸对 51 个状态**完全不可见**。
    // ⚠️ 这两个值必须**高于门槛里任何合法状态**（实测：门槛中最宽的那个状态抽 >6.5 万实例，
    // 把它压到 1<<16 会让门槛 50/51 ✗）。当前值下 51 个状态逐字节相同，而降级帧 36 s → 82 ms。
    static constexpr size_t kMaxCullCandidates = 1u << 19;   // 524,288 块砖
    static constexpr size_t kMaxInstances      = 1u << 18;   // 262,144 实例（×20 B = 5.2 MB）
    // 剔除用的派生窗口粒度**与剔除索引的细块对齐**（512 层）：以前是 4096（= 粗块），
    // 于是每个 512 层的细块命中都要白推 4096 层（实测一帧 321 次推导 × ~164 µs ≈ 53 ms ✗）。
    // provider 侧的窗口仍是 4096（checkpoint 粒度），但它的 1 块缓存会命中，摊薄到 1/8。
    static constexpr size_t kWindowFloors = 512;
    // = 上亿次比较），改成哈希后 O(1)。
    const DerivedRange* windowFor(size_t tile) const;
    void limitWarn(const char* what, size_t n) const;
    static bool limitOffCache();
    mutable double m_limitWarnAt = 0.0;

    // ③ 切片 4：四支每砖数组**不再常驻**（13 B/砖，MYC 84 MB）—— 剔除时按 4096 窗口物化。
    // 砖位置：**块相对 float**。绝对世界坐标是 double，直接窄化成 float 会在超大谱上塌掉 ——
    // primer-final（68,411,077 砖、直线轨道）的 x 到 6.84e7，float ulp 正好 **8 砖**，
    // 于是相邻 8 块砖被舍入到同一个坐标、屏幕上"轨道不渲染"（2026-10-08 用户实测）。
    // 现在每 kPosChunkShift 砖记一个 double 原点，块内偏移用 float：块内跨度 ≤65536 砖时
    // ulp ≤ 0.008 砖，内存代价是每块 16 B（65536 砖 → 0.00024 B/砖）。
    static constexpr size_t kPosChunkShift = 16;   // 65536 砖/块

    std::vector<double> m_chunkX, m_chunkY;        // 每块的 double 原点（世界坐标）

    // ④ 粗块 AABB 索引（每 4096 层一块，0.0098 B/砖）。n 小于阈值时留空 → 走原来的全扫，
    // 小谱行为与改造前**逐字相同**。查询按块升序 → 输出与全扫逐元素相同（`cull_index` 用例）。
    CullIndex m_cullIndex;
    int m_cullFloors = (int)CullIndex::kBlockFloors;   // 见 setCullBlockFloors                  // 建表用的保守半边长（= 所有形状 localBBox 的最大外扩）
    // （图标位随窗口派生：bit0 twirl / bit1 ssUp / bit2 ssDown）
    // TrackVis 的逐砖可见位：**位图**（原 1 B/砖 = MYC 6.8 MB → 0.85 MB）。lazy：只有 TrackVis 需要。
    std::vector<uint8_t> m_visible;
    bool visAt(size_t t) const { return bitAt(m_visible.data(), t); }
    void visSetRange(size_t a, size_t b, bool v);          // 闭区间 [a, b]
    bool visAllocated() const { return m_visible.size() == (size_t)(((size_t)m_nTiles + 7) / 8); }
    int m_nTiles = 0;

    // ---- 每帧可见实例（compaction 结果；draw 时上传）----
    mutable std::vector<uint32_t> m_listTiles;
    mutable std::vector<float> m_listAttr;    // 5 float/实例：offX offY offZ shape iconBits
    mutable std::vector<float> m_listColors;  // 6 float/实例：fill rgb + stroke rgb（0..1）
    mutable std::vector<float> m_listXform;   // 8 float/实例：rot, sX, sY, alpha, moveX, moveY, pivotX, pivotY
    mutable InstanceXformFn m_xformFn = nullptr;
    mutable void* m_xformCtx = nullptr;
    float m_movePad = 0.0f;      // 剔除外扩（MoveTrack）
    mutable std::vector<float> m_instData;    // 上传用交错缓冲：11 float/实例（5 + 6）
    mutable InstanceColorFn m_colorFn = nullptr;
    mutable void* m_colorCtx = nullptr;
    mutable bool m_listValid = false;
    mutable int m_viewportH = 720;
    mutable size_t m_lodStride = 1;      // 当前帧的抽稀步长（1 = 不抽）
    mutable size_t m_lodSeed = 1;        // 上一帧收敛出的步长（live 反馈用；抓帧不看它）
    mutable bool m_detCull = false;
    mutable bool m_listUploaded = false;   // 实例缓冲里是不是这一帧的列表
    mutable double m_vl = 0, m_vr = 0, m_vb = 0, m_vt = 0;
    mutable int m_visibleThreshold = 0x7fffffff;
    mutable int m_lastDrawn = 0;
    bool m_animNeeded = true;
    double m_time = 0.0;         // 当前关卡时间（由 draw 调用方设置）
    mutable double m_animTime = 0.0;   // 逐实例颜色/变换是按哪个时间算的
    mutable double m_prevCamX = 0, m_prevCamY = 0;
};

}  // namespace adofai
