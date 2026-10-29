// 形状表的键 + 量化精度阶梯（纯逻辑、无 GL —— 单测 `tests/shape_key_test.cpp`）。
//
// 为什么需要"精度档位"：GPU 端的形状表是一张 `kTexW` × rows 的 RGBA32F 纹理，
// 一行 1024 texel、每个形状 29 texel（`TileShape::kTexelsPerShape`）。行数被两个上限夹住：
//   ① 驱动的 `GL_MAX_TEXTURE_SIZE`（Intel UHD / macOS 都是 16384）；
//   ② 显存预算（`TileMesh::kMaxShapeRows` = 4096 行 = 67 MB）。
// 唯一形状数超过容量时**唯一安全的做法**是把 (sa, ea) 量化得粗一点（0.01° → 0.1° → …），
// **不能"照旧上传"**：纹理建不出来时 `texelFetch` 全返回 0 → 每块砖的 mask=0 → 所有砖塌陷成
// 零面积，画面纯黑，而且**一个错误都不报**（Issue #2：3,583,664 砖 / 3,560,627 形状 →
// 1024×100839 → Intel 驱动直接 "texture unloadable"，用户只看到黑屏）。
//
// 末档 90° 时 (sa, ea) 只剩 4×4 种组合，任何谱面都装得下 —— 所以这个阶梯**一定**收敛。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace adofai {
namespace shapekey {

// 量化档位：单位 0.01°。1 = 历史行为（0.01°，逐位不变）。
inline constexpr int kQuantumLadder[] = {1, 10, 25, 50, 100, 200, 500, 1000, 2250, 5000, 9000};
inline constexpr int kQuantumCount = (int)(sizeof(kQuantumLadder) / sizeof(kQuantumLadder[0]));

// 角度（度）→ 量化整数。档位 1 时与改造前的 `round(deg * 100.0f)` 逐位相同。
inline int angleKey(float deg, int quantum) {
    if (quantum <= 1) return (int)std::lround(deg * 100.0f);
    return (int)std::lround(deg * (100.0f / (float)quantum));
}

// 量化整数 → 角度（度）。档位 1 时与 `key / 100.0f` 逐位相同（×1.0f 是恒等）。
inline float angleFromKey(int key, int quantum) {
    return (float)key * (float)quantum / 100.0f;
}

// 形状键：sa/ea 各 20 位、mid 1 位、style 3 位 —— 档位 1 下 |键| ≤ 18000，20 位绝对够。
inline uint64_t packKey(int qsa, int qea, bool mid, uint8_t style = 0) {
    const uint32_t a = (uint32_t)qsa & 0xFFFFFu;
    const uint32_t b = (uint32_t)qea & 0xFFFFFu;
    return (uint64_t)a | ((uint64_t)b << 20) | (mid ? (1ull << 40) : 0ull) |
           ((uint64_t)(style & 0x7u) << 41);
}

// 一张 texW × rows 的表能放多少个形状（向上对齐到整行）。
inline std::size_t capacity(int rows, int texelsPerShape, int texW) {
    if (rows <= 0 || texelsPerShape <= 0 || texW <= 0) return 0;
    return (std::size_t)rows * (std::size_t)texW / (std::size_t)texelsPerShape;
}

}  // namespace shapekey
}  // namespace adofai
