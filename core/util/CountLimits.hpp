#pragma once

// 层数/元素数的**权威类型**与边界算术（2^31−1 层目标，#0）。
//
// 为什么单独抽成纯函数：真造一张 21 亿层的谱不可能（本地最大的 primer 只有 6.84e7 层 = 目标的 **3%**），
// 所以"边界不坏"这件事**只能**在纯函数上单测 —— 见 `tests/limits_test.cpp`。
//
// 规则（改 #0 相关代码时照抄）：
//   * 计数/长度一律 **int64_t**：不用 `int`（2^31−1 时 `+1` 就溢出），也不用无符号
//     （`size() - 1` 在空容器上会环绕成天文数字）；
//   * 只在**必须**交给 32 位 API 的地方（`GLsizei`、`int` 屏坐标、`vector::resize` 的实参…）
//     做一次**显式**检查 → 超了就让调用方报错，**绝不静默截断**（今天 `parseInt` 就是这么错的：
//     `parseInt("2147483648")` 得到 −2147483648，然后 floor 落到 `floor >= 0 && floor < n`
//     的守卫里被**无声丢弃**）。

#include <cstdint>

namespace adofai {

// 2^31−1：本目标的规模上限，也是 `GLsizei` / `int32_t` 的上限。
inline constexpr int64_t kInt32Max = 2147483647;
inline constexpr int64_t kTileGoal = kInt32Max;   // 目标：2^31−1 层

// 角度数 → 层数（第 0 层那一格 +1）。老的写法是 `(int)angleData.size() + 1`：
// 在 2^31−1 时那个 `+1` 就是有符号溢出（UBSan 报过），n 变负 → `resize(负)` 直接炸。
inline constexpr int64_t countFromAngleCount(int64_t angleCount) {
    return angleCount + 1;
}

// 层数优先、否则用角度数推（导出模式 `tiles` 是空的，见 Timeline.cpp）。
inline constexpr int64_t countFromTilesOrAngles(int64_t tileCount, int64_t angleCount) {
    return tileCount > 0 ? tileCount : countFromAngleCount(angleCount);
}

inline constexpr bool fitsInt32(int64_t v) { return v >= 0 && v <= kInt32Max; }

// 缩小到 int32（喂 `int` 参数的 API 前必须过这里）：越界返回 false，由调用方**明确**失败。
inline constexpr bool toInt32Checked(int64_t v, int& out) {
    if (!fitsInt32(v)) return false;
    out = static_cast<int>(v);
    return true;
}

// —— 这两条 static_assert 就是"类型层面不再坏"的机械证明 ——
// ① 2^31−1 时 +1 必须得到 2^31（int64 里合法；老的 `int` 在这里已经溢出为负）；
static_assert(countFromAngleCount(kInt32Max) == kInt32Max + 1,
              "2^31−1 层的计数不能溢出");
// ② 而它已经**超出 int32** —— 所以任何往 32 位收窄的地方都必须走 toInt32Checked 并由调用方处理。
static_assert(!fitsInt32(countFromAngleCount(kInt32Max)),
              "2^31−1 层的计数已超 int32：收窄必须显式失败");

}  // namespace adofai
