// 砖块几何自测：中旋砖（angleData=999）必须是**五边形**，而且几何顺序要满足渲染器的隐含约定。
//
// 为什么需要它：中旋砖曾经走 `createTileMesh(eA,eA)`（`ang == 0` 分支 = 圆 r=0.30 + 方块），
// 在游戏里看着像"大圆 + 菱形塞在砖格里"。改成五边形之后，对不对只能靠肉眼看图 —— 这里把
// 看出来的结论钉成断言，并且带**负向对照**（把旧的几何喂进同一套断言，必须过不了；
// 过得了就说明断言没牙，等于白写）。
//
// 断言的三件事：
//   1. 描边顶点必须整段排在填充顶点前面 —— `TileMesh.cpp` 是**扫描 types 的前缀**来算
//      strokeVertCount / strokeIdxCount 的（`for (vi…) if (type < 0.5) strokeVertCount++; else break;`），
//      几何生成器一旦把两层交错，渲染会静默少画/画错（不报错、不掉帧，只是看起来不对）。
//   2. 五边形的三条不变量（对任意入砖方向 a1 都成立）：
//        * 7 + 7 个顶点 / 3 + 3 个三角形；
//        * 尖角顶点 = 中点 + (-a1 方向 × 半宽)，即**朝来路**伸出 w；
//        * 面积 = 3w²（方块 l×2w 再加底 2w、高 w 的三角，l == w）。
//   3. 普通砖没被这次改动带歪（顶点/索引数、描边前缀这两条同样查）。
//
// 用法：`ctest --test-dir build`（或直接跑 build/tests/adocao_tile_geometry_test）

#include "TileGeometry.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

constexpr float PI = 3.14159265358979f;
constexpr float EPS = 1e-4f;

// 不含 outline 的五边形面积：方块 l×2w  +  三角 (2w × w / 2) = 3w²（因为 l == w）
float pentagonArea(float w) { return 3.0f * w * w; }

// 顶点表的顺序 **不是**边界序：0,1,2,3,4 里 2→3→4 会让闭合边穿过矩形左边（自交），
// 直接对 0..4 求 shoelace 会得到 3w²/2。真正的边界序是 0,1,2,4,3（尖角插在 2 和 3 之间）。
constexpr int BOUNDARY[5] = {0, 1, 2, 4, 3};

float shoelace(const Scratch& sc, int base) {
    float a = 0.0f;
    for (int i = 0; i < 5; i++) {
        int i0 = base + BOUNDARY[i], i1 = base + BOUNDARY[(i + 1) % 5];
        float x0 = sc.verts[(size_t)i0 * 3], y0 = sc.verts[(size_t)i0 * 3 + 1];
        float x1 = sc.verts[(size_t)i1 * 3], y1 = sc.verts[(size_t)i1 * 3 + 1];
        a += x0 * y1 - x1 * y0;
    }
    return 0.5f * a;
}

// 渲染器真正填的是三角形（不是顶点序），所以面积也直接按三角形求和查一遍
float triangleAreaSum(const Scratch& sc, size_t firstTri, size_t triCount) {
    float a = 0.0f;
    for (size_t t = 0; t < triCount; t++) {
        unsigned i0 = sc.indices[(firstTri + t) * 3], i1 = sc.indices[(firstTri + t) * 3 + 1],
                 i2 = sc.indices[(firstTri + t) * 3 + 2];
        float x0 = sc.verts[(size_t)i0 * 3], y0 = sc.verts[(size_t)i0 * 3 + 1];
        float x1 = sc.verts[(size_t)i1 * 3], y1 = sc.verts[(size_t)i1 * 3 + 1];
        float x2 = sc.verts[(size_t)i2 * 3], y2 = sc.verts[(size_t)i2 * 3 + 1];
        a += 0.5f * std::fabs(x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1));
    }
    return a;
}

std::string near(float got, float want, const char* what) {
    if (std::fabs(got - want) > EPS) {
        char buf[192];
        std::snprintf(buf, sizeof buf, "%s: 期望 %.5f，实际 %.5f", what, want, got);
        return buf;
    }
    return {};
}

