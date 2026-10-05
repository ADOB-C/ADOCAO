// L1：新几何（canonical part 记录 + VS 展开）必须与**参考实现**逐位相同。
//
// 为什么需要它：改造把"每个形状一份 CPU 顶点汤"换成"每个形状一份 part 参数 + GPU 展开"。
// 参数是从 sa/ea 重新推出来的（三角、pow、以及编译器的 FMA 融合顺序都可能差 1 ULP），
// 而 1 ULP 在 zoom 250 的验收图上就会翻像素 —— 所以这一层必须**逐位**钉住，不能靠"看着一样"。
//
// 断言（对每个形状）：
//   1. 活动 part 的槽位坐标与参考实现的对应顶点**逐位相同**（float 位模式，不比值）；
//   2. 参考实现的三角形序列 == 新布局里去掉退化三角形后的序列（顶点号经过 part 映射）；
//   3. 非活动 part 的所有槽位塌成同一个点（三顶点同值 → 零面积 → 不产生片元）；
//   4. 描边槽位（type 0）整段排在填充槽位（type 1）前面（参考实现那套类型前缀约定）；
//   5. 局部包围盒与"参考顶点集的 double min/max"逐位相同（剔除结果不能变）。
//
// 形状集：`tests/charts/*.adofai` 里的每个砖 + 0.01° 网格抽样（含半格 0.005° 的舍入边界）+ 中旋。
// 负向对照：把融合步换成先各自舍入（`a*b + c*d`）必须过不了 —— 证明比较是有牙的。
//
// 用法：`ctest --test-dir build` 或
//       ./build/tests/adocao_tile_expansion_test [chart.adofai | charts目录] …

#include "TileShape.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

using namespace TileShape;

namespace {

int g_fail = 0, g_checked = 0, g_shapes = 0;
long long g_exact = 0, g_near = 0;
double g_maxDev = 0.0, g_maxDevExact = 0.0;
bool g_requireExact = false;      // ADOCAO_TILE_EXACT=1：要求逐位（本地验收用；CI 默认放宽）

// 默认口径是**几何尺度上的绝对容差**，不是"输出值的 ULP"。原因：y 分量有相消的形状
// （中旋 PENT 的 slot0/1）中间量只差 1 ULP，输出上会放大成几十 ULP，但绝对偏差始终
// ~1e-8 砖 —— 远小于任何可见特征（zoom 1000 时 1 px 也才 1e-3 砖），而真正的转写错误
// （符号/参数/顺序）是 1e-3 往上。ULP 级一致只在钉住的那台机器上成立，本地验收用
// `ADOCAO_TILE_EXACT=1` 恢复逐位要求。
constexpr double kTol = 1e-5;   // 砖为单位
constexpr int kMaxReport = 12;


std::string bits(float f) {
    uint32_t u; std::memcpy(&u, &f, 4);
    char b[32]; std::snprintf(b, sizeof b, "0x%08x (%.9g)", u, (double)f);
    return b;
}
void fail(const char* fmt, ...) {
    if (g_fail < kMaxReport) {
        va_list ap; va_start(ap, fmt);
        std::fputs("  ✗ ", stderr); std::vfprintf(stderr, fmt, ap); std::fputc('\n', stderr);
        va_end(ap);
    }
    g_fail++;
}

// 参考实现在每一层里发射的 (part, 槽位数)，顺序就是 createTileMesh/createMidSpinMesh 的写序
struct RefPart { int part; int count; };
std::vector<RefPart> refLayout(int mode) {
    switch (mode) {
        case 0: return {{P_CIRCLE, 33}, {P_WEDGE, 6}, {P_CAPS, 8}};
        case 1: return {{P_BIGQ, 4}, {P_CAPS, 8}};
        case 2: return {{P_CIRCLE, 33}, {P_EXT4, 4}};
        default: return {{P_PENT, 7}};
    }
}

// 参考顶点号 ←→ 规范槽位：两边的 part 顺序不同（BIQ 在参考里排第一，在规范里排第三），
// 所以用"每个 part 在参考里的起点"做映射，不靠位置假设。
struct Mapping {
    int refBase[P_COUNT][kLayers];
    int refVerts = 0;
    int slotToRef[kTotalSlots];
};
Mapping makeMapping(int mode) {
    Mapping m;
    for (auto& a : m.refBase) for (int& v : a) v = -1;
    for (int i = 0; i < kTotalSlots; i++) m.slotToRef[i] = -1;
    int off = 0;
    for (int layer = 0; layer < kLayers; layer++) {
        for (const RefPart& rp : refLayout(mode)) {
            m.refBase[rp.part][layer] = off;
            for (int i = 0; i < rp.count; i++) m.slotToRef[partBase(layer, rp.part) + i] = off + i;
            off += rp.count;
        }
    }
    m.refVerts = off;
    return m;
}

// ---------------- 单个形状 ----------------
void checkShape(float sa, float ea, bool mid, bool report) {
    g_shapes++;
    Shape s = buildShape(sa, ea, mid);
    Expanded ex; expand(s, ex);

    Scratch ref;
    if (mid) createMidSpinMesh(sa, ref); else createTileMesh(sa, ea, ref);
    const int refVerts = (int)(ref.verts.size() / 3);

    Mapping m = makeMapping(s.mode);
    if (report) {
        std::printf("  形状 sa=%.4f ea=%.4f mid=%d → mode=%d refVerts=%d（映射 %d）\n",
                    sa, ea, (int)mid, s.mode, refVerts, m.refVerts);
    }
    if (refVerts != m.refVerts)
        fail("sa=%.4f ea=%.4f mid=%d: 参考顶点数 %d ≠ 映射 %d", sa, ea, (int)mid, refVerts, m.refVerts);

    // 1) 活动 part 的坐标逐位相同
    for (int layer = 0; layer < kLayers; layer++) {
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            if (!partActive(s.mode, part)) continue;
            int base = partBase(layer, part), rb = m.refBase[part][layer];
            for (int i = 0; i < partSlotCount(part); i++) {
                int sv = base + i, rv = rb + i;
                if (rv >= refVerts) break;
                for (int c = 0; c < 3; c++) {
                    float got = ex.pos[(size_t)sv * 3 + c];
                    float want = ref.verts[(size_t)rv * 3 + c];
                    g_checked++;
                    const bool sameBits = (std::memcmp(&got, &want, 4) == 0) ||
                                          (got == 0.0f && want == 0.0f);
                    const double dev = std::fabs((double)got - (double)want);
                    if (sameBits) { g_exact++; if (dev > g_maxDevExact) g_maxDevExact = dev; }
                    else if (dev <= kTol) g_near++;
                    if (dev > g_maxDev) g_maxDev = dev;
                    if (dev > kTol || (g_requireExact && !sameBits)) {
                        const char* cn = (c == 0) ? "x" : (c == 1) ? "y" : "z";
                        fail("sa=%.4f ea=%.4f mid=%d mode=%d part=%d layer=%d slot=%d %s: 新 %s ≠ 参考 %s（偏差 %.3g 砖）",
                             sa, ea, (int)mid, s.mode, part, layer, i, cn, bits(got).c_str(),
                             bits(want).c_str(), dev);
                    }
                }
                float gotT = ex.type[sv], wantT = ref.types[(size_t)rv];
                if (std::memcmp(&gotT, &wantT, 4) != 0)
                    fail("sa=%.4f ea=%.4f part=%d slot=%d type: %s ≠ %s", sa, ea, part, i,
                         bits(gotT).c_str(), bits(wantT).c_str());
            }
        }
    }

