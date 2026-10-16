// 剔除策略对比探针（④）：在同一份真实数据、同一套 AABB 判定下，量三种"候选集"策略的
// **候选数 / 漏砖数 / 耗时 / 索引体积**：
//
//   A  全扫 O(n)                    —— 我们现状（`TileMesh::ensureDrawList` 遍历 m_drawOrder）
//   B  "当前层号 ± 4096" 索引窗口    —— Re_ADOJAS 播放路径（`VISIBLE_INDEX_WINDOW = 4096`）
//   C  4096 层块 AABB 查询           —— 提案：每块一个 AABB，先筛块、再在块内逐砖判
//   D  均匀网格（哈希）+ 查完排序     —— 我最初在 ③ 报告里提的那条：格子 → 下标流，
//                                       查完把候选按"绘制序下标"排序以恢复绘制顺序
//   D2 均匀网格 + 多路归并            —— D 的免排序版：每格的下标流**预先按绘制序排好**，
//                                       查询按栅格序取格，做 #cells 路归并（O(k log #cells)）
//
// 口径（写清楚，免得数字被读过头）：
//   * 用**砖中心 ± 0.5 砖**当 AABB，三种策略用同一个判定 → 相互可比；
//     真实渲染用的是"形状 localBBox + 位置"（每个形状的 bbox 大小不同），
//     所以这里的**可见数**略小于真实值，但三种策略同等受益 → 结论不变。
//   * 相机矩形按 `Camera::frustumBounds()` 的约定：halfH = 6/(zoom/100)、halfW = halfH*16/9，
//     再照 `ensureDrawList` 加 20 世界单位外扩 —— 这样 zoom 是**真实缩放**。
//   * 砖位置直接用 `LevelData::tiles[i].position`（double，渲染侧是块相对 f32 + 块基点，
//     两者差在 ulp 级，不影响候选计数）。
//
// 用法：
//   adocao_cull_cost_probe <谱> [--tiles k1,k2,...] [--zooms 25,100] [--repeat N]
//                             [--cells 5,16,64]

#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {

constexpr int kBlockFloors = 4096;     // = ②③ 的分块粒度（也是 Re_ADOJAS 的窗口半径）
constexpr double kMargin = 20.0;       // 与 ensureDrawList 的外扩一致
constexpr double kHalfTile = 0.5;      // 砖中心 ± 0.5 砖

struct Block {
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    uint32_t first = 0, count = 0;
};

struct Roi {
    double l = 0, r = 0, b = 0, t = 0;
};

// D 用的均匀网格（哈希）。键 = cx * 2^22 + (cy + 2^21)，与 Re_ADOJAS 的无碰撞打包同构。
struct HashGrid {
    double cell = 16.0;
    std::unordered_map<int64_t, std::vector<uint32_t>> map;
    size_t entries = 0;
};

int64_t cellKey(int64_t cx, int64_t cy) { return cx * 4194304LL + (cy + 2097152LL); }

HashGrid buildGrid(const LevelData& lv, double cell) {
    HashGrid g;
    g.cell = cell;
    const size_t n = lv.angleData.size();
    g.map.reserve(n / 4 + 16);
    for (size_t i = 0; i < n; ++i) {
        const int64_t cx = (int64_t)std::floor(lv.tiles[i].position[0] / cell);
        const int64_t cy = (int64_t)std::floor(lv.tiles[i].position[1] / cell);
        g.map[cellKey(cx, cy)].push_back((uint32_t)i);
    }
    for (auto& kv : g.map) g.entries += kv.second.size();
    return g;
}

bool inRect(double x, double y, const Roi& q) {
    return x >= q.l && x <= q.r && y >= q.b && y <= q.t;
}

// 防 DCE：三种策略的结果都要"被用掉"，否则编译器会把整个循环删掉（实测 B/C 计时变成 0.000 ms）
volatile size_t g_sink = 0;

double msNow() {
    using clk = std::chrono::steady_clock;
    static clk::time_point start = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - start).count();
}

struct Result {
    size_t visible = 0;        // 中心落在矩形里的砖数（= "真的该画"的代理）
    size_t candidates = 0;     // 这个策略要测多少块砖
    size_t found = 0;          // 策略最终留下的
    size_t missed = 0;         // 可见但被策略漏掉的（正确性）
    double ms = 0;             // 实测耗时（取 repeat 次里的最小值）
    double sortMs = 0;         // 其中"恢复绘制序的排序"占多少（D 才有）
};

