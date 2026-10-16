// ④ 粗块 AABB 索引的逐元素对拍测试（ctest `cull_index`）。
//
// 主张：**按块筛 + 块内判** 得到的可见砖序列，与**全扫**逐元素相同（同集合、同顺序）。
// 这条是像素门槛能保持 51/51 逐字节的前提 —— 集合或顺序任一变了，出图就可能变。
//
// 覆盖：直线 / 圆（折返）/ 随机游走 / 密集螺旋 四组合成数据 + 一张真实谱（angles360）。
// 每组的判定都用**逐砖自己的半边长**（模拟真实形状 localBBox 大小不一），而索引按
// **最大半边长**保守建表 —— 这正是渲染侧的情形。
//
// 反面对照（必须有）：故意把建表用的 halfExtent 调小 → 对拍**必须**失败，
// 否则说明这个测试根本抓不住"索引漏砖"。

#include "render/CullIndex.hpp"
#include "core/level/LevelData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

struct Pt { double x, y, half; };

struct Rect { double l, r, b, t; const char* name; };

// 全扫：按砖号升序，逐砖用它自己的半边长判 AABB-矩形相交
std::vector<uint32_t> fullScan(const std::vector<Pt>& pts, const Rect& q) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt& p = pts[i];
        if (p.x - p.half > q.r || p.x + p.half < q.l) continue;
        if (p.y - p.half > q.t || p.y + p.half < q.b) continue;
        out.push_back((uint32_t)i);
    }
    return out;
}

// 索引路：先筛块，再在块内逐砖用同一个谓词判
std::vector<uint32_t> indexedScan(const CullIndex& idx, const std::vector<Pt>& pts, const Rect& q) {
    std::vector<uint32_t> out;
    idx.forEachHitBlock(q.l, q.r, q.b, q.t, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; ++i) {
            const Pt& p = pts[i];
            if (p.x - p.half > q.r || p.x + p.half < q.l) continue;
            if (p.y - p.half > q.t || p.y + p.half < q.b) continue;
            out.push_back(i);
        }
    });
    return out;
}

// 对一组矩形跑对拍；返回 (检查数, 可见数合计, 候选数合计)
struct Tally { size_t cases = 0, visible = 0, candidates = 0; };

Tally compareAll(const char* what, const std::vector<Pt>& pts, const std::vector<Rect>& rects,
                 double buildHalf) {
    CullIndex idx;
    idx.build(pts.size(), [&](size_t i, double& x, double& y) { x = pts[i].x; y = pts[i].y; },
              buildHalf);
    Tally t;
    for (const Rect& q : rects) {
        const std::vector<uint32_t> ref = fullScan(pts, q);
        const std::vector<uint32_t> got = indexedScan(idx, pts, q);
        size_t hit = 0, cand = 0;
        idx.countCandidates(q.l, q.r, q.b, q.t, hit, cand);
        ++t.cases;
        t.visible += ref.size();
        t.candidates += cand;
        if (ref != got) {
            // 找出第一处不同，好定位
            size_t k = 0;
            while (k < ref.size() && k < got.size() && ref[k] == got[k]) ++k;
            std::printf("FAIL %s[%s]：第 %zu 个元素起不同（ref %zu 个 / got %zu 个；"
                        "ref[%zu]=%u got[%zu]=%u）\n",
                        what, q.name, k, ref.size(), got.size(), k,
                        k < ref.size() ? ref[k] : 0xffffffffu, k,
                        k < got.size() ? got[k] : 0xffffffffu);
            ++g_fail;
        }
        // 顺序单调（块序 + 块内砖号序 ⇒ 全局升序）
        check(std::is_sorted(got.begin(), got.end()), std::string(what) + "：输出应升序");
    }
    std::printf("     %-22s %zu 块｜%zu 用例｜可见合计 %zu｜候选合计 %zu（冗余 %.2fx）\n",
                what, idx.blockCount(), t.cases, t.visible, t.candidates,
                t.visible ? (double)t.candidates / (double)t.visible : 0.0);
    return t;
}

std::vector<Pt> makeLine(size_t n) {
    std::vector<Pt> v;
    for (size_t i = 0; i < n; ++i) v.push_back({(double)i * 1.0, 0.0, 0.5});
    return v;
}

std::vector<Pt> makeCircle(size_t n, double r) {   // 折返：绕很多圈，屏幕同一片区域层号差很远
    std::vector<Pt> v;
    for (size_t i = 0; i < n; ++i) {
        const double a = (double)i * 0.05;         // 每砖 0.05 rad → n=20000 绕 159 圈
        v.push_back({r * std::cos(a), r * std::sin(a), 0.5});
    }
    return v;
}