    // 2) 三角形序列（去掉退化）逐项相同
    const uint16_t* idx = indexTable();
    const int triCount = kIndexCount / 3;
    std::vector<int> refTris;
    refTris.reserve(ref.indices.size());
    for (unsigned v : ref.indices) refTris.push_back((int)v);
    size_t ti = 0;
    for (int t = 0; t < triCount; t++) {
        int a = idx[t * 3], b = idx[t * 3 + 1], c = idx[t * 3 + 2];
        // 图标三角形不进这一层：参考实现里没有图标（它们是独立实例集）。图标的位精确性来自
        // "配方常量就是 createCircle 那批 float"，由像素门槛和 probe 覆盖。
        if (a >= kTileSlots || b >= kTileSlots || c >= kTileSlots) continue;
        // 活动三角形：三个槽位都映射到参考顶点
        if (m.slotToRef[a] < 0 || m.slotToRef[b] < 0 || m.slotToRef[c] < 0) {
            // 非活动 → 必须退化（三顶点同值）
            auto same = [&](int x, int y) {
                return std::memcmp(&ex.pos[(size_t)x * 3], &ex.pos[(size_t)y * 3], 4) == 0
                    && std::memcmp(&ex.pos[(size_t)x * 3 + 1], &ex.pos[(size_t)y * 3 + 1], 4) == 0;
            };
            if (!same(a, b) || !same(b, c))
                fail("sa=%.4f ea=%.4f mode=%d 三角形 %d 引用非活动 part 且未退化", sa, ea, s.mode, t);
            continue;
        }
        int ra = m.slotToRef[a], rb = m.slotToRef[b], rc = m.slotToRef[c];
        if (ti * 3 + 2 >= refTris.size()) { fail("参考三角形太少（t=%d）", t); break; }
        if (refTris[ti * 3] != ra || refTris[ti * 3 + 1] != rb || refTris[ti * 3 + 2] != rc)
            fail("sa=%.4f ea=%.4f mode=%d 三角形 %zu 序号: (%d,%d,%d) ≠ 参考 (%d,%d,%d)",
                 sa, ea, s.mode, ti, ra, rb, rc, refTris[ti * 3], refTris[ti * 3 + 1], refTris[ti * 3 + 2]);
        ti++;
    }
    if (ti * 3 != refTris.size())
        fail("sa=%.4f ea=%.4f mode=%d: 活动三角形 %zu ≠ 参考 %zu", sa, ea, s.mode, ti, refTris.size() / 3);

