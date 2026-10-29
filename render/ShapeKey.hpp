// 形状表的键 + 量化精度阶梯（纯逻辑、无 GL —— 单测 `tests/shape_key_test.cpp`）。
//
// 为什么需要"精度档位"：GPU 端的形状表是一张 `kTexW` × rows 的 RGBA32F 纹理，
// 一行 1024 texel、每个形状 29 texel（`TileShape::kTexelsPerShape`）。行数被两个上限夹住：
//   ① 驱动的 `GL_MAX_TEXTURE_SIZE`（Intel UHD / macOS / 2011 年的 GTX 590 都是 16384）；
//   ② 显存预算（`TileMesh::kMaxShapeRows` = 4096 行 = 67 MB）。
// 唯一形状数超过容量时**唯一安全的做法**是把 (sa, ea) 量化得粗一点（见下面的阶梯），
// **不能"照旧上传"**：纹理建不出来时 `texelFetch` 全返回 0 → 每块砖的 mask=0 → 所有砖塌陷成
// 零面积，画面纯黑，而且**一个错误都不报**（Issue #2：3,583,664 砖 / 3,560,627 形状 →
// 1024×100839 → Intel 驱动直接 "texture unloadable"，用户只看到黑屏）。
//
// **基准精度（2026-10-28 用户："0.01° 太看不起人家了"）**：不拿 0.01° 当起点 —— angleData 是
// float32，`±180°` 处的 ULP 只有 **1.5e-5°**，0.01° 比它粗三个数量级，等于把谱面的角度随手抹平
// （这一步会改几何：见 `docs/rendering.md`「形状表的容量闸」）。所以阶梯从
// **1/16384° = 6.1e-5°**（≈ 4 ULP，已在数据精度下限附近，再细没有意义）起，**×2 放粗**
// （每档只放大 2 倍，落点离容量上限不会超过 2 倍），到 **1° 封底**：1° 网格下 (sa, ea) 最多
// 360×360（实测那个随机角度谱 1° → 130,321 个形状），配上 mid×style 也仍在老 GPU 的 16384 行
// （578k 形状）之内 —— 更粗的档位纯属浪费。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace adofai {
namespace shapekey {

// 量化档位：**每度多少格**（steps/degree），从细到粗。末档 1 = 1°。
inline constexpr int kStepsLadder[] = {16384, 8192, 4096, 2048, 1024, 512, 256,
                                       128,   64,   32,   16,   8,    4,   2, 1};
inline constexpr int kStepsCount = (int)(sizeof(kStepsLadder) / sizeof(kStepsLadder[0]));

// 角度（度）→ 量化整数。100 档（0.01°）与改造前的 `round(deg * 100.0f)` 逐位相同。
inline int angleKey(float deg, int steps) { return (int)std::lround(deg * (float)steps); }

// 量化整数 → 角度（度）。100 档与 `key / 100.0f` 逐位相同。
inline float angleFromKey(int key, int steps) { return (float)key / (float)steps; }

// 形状键：sa/ea 各 **24 位**、mid 1 位、style 3 位（共 52 位，uint64 富余）。
// 24 位装 ±8,388,607；最细档 16384 下 sa ∈ [-180, 180]、ea ∈ [0, 360] →
// |键| ≤ 360×16384 = 5,898,240 ✓ —— 这也是"最细只能到 16384"的原因，再细就撞 24 位边界。
// 100 档（0.01°）下 |键| ≤ 36000，与改造前逐位相同。
inline uint64_t packKey(int qsa, int qea, bool mid, uint8_t style = 0) {
    const uint32_t a = (uint32_t)qsa & 0xFFFFFFu;
    const uint32_t b = (uint32_t)qea & 0xFFFFFFu;
    return (uint64_t)a | ((uint64_t)b << 24) | (mid ? (1ull << 48) : 0ull) |
           ((uint64_t)(style & 0x7u) << 49);
}

// 一张 texW × rows 的表能放多少个形状（向下取整到整行）。
inline std::size_t capacity(int rows, int texelsPerShape, int texW) {
    if (rows <= 0 || texelsPerShape <= 0 || texW <= 0) return 0;
    return (std::size_t)rows * (std::size_t)texW / (std::size_t)texelsPerShape;
}

}  // namespace shapekey
}  // namespace adofai