std::vector<Pt> makeWalk(size_t n) {               // 确定性随机游走（LCG）
    std::vector<Pt> v;
    uint64_t s = 12345;
    double x = 0, y = 0;
    for (size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double a = (double)((s >> 33) % 6283185) / 1e6;   // [0,2π)
        x += std::cos(a);
        y += std::sin(a);
        v.push_back({x, y, 0.32});
    }
    return v;
}

std::vector<Pt> makeSpiral(size_t n) {             // 密集螺旋：层号小范围里塞很多砖
    std::vector<Pt> v;
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i * 0.01;
        const double r = 0.02 * t;
        v.push_back({r * std::cos(t), r * std::sin(t), 0.5});
    }
    return v;
}

// 围绕若干砖心、按几种"缩放"造查询矩形（zoom 越大矩形越小）
std::vector<Rect> rectsAround(const std::vector<Pt>& pts, const std::vector<size_t>& centers,
                              const std::vector<double>& halfSizes) {
    std::vector<Rect> out;
    for (size_t c : centers) {
        if (c >= pts.size()) continue;
        for (double h : halfSizes) {
            char* nm = new char[64];
            std::snprintf(nm, 64, "c%zu/h%.1f", c, h);
            out.push_back({pts[c].x - h, pts[c].x + h, pts[c].y - h, pts[c].y + h, nm});
        }
    }
    out.push_back({1e9, 1e9 + 1, 1e9, 1e9 + 1, "空区"});
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== ④ 粗块 AABB 索引：与全扫逐元素对拍 ===\n");
    const std::vector<double> halves{0.5, 2.0, 8.0, 40.0, 400.0, 5000.0};

    // 1) 直线（primer 型）
    {
        auto pts = makeLine(40000);
        compareAll("直线 4 万", pts, rectsAround(pts, {0, 1000, 20000, 39999}, halves), 0.5);
    }
    // 2) 圆 / 折返（Singularity 型：块的 AABB 会被撑大）
    {
        auto pts = makeCircle(20000, 100.0);
        compareAll("圆 2 万（折返）", pts, rectsAround(pts, {0, 5000, 10000, 19999}, halves), 0.5);
    }
    // 3) 随机游走
    {
        auto pts = makeWalk(30000);
        compareAll("随机游走 3 万", pts, rectsAround(pts, {0, 7000, 15000, 29999}, halves), 0.32);
    }
    // 4) 密集螺旋
    {
        auto pts = makeSpiral(25000);
        compareAll("螺旋 2.5 万", pts, rectsAround(pts, {0, 8000, 16000, 24999}, halves), 0.5);
    }
    // 5) 真实谱（取位置；形状半边长这里统一用 0.5，真实渲染是各形状自己的 localBBox）
    if (argc > 1) {
        LevelData lv;
        if (lv.loadFromFile(argv[1])) {
            std::vector<Pt> pts;
            const size_t n = lv.angleData.size();
            pts.reserve(n);
            for (size_t i = 0; i < n; ++i)
                pts.push_back({lv.tiles[i].position[0], lv.tiles[i].position[1], 0.5});
            if (!pts.empty()) {
                compareAll("真实谱", pts,
                           rectsAround(pts, {0, n / 4, n / 2, n * 3 / 4, n - 1}, halves), 0.5);
            }
        } else {
            std::printf("FAIL 载入测试谱失败：%s\n", argv[1]);
            ++g_fail;
        }
    }

    // 6) **反面对照**：建表用的 halfExtent 故意小于砖的实际半边长 → 必须对拍失败
    {
        std::vector<Pt> pts;
        for (size_t i = 0; i < 12000; ++i)
            pts.push_back({std::cos((double)i * 0.3) * 50.0, std::sin((double)i * 0.3) * 50.0, 0.5});
        CullIndex bad;   // halfExtent = 0.0（比 0.5 小）→ 边缘砖会被漏掉
        bad.build(pts.size(), [&](size_t i, double& x, double& y) { x = pts[i].x; y = pts[i].y; }, 0.0);
        const Rect q{-51.0, -50.0, -1.0, 1.0, "边缘"};
        const size_t refN = fullScan(pts, q).size();
        const size_t gotN = indexedScan(bad, pts, q).size();
        std::printf("     反面对照（halfExtent 偏小）：全扫 %zu / 索引 %zu（应当不同）\n", refN, gotN);
        check(refN != gotN, "反面对照：索引漏砖时对拍**必须**失败，现在却是相同的");
        // 正面对照：同样数据、正确的外扩 → 必须相同
        CullIndex good;
        good.build(pts.size(), [&](size_t i, double& x, double& y) { x = pts[i].x; y = pts[i].y; }, 0.5);
        check(fullScan(pts, q) == indexedScan(good, pts, q),
              "正面对照：正确外扩时必须逐元素相同");
    }

    if (g_fail == 0) std::printf("全部通过\n");
    else std::printf("%d 项不通过\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
