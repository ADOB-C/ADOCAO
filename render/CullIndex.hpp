#pragma once

#include <algorithm>

// 剔除用的**粗块 AABB 索引**（④）。
//
// 背景：`TileMesh::ensureDrawList` 原来是"遍历全部 n 块砖、逐块做 AABB-视锥判定"，
// 每次视锥变化都是 O(n)（MYC 6,770,912 砖实测 **3.14 ms/次**，是 60 fps 预算的 19%）。
//
// 做法：每 `kBlockFloors` 层算一个保守 AABB（含砖半径外扩），查询时先筛块、再在命中块内
// 逐砖判 —— 判定谓词与全扫**逐字相同**，所以输出集合也**逐元素相同**（含顺序：
// 块按层号升序、块内按砖号升序，而 `TileMesh::m_drawOrder` 现在就是层号升序）。
//
// 为什么是 4096（`tests/cull_cost_probe.cpp --blocks …` 实测）：
//   * 两项成本走向相反：**扫块表 O(#blocks)** ≈ 0.42 ns/块、**块内 O(候选)** ≈ 0.47 ns/砖；
//   * MYC（局部稀疏）两项在 **4096** 交叉（0.0026 ms）；折叠谱（Singularity）最优是 256
//     （0.0092 ms），但 4096 也只是 0.1167 ms = 帧预算的 0.7%；
//   * 而**内存**差 16 倍：4096 → **0.0098 B/层**（2³¹ 的 20 MB）、256 → 320 MB、64 → 1.28 GB ✗。
//   所以主参数取 4096（与 ② 的分块、Re_ADOJAS 的窗口同粒度）；折叠谱要更快时按需给
//   "命中且候选大"的粗块建细表（两级），而不是全局铺细表。
//
// 它是**纯几何**的：不碰 GL、不碰 LevelData，只有"位置 → 块 AABB"和"矩形 → 命中块"，
// 所以能进 `tests/cull_index_test.cpp` 直接与全扫逐元素对拍。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adofai {

class CullIndex {
public:
    // 一块的保守 AABB（world 坐标，含砖半径外扩）+ 它覆盖的砖号区间。
    // 砖号区间是**连续**的：调用方（TileMesh）保证 `m_drawOrder[i] == i`。
    struct Block {
        double minX = 0, maxX = 0, minY = 0, maxY = 0;
        uint32_t first = 0, count = 0;
    };
    static_assert(sizeof(Block) == 40, "CullIndex::Block 应是 40 B（索引体积按它算）");

    static constexpr size_t kBlockFloors = 4096;
    // 两级索引（2026-10-16）：粗块（默认 4096 层）+ 块内细块（默认 512 层）。
    // 动机：一个 4096 层的粗块在一张"螺旋叠圆"的谱上，AABB 是**一整圈**（~1300 单位）——
    // 61×52 的小视锥也会命中它 → 逐砖剔除要白扫 4096 块砖（实测 196 万候选里只有 5,284 可见，
    // 冗余 370 倍，单帧 250~460 ms）。细块把 AABB 收到 ~160 单位，命中数掉一到两个数量级。
    // **只影响"扫哪些砖"，不影响"哪些砖可见"**（可见性仍由逐砖 AABB 判定）→ 出图逐字节相同。
    // **粒度是权衡，不是越小越好**（实测）：
    //   * 512 层：候选 373,248 / 可见 4,705（弧的 AABB 有 ~900 单位，过覆盖 80× ✗），p50 15~18 ms；
    //   * 64 层：AABB 收到 ~50 单位，但 range 数 ×8 → **每个 range 的固定开销**（cullRange 的
    //     SIMD 暂存/闭包/窗口查表）成了新主项，p50 反而 17~23 ms ✗；
    //   * 4096 层（单级）：p50 15~20 ms。→ 512 是这个权衡的甜点，保留。
    // 真要再降，得让"单个 range 的开销"变小（例如批量处理同一窗口的多个细块），记在 TODO。
    static constexpr size_t kSubFloors = 512;
    static constexpr size_t kSubPerBlock = kBlockFloors / kSubFloors;

