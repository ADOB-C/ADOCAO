#pragma once

#include <cstdint>
#include <string_view>

namespace adofai {

// ADOFAI 的 32 个缓动名 —— 谱面里 `ease` 字段的**合法取值全集**（未知名回退 `Linear`）。
// 权威来源：`~/Repos/Re_ADOJAS/src/lib/Player/Easing.ts`（名字就是契约）。
// 枚举顺序与那份文件的对象键顺序一致；金标测试按**名字**查表，所以顺序不是接口。
enum class Ease : uint8_t {
    Linear = 0,
    InSine, OutSine, InOutSine,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InQuint, OutQuint, InOutQuint,
    InExpo, OutExpo, InOutExpo,
    InCirc, OutCirc, InOutCirc,
    InBack, OutBack, InOutBack,
    InElastic, OutElastic, InOutElastic,
    InBounce, OutBounce, InOutBounce,
    Unset,
    Count,
};

inline constexpr int kEaseCount = (int)Ease::Count;

// 未知/空名字 → `Ease::Linear`（对应 Re_ADOJAS 的 `EasingFunctions[name] || Linear`）。
Ease easeFromName(std::string_view name);

// 反向查名（诊断/日志）。合法枚举值一定返回非空；`Ease::Count` 返回 ""。
const char* easeName(Ease e);

// 纯函数，**不 clamp**：Re_ADOJAS 在 `evalTween` 里先把进度 clamp 到 [0,1] 再调用缓动。
// Back/Elastic/Bounce 在 [0,1] 内的取值会越界（<0 或 >1），这是 ADOFAI 的预期行为，
// 调用方**不要**再夹一次（否则运镜的"回弹/过冲"会被吃掉）。
double easeApply(Ease e, double t);

}  // namespace adofai