// A：全扫
Result runFull(const LevelData& lv, const Roi& q, int repeat) {
    Result r;
    const size_t n = lv.angleData.size();
    double best = 1e18;
    for (int rep = 0; rep < repeat; ++rep) {
        size_t found = 0;
        const double t0 = msNow();
        for (size_t i = 0; i < n; ++i) {
            const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
            if (x - kHalfTile > q.r || x + kHalfTile < q.l) continue;
            if (y - kHalfTile > q.t || y + kHalfTile < q.b) continue;
            ++found;
        }
        best = std::min(best, msNow() - t0);
        r.found = found;
    }
    r.candidates = n;
    r.ms = best;
    return r;
}

// B：当前层号 ± kBlockFloors（Re_ADOJAS 播放路径）
Result runWindow(const LevelData& lv, const Roi& q, size_t cur, int repeat) {
    Result r;
    const size_t n = lv.angleData.size();
    const size_t from = (cur > (size_t)kBlockFloors) ? cur - kBlockFloors : 0;
    const size_t to = std::min(n, cur + (size_t)kBlockFloors + 1);
    r.candidates = to - from;
    double best = 1e18;
    for (int rep = 0; rep < repeat; ++rep) {
        size_t found = 0;
        const double t0 = msNow();
        for (size_t i = from; i < to; ++i) {
            const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
            if (x - kHalfTile > q.r || x + kHalfTile < q.l) continue;
            if (y - kHalfTile > q.t || y + kHalfTile < q.b) continue;
            ++found;
        }
        best = std::min(best, msNow() - t0);
        r.found = found;
        g_sink += found;
    }
    r.ms = best;
    // 正确性：窗口外还有多少"可见"
    for (size_t i = 0; i < n; ++i) {
        if (i >= from && i < to) continue;
        const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
        if (x + kHalfTile < q.l || x - kHalfTile > q.r) continue;
        if (y + kHalfTile < q.b || y - kHalfTile > q.t) continue;
        ++r.missed;
    }
    return r;
}

// C：4096 层块 AABB 查询
std::vector<Block> buildBlocks(const LevelData& lv) {
    const size_t n = lv.angleData.size();
    const size_t nb = (n + kBlockFloors - 1) / kBlockFloors;
    std::vector<Block> bl(nb);
    for (size_t b = 0; b < nb; ++b) {
        const size_t lo = b * kBlockFloors;
        const size_t hi = std::min(lo + (size_t)kBlockFloors, n);
        Block& blk = bl[b];
        blk.first = (uint32_t)lo;
        blk.count = (uint32_t)(hi - lo);
        double mnx = 1e308, mxx = -1e308, mny = 1e308, mxy = -1e308;
        for (size_t i = lo; i < hi; ++i) {
            const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
            mnx = std::min(mnx, x - kHalfTile); mxx = std::max(mxx, x + kHalfTile);
            mny = std::min(mny, y - kHalfTile); mxy = std::max(mxy, y + kHalfTile);
        }
        blk.minX = mnx; blk.maxX = mxx; blk.minY = mny; blk.maxY = mxy;
    }
    return bl;
}

Result runBlocks(const LevelData& lv, const std::vector<Block>& bl, const Roi& q, int repeat) {
    Result r;
    // 先筛块（这一步就是"索引查询"，成本与块数成正比）
    std::vector<uint32_t> hits;
    hits.reserve(bl.size() / 8 + 8);
    for (size_t b = 0; b < bl.size(); ++b) {
        const Block& k = bl[b];
        if (k.maxX < q.l || k.minX > q.r || k.maxY < q.b || k.minY > q.t) continue;
        hits.push_back((uint32_t)b);
        r.candidates += k.count;
    }
    double best = 1e18;
    for (int rep = 0; rep < repeat; ++rep) {
        size_t found = 0;
        const double t0 = msNow();
        for (uint32_t b : hits) {
            const Block& k = bl[b];
            for (size_t i = k.first; i < k.first + k.count; ++i) {
                const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
                if (x - kHalfTile > q.r || x + kHalfTile < q.l) continue;
                if (y - kHalfTile > q.t || y + kHalfTile < q.b) continue;
                ++found;
            }
        }
        best = std::min(best, msNow() - t0);
        r.found = found;
        g_sink += found;
    }
    r.ms = best;
    return r;
}