    // 建表。`posAt(i, x, y)` 给出第 i 块砖的世界坐标（模板以便零开销地接 TileMesh 的
    // 块相对 float + double 原点，不必先摊出一份 double 数组）。
    // `halfExtent` = 砖的保守半边长（AABB 按 x±half 外扩，宁可多命中也绝不漏）。
    template <class F>
    void build(size_t n, F&& posAt, double halfExtent, size_t blockFloors = kBlockFloors) {
        m_blocks.clear();
        m_floors = blockFloors ? blockFloors : kBlockFloors;
        if (n == 0) return;
        const size_t nb = (n + m_floors - 1) / m_floors;
        m_blocks.resize(nb);
        // 细表只在**体积可控**时建（粗表 0.0098 B/砖，细表约 0.078 B/砖）：超预算就退回单级
        // （2³¹−1 层时细表 ~168 MB 太贵；那种规模的谱本地不存在，D1 的账仍以粗表为准）。
        const size_t subBytes = nb * kSubPerBlock * sizeof(Block);
        m_useSub = (m_floors % kSubFloors == 0) && subBytes <= kSubMaxBytes;
        m_sub.clear();
        if (m_useSub) m_sub.resize(nb * kSubPerBlock);
        for (size_t b = 0; b < nb; ++b) {
            const size_t lo = b * m_floors;
            const size_t hi = (lo + m_floors < n) ? (lo + m_floors) : n;
            Block& blk = m_blocks[b];
            blk.first = (uint32_t)lo;
            blk.count = (uint32_t)(hi - lo);
            double mnx = 0, mxx = 0, mny = 0, mxy = 0;
            for (size_t i = lo; i < hi; ++i) {
                double x = 0, y = 0;
                posAt(i, x, y);
                const double nlo_x = x - halfExtent, nhi_x = x + halfExtent;
                const double nlo_y = y - halfExtent, nhi_y = y + halfExtent;
                if (i == lo) {
                    mnx = nlo_x; mxx = nhi_x; mny = nlo_y; mxy = nhi_y;
                } else {
                    if (nlo_x < mnx) mnx = nlo_x;
                    if (nhi_x > mxx) mxx = nhi_x;
                    if (nlo_y < mny) mny = nlo_y;
                    if (nhi_y > mxy) mxy = nhi_y;
                }
            }
            blk.minX = mnx; blk.maxX = mxx; blk.minY = mny; blk.maxY = mxy;
            if (m_useSub) buildSub(b, lo, hi, posAt, halfExtent);
        }
    }

    void clear() { m_blocks.clear(); m_sub.clear(); m_useSub = false; }
    bool valid() const { return !m_blocks.empty(); }
    size_t blockCount() const { return m_blocks.size(); }
    size_t floorsPerBlock() const { return m_floors; }
    size_t bytes() const { return m_blocks.size() * sizeof(Block) + m_sub.size() * sizeof(Block); }
    bool hasSubLevel() const { return m_useSub; }
    // 诊断/测试用：把覆盖到的块直接换出来（只读）
    const std::vector<Block>& blocks() const { return m_blocks; }

    template <class F>
    void buildSub(size_t b, size_t lo, size_t hi, F&& posAt, double halfExtent) {
        const size_t nsub = kSubPerBlock;
        size_t i = lo;
        for (size_t si = 0; si < nsub && i < hi; ++si) {
            const size_t subLo = i;
            const size_t subHi = (i + kSubFloors < hi) ? (i + kSubFloors) : hi;
            Block& sb = m_sub[b * nsub + si];
            sb.first = (uint32_t)subLo;
            sb.count = (uint32_t)(subHi - subLo);
            double mnx = 0, mxx = 0, mny = 0, mxy = 0;
            for (size_t k = subLo; k < subHi; ++k) {
                double x = 0, y = 0;
                posAt(k, x, y);
                const double ax = x - halfExtent, bx = x + halfExtent;
                const double ay = y - halfExtent, by = y + halfExtent;
                if (k == subLo) { mnx = ax; mxx = bx; mny = ay; mxy = by; }
                else {
                    if (ax < mnx) mnx = ax; if (bx > mxx) mxx = bx;
                    if (ay < mny) mny = ay; if (by > mxy) mxy = by;
                }
            }
            sb.minX = mnx; sb.maxX = mxx; sb.minY = mny; sb.maxY = mxy;
            i = subHi;
        }
    }