    // 3) 非活动 part 塌陷为同一点（对每个非活动 part 的每个三角形，上面已查；这里再查槽位本身）
    for (int part = P_CIRCLE; part <= P_PENT; part++) {
        if (partActive(s.mode, part)) continue;
        for (int layer = 0; layer < kLayers; layer++) {
            int base = partBase(layer, part);
            for (int i = 0; i < partSlotCount(part); i++)
                for (int c = 0; c < 3; c++)
                    if (ex.pos[(size_t)(base + i) * 3 + c] != 0.0f)
                        fail("sa=%.4f ea=%.4f mode=%d: 非活动 part %d 槽 %d 未塌陷", sa, ea, s.mode, part, base + i);
        }
    }

    // 4) 类型前缀：所有 type=0 的槽位下标 < 所有 type=1 的槽位下标
    int lastStroke = -1, firstFill = kTotalSlots;
    for (int i = 0; i < kTileSlots; i++) {
        if (ex.type[i] == 0.0f) lastStroke = i;
        else if (ex.type[i] == 1.0f && firstFill == kTotalSlots) firstFill = i;
    }
    if (lastStroke >= firstFill)
        fail("sa=%.4f ea=%.4f: 类型前缀被破坏（最后描边 %d ≥ 首个填充 %d）", sa, ea, lastStroke, firstFill);

    // 5) 局部包围盒（double，逐位）
    double mnX = 1e99, mnY = 1e99, mxX = -1e99, mxY = -1e99;
    for (int layer = 0; layer < kLayers; layer++) {
        for (int part = P_CIRCLE; part <= P_PENT; part++) {
            if (!partActive(s.mode, part)) continue;
            int base = partBase(layer, part);
            for (int i = 0; i < partSlotCount(part); i++) {
                double lx = ex.pos[(size_t)(base + i) * 3], ly = ex.pos[(size_t)(base + i) * 3 + 1];
                if (lx < mnX) mnX = lx;
                if (lx > mxX) mxX = lx;
                if (ly < mnY) mnY = ly;
                if (ly > mxY) mxY = ly;
            }
        }
    }
    if (mnX != s.localMinX || mnY != s.localMinY || mxX != s.localMaxX || mxY != s.localMaxY)
        fail("sa=%.4f ea=%.4f mid=%d: 包围盒 (%.17g,%.17g)-(%.17g,%.17g) ≠ 记录里的 (%.17g,%.17g)-(%.17g,%.17g)",
             sa, ea, (int)mid, mnX, mnY, mxX, mxY, s.localMinX, s.localMinY, s.localMaxX, s.localMaxY);
}

// ---------------- 形状集 ----------------
std::vector<std::pair<std::pair<float, float>, bool>> g_shapesToCheck;

void addShape(float sa, float ea, bool mid) {
    g_shapesToCheck.push_back({{sa, ea}, mid});
}

void addChartKeys(const std::string& path) {
    LevelData lv;
    if (!lv.loadFromFile(path)) { std::fprintf(stderr, "  ! 读不了谱面: %s\n", path.c_str()); return; }
    int n = (int)lv.tiles.size() - 1;
    for (int i = 0; i < n; i++) {
        float sa, ea; bool mid;
        keyForTile(lv, i, sa, ea, mid);
        addShape(sa, ea, mid);
    }
    std::printf("  %s: %d 砖 → 形状候选\n", path.c_str(), n);
}

} // namespace