// 描边在前、填充在后（TileMesh.cpp 的前缀扫描依赖它）
std::string strokePrefixSelfTest(const Scratch& sc, const char* what) {
    size_t stroke = 0;
    while (stroke < sc.types.size() && sc.types[stroke] < 0.5f) stroke++;
    for (size_t i = stroke; i < sc.types.size(); i++)
        if (sc.types[i] < 0.5f) {
            char buf[192];
            std::snprintf(buf, sizeof buf, "%s: 第 %zu 个顶点是描边，但前面已经出现过填充（层次交错）",
                          what, i);
            return buf;
        }
    if (stroke == 0 || stroke == sc.types.size()) {
        char buf[192];
        std::snprintf(buf, sizeof buf, "%s: 描边/填充的顶点数异常（描边 %zu / 总 %zu）",
                      what, stroke, sc.types.size());
        return buf;
    }
    if (stroke != sc.verts.size() / 3 / 2) {
        char buf[192];
        std::snprintf(buf, sizeof buf, "%s: 描边与填充顶点数不等（%zu vs %zu）—— 描边层应是把同形状放大 OUTLINE",
                      what, stroke, sc.types.size() - stroke);
        return buf;
    }
    return {};
}

// 五边形的三条不变量。返回非空 = 失败。
std::string pentagonInvariants(const Scratch& sc, float a1, const char* what) {
    if (sc.verts.size() != 14 * 3)
        return std::string(what) + ": 顶点数不是 7+7=14（实际 " + std::to_string(sc.verts.size() / 3) + "）";
    if (sc.types.size() != 14)
        return std::string(what) + ": types 数不是 14";
    if (sc.indices.size() != 18)
        return std::string(what) + ": 索引数不是 3+3 个三角形 = 18（实际 " + std::to_string(sc.indices.size()) + "）";

    const float wOut = TILE_WIDTH + OUTLINE;     // 描边层半宽
    const float wIn = TILE_WIDTH - OUTLINE;      // 填充层半宽
    const float m1 = std::cos(a1 * PI / 180.0f), m2 = std::sin(a1 * PI / 180.0f);
    const float mx = -m1 * 0.04f, my = -m2 * 0.04f;   // 整体沿 -a1 挪 0.04

    // 尖角：中点 + (-w * a1方向)
    if (auto e = near(sc.verts[4 * 3], mx - wOut * m1, "尖角 x"); !e.empty()) return e;
    if (auto e = near(sc.verts[4 * 3 + 1], my - wOut * m2, "尖角 y"); !e.empty()) return e;
    // 尖角必须在"来路"一侧：与前进方向点乘为负
    if ((sc.verts[4 * 3] - mx) * m1 + (sc.verts[4 * 3 + 1] - my) * m2 >= 0.0f)
        return std::string(what) + ": 尖角没有朝来路（点乘 >= 0）";

    // 面积不随 a1 变：边界序 shoelace 与"渲染器真正填的三角形"两条都要对上
    if (auto e = near(std::fabs(shoelace(sc, 0)), pentagonArea(wOut), "描边层边界面积"); !e.empty()) return e;
    if (auto e = near(std::fabs(shoelace(sc, 7)), pentagonArea(wIn), "填充层边界面积"); !e.empty()) return e;
    if (auto e = near(triangleAreaSum(sc, 0, 3), pentagonArea(wOut), "描边层三角形面积和"); !e.empty()) return e;
    if (auto e = near(triangleAreaSum(sc, 3, 3), pentagonArea(wIn), "填充层三角形面积和"); !e.empty()) return e;

    // 5 个角互不相同（不是退化三角形/重复点）
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++) {
            float dx = sc.verts[(size_t)i * 3] - sc.verts[(size_t)j * 3];
            float dy = sc.verts[(size_t)i * 3 + 1] - sc.verts[(size_t)j * 3 + 1];
            if (std::sqrt(dx * dx + dy * dy) < 1e-3f)
                return std::string(what) + ": 有重合的角（" + std::to_string(i) + ", " + std::to_string(j) + "）";
        }
    return {};
}

// 负向对照用的旧几何：`createTileMesh(a, a)`（ang == 0 分支）
std::string oldShapeInvariants(const Scratch& sc) {
    if (sc.verts.size() == 14 * 3) return "旧的圆+方块几何居然也是 14 个顶点 —— 断言认不出旧形状";
    if (std::fabs(triangleAreaSum(sc, 0, sc.indices.size() / 3) - pentagonArea(TILE_WIDTH + OUTLINE)) < EPS)
        return "旧的圆+方块几何面积居然等于五边形 —— 断言认不出旧形状";
    return {};
}