// D：均匀网格查询 + 排序（恢复绘制序）。排序键用**砖号**代替绘制序下标 —— 真实渲染里
// `m_drawOrder` 是砖号的排列，换成那个键**成本一样**（k log k），只是键不同。
// D2：格子按下标**预排序** + 查询时多路归并（免掉全局 sort）
Result runGridMerge(const LevelData& lv, const HashGrid& g, const Roi& q, int repeat) {
    Result r;
    const double ql = q.l - kHalfTile, qr = q.r + kHalfTile;
    const double qb = q.b - kHalfTile, qt = q.t + kHalfTile;
    const int64_t minCx = (int64_t)std::floor(ql / g.cell), maxCx = (int64_t)std::floor(qr / g.cell);
    const int64_t minCy = (int64_t)std::floor(qb / g.cell), maxCy = (int64_t)std::floor(qt / g.cell);
    // 取格（栅格序）——每格的流天然已按砖号排序（构建时就按 i 递增 push）
    std::vector<const std::vector<uint32_t>*> lists;
    for (int64_t cx = minCx; cx <= maxCx; ++cx)
        for (int64_t cy = minCy; cy <= maxCy; ++cy) {
            auto it = g.map.find(cellKey(cx, cy));
            if (it != g.map.end()) lists.push_back(&it->second);
        }
    std::vector<size_t> pos(lists.size(), 0);
    for (const auto* l : lists) r.candidates += l->size();
    double best = 1e18, bestMerge = 1e18;
    for (int rep = 0; rep < repeat; ++rep) {
        size_t found = 0;
        std::fill(pos.begin(), pos.end(), 0);
        const double t0 = msNow();
        for (;;) {                                   // 多路归并：每次挑最小的头（这里用简单扫描，
            size_t pick = (size_t)-1;                // 真实实现用 #cells 路小顶堆；#cells 很小）
            for (size_t s = 0; s < lists.size(); ++s) {
                if (pos[s] >= lists[s]->size()) continue;
                if (pick == (size_t)-1 || (*lists[s])[pos[s]] < (*lists[pick])[pos[pick]]) pick = s;
            }
            if (pick == (size_t)-1) break;
            const uint32_t i = (*lists[pick])[pos[pick]++];
            const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
            if (x - kHalfTile > q.r || x + kHalfTile < q.l) continue;
            if (y - kHalfTile > q.t || y + kHalfTile < q.b) continue;
            ++found;
        }
        const double t1 = msNow();
        best = std::min(best, t1 - t0);
        bestMerge = std::min(bestMerge, t1 - t0);
        r.found = found;
        g_sink += found;
    }
    r.ms = best;
    r.sortMs = bestMerge;   // 这里语义是"归并"的耗时（同一列输出）
    return r;
}