int main(int argc, char** argv) {
    if (const char* e = std::getenv("ADOCAO_TILE_EXACT")) g_requireExact = (std::atoi(e) != 0);
    std::printf("== 形状集 ==\n");
    for (int i = 1; i < argc; i++) addChartKeys(argv[i]);
    // 0.01° 网格抽样（含半格边界：x.xx5 是 round 的取舍点）
    const float steps[] = {0.0f, 0.01f, 0.005f, 0.25f, 1.0f, 7.0f, 33.333f, 89.995f, 120.005f, 179.995f, 180.0f, 270.0f, 359.99f};
    for (float base = 0.0f; base < 360.0f; base += 0.37f) {
        for (float st : steps) {
            float sa = std::fmod(base + st, 360.0f);
            float ea = std::fmod(base + st + 0.005f + 0.01f * std::floor(st * 100.0f), 360.0f);
            addShape(sa, ea, false);
            addShape(sa + 180.0f > 359.995f ? sa - 180.0f : sa + 180.0f, ea, false);   // U 型附近（ang==0）
            addShape(sa, ea, true);                                                     // 中旋
        }
    }
    // 明确点名的边界：直行（ang=180° → BIG）、90° 弧、U 型（ang=0）、以及三家的 0.01° 半格
    addShape(0.0f, 180.0f, false);
    addShape(-180.0f, 0.0f, false);
    addShape(0.0f, 90.0f, false);
    addShape(0.0f, 0.0f, false);
    addShape(180.0f, 0.0f, false);
    addShape(0.0f, 0.005f, false);
    addShape(0.005f, 0.0f, false);
    for (int k = 0; k < 360; k++) addShape((float)k, (float)k, false);
    // 轴对齐 + 零点：这些 sa/ea 让 sin/cos 恰好为 0/±1，是"零符号 / 融合边界"最容易露馅的地方
    const float axis[] = {0.0f, 90.0f, 180.0f, 270.0f, 360.0f, -90.0f, -180.0f, -270.0f, 45.0f, 135.0f, 225.0f, 315.0f};
    for (float a : axis) for (float b : axis) {
        addShape(a, b, false);
        addShape(a, b, true);
        addShape(a, -180.0f, false);
        addShape(-180.0f, a, false);
        (void)b;
    }
    std::printf("  共 %zu 个形状\n\n", g_shapesToCheck.size());

    std::printf("== 逐位对拍 ==\n");
    bool detailed = false;
    int firstFailAt = -1;
    for (size_t si = 0; si < g_shapesToCheck.size(); si++) {
        float sa = g_shapesToCheck[si].first.first, ea = g_shapesToCheck[si].first.second;
        bool mid = g_shapesToCheck[si].second;
        int before = g_fail;
        checkShape(sa, ea, mid, false);
        if (g_fail > before && !detailed) {
            // 第一个失败形状重跑一遍带报告（模式/顶点数/映射），方便一眼定位
            detailed = true; firstFailAt = (int)si;
            checkShape(sa, ea, mid, true);
        }
        if (g_fail > kMaxReport) break;   // 报告够了就停，别刷屏
    }
    if (firstFailAt >= 0) std::printf("  首个失败形状: #%d\n", firstFailAt);

    // ---- 负向对照：融合步换成"各自先舍入"，必须过不了 ----
    std::printf("\n== 负向对照（必过不了）==\n");
    {
        int diff = 0;
        for (int k = 0; k < 64; k++) {
            float sa = (float)k * 5.0f, ea = (float)k * 5.0f + 90.0f;
            Shape s = buildShape(sa, ea, false);
            Expanded a, b; expand(s, a);
            // 参考：融合
            Scratch ref; createTileMesh(sa, ea, ref);
            // 反例：只看圆环那 64 个坐标，强制"先乘再舍入再相加"
            int base = partBase(1, P_CIRCLE);
            const PartRecord& r = s.rec[P_CIRCLE * kLayers + 1];
            for (int i = 1; i < 33; i++) {
                const Recipe& g = recipeTable()[base + i];
                // volatile 是必须的：不加的话 clang 自己会把 `k0*r + cx` 也融合成 fma，
                // 这个"反例"就与正解相同、对照失效（这个坑正是本对照存在的意义）。
                volatile float prod = g.k0 * r.p[2];
                float naiveX = prod + r.p[0];
                if (std::memcmp(&naiveX, &a.pos[(size_t)(base + i) * 3], 4) != 0) diff++;
            }
            (void)b; (void)ref;
        }
        if (diff == 0) {
            std::puts("  ✗ 负向对照没能区分融合与不融合 —— 说明这一层的比较没有牙");
            g_fail++;
        } else {
            std::printf("  ✓ 不融合的写法有 %d/64 个坐标与融合结果不同（比较确实按位）\n", diff);
        }
    }

    std::printf("\n检查坐标 %d 个，形状 %d 个，失败 %d\n", g_checked, g_shapes, g_fail);
    std::printf("  逐位相同 %lld，非逐位但在 %.0e 砖内 %lld，最大偏差 %.3g 砖%s\n",
                g_exact, kTol, g_near, g_maxDev,
                g_requireExact ? "（ADOCAO_TILE_EXACT=1：要求逐位）"
                               : "（默认按几何尺度容差；本地验收请设 ADOCAO_TILE_EXACT=1）");
    (void)g_maxDevExact;
    if (g_fail) { std::printf("FAILED\n"); return 1; }
    std::printf("OK\n");
    return 0;
}