    // 与矩形相交的块，按层号升序依次交给 fn(first, count)。
    // 命中块内的砖**仍要**逐块做真正的 AABB 判定（块是保守包围盒）。
    template <class F>
    void forEachHitBlock(double vl, double vr, double vb, double vt, F&& fn) const {
        for (size_t bi = 0; bi < m_blocks.size(); ++bi) {
            const Block& b = m_blocks[bi];
            if (b.maxX < vl || b.minX > vr || b.maxY < vb || b.minY > vt) continue;
            if (!m_useSub) { fn(b.first, b.count); continue; }
            // 细块：只交出真正与视锥相交的那些（每个细块的 AABB 比粗块小 8 倍量级）
            for (size_t si = 0; si < kSubPerBlock; ++si) {
                const Block& sb = m_sub[bi * kSubPerBlock + si];
                if (sb.count == 0) continue;
                if (sb.maxX < vl || sb.minX > vr || sb.maxY < vb || sb.minY > vt) continue;
                fn(sb.first, sb.count);
            }
        }
    }

    // ⑦ 容量边界：命中**细块**按到相机的距离排序，取到预算为止（累计砖数 ≤ budget）。
    // 关键：一定要在**细块**粒度上算距离 —— 粗块 AABB 是一整圈（~1300 单位），小视锥会被几百个块
    // "包住"（距离全 0），挑出来的块里可能一块砖都不在视锥内（实测 drawn=0、轨道消失 ✗）。
    // 细块 AABB 只有 ~160 单位，距离才真的代表"离相机多近"。确定性：等距按砖号。
    void collectHitSubNearest(double vl, double vr, double vb, double vt,
                              double camX, double camY, size_t budget,
                              std::vector<std::pair<size_t, size_t>>& out) const {
        out.clear();
        struct Hit { double d2; size_t first; size_t count; };
        std::vector<Hit> hits;
        if (!m_useSub) {                        // 没有细表就只能交粗块（并保留原来的顺序语义）
            for (const Block& b : m_blocks) {
                if (b.maxX < vl || b.minX > vr || b.maxY < vb || b.minY > vt) continue;
                hits.push_back({0.0, b.first, b.count});
            }
        } else {
            for (size_t bi = 0; bi < m_blocks.size(); ++bi) {
                const Block& b = m_blocks[bi];
                if (b.maxX < vl || b.minX > vr || b.maxY < vb || b.minY > vt) continue;
                for (size_t si = 0; si < kSubPerBlock; ++si) {
                    const Block& sb = m_sub[bi * kSubPerBlock + si];
                    if (sb.count == 0) continue;
                    if (sb.maxX < vl || sb.minX > vr || sb.maxY < vb || sb.minY > vt) continue;
                    const double cx = std::min(std::max(camX, sb.minX), sb.maxX);
                    const double cy = std::min(std::max(camY, sb.minY), sb.maxY);
                    const double dx = camX - cx, dy = camY - cy;
                    hits.push_back({dx * dx + dy * dy, sb.first, sb.count});
                }
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b2) {
            if (a.d2 != b2.d2) return a.d2 < b2.d2;
            return a.first < b2.first;
        });
        size_t acc = 0;
        for (const Hit& h : hits) {
            if (acc >= budget) break;
            out.emplace_back(h.first, h.first + h.count);
            acc += h.count;
        }
    }

    // 诊断：命中块数 / 候选砖数（用于核对"候选 ≈ 可见"的冗余度）
    void countCandidates(double vl, double vr, double vb, double vt,
                         size_t& hitBlocks, size_t& candidates) const {
        hitBlocks = 0;
        candidates = 0;
        forEachHitBlock(vl, vr, vb, vt, [&](uint32_t, uint32_t count) {
            ++hitBlocks;
            candidates += count;
        });
    }

private:
    static constexpr size_t kSubMaxBytes = 64u << 20;   // 细表体积上限（超了就退回单级）
    std::vector<Block> m_blocks;
    std::vector<Block> m_sub;
    bool m_useSub = false;
    size_t m_floors = kBlockFloors;
};

}  // namespace adofai
