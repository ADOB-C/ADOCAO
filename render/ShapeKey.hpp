// 形状表的键 + 量化精度阶梯（纯逻辑、无 GL —— 单测 `tests/shape_key_test.cpp`）。
//
// 为什么需要"精度档位"：GPU 端的形状表是一张 `kTexW` × rows 的 RGBA32F 纹理，
// 一行 1024 texel、每个形状 29 texel（`TileShape::kTexelsPerShape`）。行数被两个上限夹住：
//   ① 驱动的 `GL_MAX_TEXTURE_SIZE`（Intel UHD、macOS、2011 年的 GTX 590 都是 16384）；
//   ② 显存预算（`TileMesh::kMaxShapeRows` = 4096 行 = 67 MB）。
// 唯一形状数超过容量时**唯一安全的做法**是把 (sa, ea) 量化得粗一点（见下面的阶梯），
// **不能"照旧上传"**：纹理建不出来时 `texelFetch` 全返回 0 → 每块砖的 mask=0 → 所有砖塌陷成
// 零面积，画面纯黑，而且**一个错误都不报**（Issue #2：3,583,664 砖 / 3,560,627 形状 →
// 1024×100839 → Intel 驱动直接 "texture unloadable"，用户只看到黑屏）。
//
// **基准档 0.01° 不许动**（用户 2026-10-28："没碰壁根本不用动啊"）。曾经试过把基准提到
// float32 量级（1/16384° = 6.1e-5°）：5 张门槛谱全部落在最细档，但只有 Singularity 的
// 18 块砖几何真的变了 → 像素门槛动 152 px、要重存基线。**没有一张谱是"装不下"才需要精度**，
// 所以退回来了 —— 精度只在**容量装不下**时放粗，永远不主动改几何。0.01° 换来的表也小得可笑
// （MyC 294 / Singularity 9,349 / The Moon 215 个形状，最大的 angles360 也才 138 行）。
//
// 放粗阶梯（每档最多 2.5 倍，1° 封底）：0.01° → 0.02° → 0.04° → 0.1° → 0.2° → 0.5° → 1°。
// **1° 以上直接删掉**（用户："1° 以上可以删了，GTX 590 来了都能存"）：1° 网格下 (sa, ea) 最多
// 360×360，那个随机角度谱实测 1° 就是 130,321 个形状 = 3,691 行，离 16384 行还差四倍多。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace adofai {
namespace shapekey {

// 量化档位：**每度多少格**（steps/degree），从细到粗。0.01° = 100 档（历史行为），末档 1 = 1°。
inline constexpr int kStepsLadder[] = {100, 50, 25, 10, 5, 2, 1};
inline constexpr int kStepsCount = (int)(sizeof(kStepsLadder) / sizeof(kStepsLadder[0]));

// 角度（度）→ 量化整数。100 档（0.01°）就是 7.2.0 的算式 `round(deg * 100.0f)`，逐位相同。
inline int angleKey(float deg, int steps) { return (int)std::lround(deg * (float)steps); }

// 量化整数 → 角度（度）。100 档就是 `key / 100.0f`，逐位相同。
inline float angleFromKey(int key, int steps) { return (float)key / (float)steps; }

// 形状键：sa/ea 各 **20 位**、mid 1 位、style 3 位（与 7.2.0 同一布局）。
// 20 位装 ±524,288；最细档 100 下 sa ∈ [-180,180]、ea ∈ [0,360] → |键| ≤ 36,000 ✓。
// style 只有 3 位是**刻意**的：颜色（0x000000..0xFFFFFF，16.7M 个值）不进这个键，
// 它走逐实例 fill/stroke（见 docs/rendering.md）。
inline uint64_t packKey(int qsa, int qea, bool mid, uint8_t style = 0) {
    const uint32_t a = (uint32_t)qsa & 0xFFFFFu;
    const uint32_t b = (uint32_t)qea & 0xFFFFFu;
    return (uint64_t)a | ((uint64_t)b << 20) | (mid ? (1ull << 40) : 0ull) |
           ((uint64_t)(style & 0x7u) << 41);
}

// 一张 texW × rows 的表能放多少个形状（向下取整到整行）。
inline std::size_t capacity(int rows, int texelsPerShape, int texW) {
    if (rows <= 0 || texelsPerShape <= 0 || texW <= 0) return 0;
    return (std::size_t)rows * (std::size_t)texW / (std::size_t)texelsPerShape;
}

}  // namespace shapekey
}  // namespace adofai