// 调用点护栏（源码级，和 scripts/check-cli-help.sh 一个路子）：几何单测管不到"谁来调它"。
// 2026-10 起 TileMesh 走 `TileShape::buildShape()`（几何在 GPU 展开），**不再**调用这个文件里的
// `createTileMesh/createMidSpinMesh` —— 所以护栏改成：TileMesh 必须建形状表，且不许退回 CPU 顶点汤。
// 要求显式传路径（不许静默跳过：静默回退让测试全绿这个坑踩过）。
std::string callSiteSelfTest(const char* tileMeshCpp) {
    std::ifstream f(tileMeshCpp);
    if (!f) return std::string("读不到 ") + tileMeshCpp;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (src.find("TileShape::buildShape") == std::string::npos)
        return "TileMesh.cpp 里没有 `TileShape::buildShape` —— 砖没有走「形状表 + GPU 展开」";
    if (src.find("createMidSpinMesh(") != std::string::npos ||
        src.find("createTileMesh(") != std::string::npos)
        return "TileMesh.cpp 里又出现了 CPU 顶点汤（createTileMesh/createMidSpinMesh）";
    // 图标必须是"跟着砖"的：每砖一个 iconBits + 图标 part 进 canonical 表，
    // 而且**和砖在同一次 draw** 里（图标在拖尾之前；拖尾是半透明混合，
    // 改造前"砖 → 拖尾 → 行星 → 图标"让不透明的图标把拖尾擦掉一块，那是 bug）。
    if (src.find("m_iconGroup") != std::string::npos || src.find("m_iconEntries") != std::string::npos)
        return "TileMesh.cpp 里又出现了独立的图标实例集（m_iconGroups/m_iconEntries）";
    if (src.find("m_iconBits") == std::string::npos)
        return "TileMesh.cpp 里没有 m_iconBits —— 图标没有跟着砖走";
    if (src.find("drawIcons") != std::string::npos)
        return "TileMesh.cpp 里又出现了单独的图标 draw 了 —— 图标必须跟砖同一次 draw（在拖尾之前）";
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    int failed = 0;
    auto report = [&](const std::string& err, const char* what) {
        if (err.empty()) { std::printf("ok   %s\n", what); return; }
        std::printf("FAIL %s\n     %s\n", what, err.c_str());
        failed++;
    };

    // 1) 中旋砖：五边形 + 三条不变量，覆盖各种入砖方向
    {
        const float angles[] = {0.0f, 30.0f, 45.0f, 90.0f, 137.0f, -180.0f, 270.0f};
        std::string err;
        for (float a : angles) {
            Scratch sc;
            createMidSpinMesh(a, sc);
            if (auto e = strokePrefixSelfTest(sc, "中旋砖层次"); !e.empty()) { err = e; break; }
            if (auto e = pentagonInvariants(sc, a, "中旋砖五边形不变量"); !e.empty()) {
                char buf[256];
                std::snprintf(buf, sizeof buf, "%s（a1=%.0f）", e.c_str(), a);
                err = buf;
                break;
            }
        }
        report(err, "中旋砖 = 五边形（7+7 顶点 / 尖角朝来路 / 面积 3w²，7 个方向都查）");
    }

    // 2) 负向对照：旧的 `createTileMesh(a1, a1)` 必须过不了同一套断言
    {
        Scratch old;
        createTileMesh(0.0f, 0.0f, old);
        report(oldShapeInvariants(old),
               "负向对照：旧的 `createTileMesh(a,a)`（圆+方块）过不了五边形断言");
    }

    // 3) 普通砖没被带歪：顶点/索引数 + 描边前缀
    {
        Scratch sc;
        createTileMesh(0.0f, 60.0f, sc);
        std::string err = strokePrefixSelfTest(sc, "普通砖层次");
        if (err.empty() && sc.verts.empty()) err = "普通砖几何为空";
        if (err.empty() && sc.indices.empty()) err = "普通砖索引为空";
        if (err.empty() && sc.verts.size() / 3 != sc.types.size()) err = "普通砖 顶点数 != types 数";
        report(err, "普通砖：描边/填充分层顺序不变（TileMesh 的前缀扫描依赖它）");
    }

    // 4) 调用点护栏（需要 TileMesh.cpp 的路径）
    if (argc < 2) {
        std::printf("FAIL 调用点护栏：没给 render/TileMesh.cpp 的路径\n"
                    "     用法：%s <repo>/render/TileMesh.cpp\n", argv[0]);
        failed++;
    } else {
        report(callSiteSelfTest(argv[1]),
               "调用点护栏：TileMesh.cpp 必须走 TileShape::buildShape，且不许退回 CPU 顶点汤");
    }

    std::printf("\n%s\n", failed ? "有失败用例" : "全部通过");
    return failed ? 1 : 0;
}
