#pragma once

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
        }
    }

    void clear() { m_blocks.clear(); }
    bool valid() const { return !m_blocks.empty(); }
    size_t blockCount() const { return m_blocks.size(); }
    size_t floorsPerBlock() const { return m_floors; }
    size_t bytes() const { return m_blocks.size() * sizeof(Block); }
    // 诊断/测试用：把覆盖到的块直接换出来（只读）
    const std::vector<Block>& blocks() const { return m_blocks; }

    // 与矩形相交的块，按层号升序依次交给 fn(first, count)。
    // 命中块内的砖**仍要**逐块做真正的 AABB 判定（块是保守包围盒）。
    template <class F>
    void forEachHitBlock(double vl, double vr, double vb, double vt, F&& fn) const {
        for (const Block& b : m_blocks) {
            if (b.maxX < vl || b.minX > vr || b.maxY < vb || b.minY > vt) continue;
            fn(b.first, b.count);
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
    std::vector<Block> m_blocks;
    size_t m_floors = kBlockFloors;
};

}  // namespace adofai