Result runGrid(const LevelData& lv, const HashGrid& g, const Roi& q, int repeat) {
    Result r;
    // ⚠️ 格子范围要按**砖半径**外扩：判定用的是"AABB 与矩形相交"，中心的 x 可以落在
    // 矩形外 0.5 砖处（属于隔壁格）。不外扩就会漏掉边界那几块（探针一开始就是这么漏了 2 块）。
    const double ql = q.l - kHalfTile, qr = q.r + kHalfTile;
    const double qb = q.b - kHalfTile, qt = q.t + kHalfTile;
    const int64_t minCx = (int64_t)std::floor(ql / g.cell), maxCx = (int64_t)std::floor(qr / g.cell);
    const int64_t minCy = (int64_t)std::floor(qb / g.cell), maxCy = (int64_t)std::floor(qt / g.cell);
    std::vector<uint32_t> cand;
    for (int64_t cx = minCx; cx <= maxCx; ++cx) {
        for (int64_t cy = minCy; cy <= maxCy; ++cy) {
            auto it = g.map.find(cellKey(cx, cy));
            if (it == g.map.end()) continue;
            cand.insert(cand.end(), it->second.begin(), it->second.end());
        }
    }
    r.candidates = cand.size();
    double best = 1e18, bestSort = 1e18;
    for (int rep = 0; rep < repeat; ++rep) {
        size_t found = 0;
        std::vector<uint32_t> work = cand;          // 计时包含这次拷贝（真实实现就地复用缓冲）
        const double t0 = msNow();
        std::sort(work.begin(), work.end());        // ← 恢复绘制序：k log k（k = 候选数）
        const double t1 = msNow();
        for (uint32_t i : work) {
            const double x = lv.tiles[i].position[0], y = lv.tiles[i].position[1];
            if (x - kHalfTile > q.r || x + kHalfTile < q.l) continue;
            if (y - kHalfTile > q.t || y + kHalfTile < q.b) continue;
            ++found;
        }
        const double t2 = msNow();
        best = std::min(best, t2 - t0);
        bestSort = std::min(bestSort, t1 - t0);
        r.found = found;
        g_sink += found;
    }
    r.ms = best;
    r.sortMs = bestSort;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    adofai::archive::install();      // .xz/.zst/.adocao 的解码后端（core 只认接口）
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <谱> [--tiles k1,k2] [--zooms 25,100] [--repeat N]\n", argv[0]);
        return 2;
    }
    std::vector<int> zooms{25, 100};
    std::vector<size_t> tilesArg;
    std::vector<double> cells{5.0, 16.0, 64.0};
    int repeat = 5;
    for (int i = 2; i < argc; ++i) {
        auto nums = [&](const char* s, std::vector<long>& out) {
            out.clear();
            const char* p = s;
            while (*p) {
                char* e = nullptr;
                const long v = std::strtol(p, &e, 10);
                if (e == p) break;
                out.push_back(v);
                p = (*e == ',') ? e + 1 : e;
            }
        };
        std::vector<long> v;
        if (std::strcmp(argv[i], "--tiles") == 0 && i + 1 < argc) {
            nums(argv[++i], v);
            for (long x : v) tilesArg.push_back((size_t)x);
        } else if (std::strcmp(argv[i], "--zooms") == 0 && i + 1 < argc) {
            nums(argv[++i], v);
            zooms.clear();
            for (long x : v) zooms.push_back((int)x);
        } else if (std::strcmp(argv[i], "--cells") == 0 && i + 1 < argc) {
            std::vector<long> cv;
            nums(argv[++i], cv);
            cells.clear();
            for (long x : cv) cells.push_back((double)x);
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = std::atoi(argv[++i]);
            if (repeat < 1) repeat = 1;
        }
    }

    LevelData lv;
    if (!lv.loadFromFile(argv[1])) { std::fprintf(stderr, "加载失败: %s\n", argv[1]); return 1; }
    const size_t n = lv.angleData.size();
    if (n == 0) { std::fprintf(stderr, "空谱\n"); return 1; }

    // 索引构建成本（一次性）
    const double t0 = msNow();
    const std::vector<Block> bl = buildBlocks(lv);
    const double buildMs = msNow() - t0;

    std::printf("谱 %s：%zu 层｜块 %zu 个（每 %d 层）｜索引 %zu B（%.4f B/层）"
                "｜构建 %.1f ms（一次性）\n",
                argv[1], n, bl.size(), kBlockFloors, bl.size() * sizeof(Block),
                (double)(bl.size() * sizeof(Block)) / (double)n, buildMs);

    // D 的索引（每种 cell 一份）：构建成本 + 体积
    struct GridInfo { double cell; HashGrid g; double buildMs; size_t bytes; };
    std::vector<GridInfo> grids;
    for (double c : cells) {
        const double tb = msNow();
        HashGrid g = buildGrid(lv, c);
        const double bm = msNow() - tb;
        // 体积估算：下标流 4 B/砖 + 每格一个 unordered_map 结点 + vector 头（≈48 B）
        const size_t bytes = g.entries * sizeof(uint32_t) + g.map.size() * 48;
        grids.push_back({c, std::move(g), bm, bytes});
        std::printf("  网格 cell=%-4.0f：%zu 格｜%zu 下标｜约 %zu B（%.2f B/层）｜构建 %.1f ms\n",
                    c, grids.back().g.map.size(), grids.back().g.entries, bytes,
                    (double)bytes / (double)n, bm);
    }

    if (tilesArg.empty()) {
        // 默认取几个有代表性的播放位置：开头 / 1/4 / 中点 / 3/4
        tilesArg = {0, n / 4, n / 2, n * 3 / 4};
    }
    for (int z : zooms) {
        const double halfH = 6.0 / ((double)z / 100.0);
        const double halfW = halfH * 16.0 / 9.0;
        for (size_t cur : tilesArg) {
            if (cur >= n) continue;
            const double cx = lv.tiles[cur].position[0], cy = lv.tiles[cur].position[1];
            Roi q{cx - halfW - kMargin, cx + halfW + kMargin,
                  cy - halfH - kMargin, cy + halfH + kMargin};
            Result a = runFull(lv, q, repeat);
            a.visible = a.found;
            Result b = runWindow(lv, q, cur, repeat);
            Result c = runBlocks(lv, bl, q, repeat);
            std::printf("  zoom %-5d 层 %-9zu 可见 %-7zu │ A 全扫 %8.3f ms │ "
                        "B 窗口 %6.3f ms 漏 %-7zu │ C 块 %6.3f ms 漏 %-3zu",
                        z, cur, a.visible, a.ms, b.ms, b.missed, c.ms, c.missed);
            for (const GridInfo& gi : grids) {
                Result d = runGrid(lv, gi.g, q, repeat);
                Result d2 = runGridMerge(lv, gi.g, q, repeat);
                std::printf(" │ D(cell=%.0f) %6.3f ms（排序 %5.3f）候选 %-6zu 漏 %-4zu"
                            " │ D2归并 %6.3f ms 漏 %-4zu",
                            gi.cell, d.ms, d.sortMs, d.candidates,
                            a.visible > d.found ? a.visible - d.found : 0,
                            d2.ms, a.visible > d2.found ? a.visible - d2.found : 0);
            }
            std::printf("\n");
        }
    }
    return 0;
}
