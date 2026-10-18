#include "core/util/Easing.hpp"

#include <cmath>
#include <cstring>

namespace adofai {

namespace {

constexpr const char* kNames[kEaseCount] = {
    "Linear",
    "InSine", "OutSine", "InOutSine",
    "InQuad", "OutQuad", "InOutQuad",
    "InCubic", "OutCubic", "InOutCubic",
    "InQuart", "OutQuart", "InOutQuart",
    "InQuint", "OutQuint", "InOutQuint",
    "InExpo", "OutExpo", "InOutExpo",
    "InCirc", "OutCirc", "InOutCirc",
    "InBack", "OutBack", "InOutBack",
    "InElastic", "OutElastic", "InOutElastic",
    "InBounce", "OutBounce", "InOutBounce",
    "Unset",
};

constexpr double kPi = 3.14159265358979323846;

// OutBounce 的参考实现（Re_ADOJAS 里对 t 做了 `t -= …` 的原地修改，这里用局部变量照抄，
// 分段常数 7.5625 / 2.75 与 0.75 / 0.9375 / 0.984375 一个字都别动）。
double outBounce(double t) {
    constexpr double n1 = 7.5625;
    constexpr double d1 = 2.75;
    if (t < 1 / d1) return n1 * t * t;
    if (t < 2 / d1) { t -= 1.5 / d1; return n1 * t * t + 0.75; }
    if (t < 2.5 / d1) { t -= 2.25 / d1; return n1 * t * t + 0.9375; }
    t -= 2.625 / d1;
    return n1 * t * t + 0.984375;
}

}  // namespace

Ease easeFromName(std::string_view name) {
    for (int i = 0; i < kEaseCount; i++) {
        const char* n = kNames[i];
        if (name.size() == std::strlen(n) && std::memcmp(name.data(), n, name.size()) == 0)
            return (Ease)i;
    }
    return Ease::Linear;
}

const char* easeName(Ease e) {
    const int i = (int)e;
    return (i < kEaseCount) ? kNames[i] : "";   // Ease 底层是 uint8_t，i 不可能为负
}

double easeApply(Ease e, double t) {
    switch (e) {
    case Ease::Linear: return t;
    case Ease::Unset:  return t;

    case Ease::InSine:    return 1 - std::cos((t * kPi) / 2);
    case Ease::OutSine:   return std::sin((t * kPi) / 2);
    case Ease::InOutSine: return -(std::cos(kPi * t) - 1) / 2;

    case Ease::InQuad:    return t * t;
    case Ease::OutQuad:   return 1 - (1 - t) * (1 - t);
    case Ease::InOutQuad: return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;

    case Ease::InCubic:    return t * t * t;
    case Ease::OutCubic:   return 1 - std::pow(1 - t, 3);
    case Ease::InOutCubic: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;

    case Ease::InQuart:    return t * t * t * t;
    case Ease::OutQuart:   return 1 - std::pow(1 - t, 4);
    case Ease::InOutQuart: return t < 0.5 ? 8 * t * t * t * t : 1 - std::pow(-2 * t + 2, 4) / 2;

    case Ease::InQuint:    return t * t * t * t * t;
    case Ease::OutQuint:   return 1 - std::pow(1 - t, 5);
    case Ease::InOutQuint: return t < 0.5 ? 16 * t * t * t * t * t : 1 - std::pow(-2 * t + 2, 5) / 2;

    case Ease::InExpo:    return t == 0 ? 0 : std::pow(2, 10 * t - 10);
    case Ease::OutExpo:   return t == 1 ? 1 : 1 - std::pow(2, -10 * t);
    case Ease::InOutExpo: return t == 0 ? 0
                               : t == 1 ? 1
                               : t < 0.5 ? std::pow(2, 20 * t - 10) / 2
                                         : (2 - std::pow(2, -20 * t + 10)) / 2;

    case Ease::InCirc:    return 1 - std::sqrt(1 - t * t);
    case Ease::OutCirc:   return std::sqrt(1 - (t - 1) * (t - 1));
    case Ease::InOutCirc: return t < 0.5 ? (1 - std::sqrt(1 - std::pow(2 * t, 2))) / 2
                                         : (std::sqrt(1 - std::pow(-2 * t + 2, 2)) + 1) / 2;

    case Ease::InBack: {
        constexpr double c1 = 1.70158;
        constexpr double c3 = c1 + 1;
        return c3 * t * t * t - c1 * t * t;
    }
    case Ease::OutBack: {
        constexpr double c1 = 1.70158;
        constexpr double c3 = c1 + 1;
        return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
    }
    case Ease::InOutBack: {
        constexpr double c1 = 1.70158;
        constexpr double c2 = c1 * 1.525;
        return t < 0.5 ? (std::pow(2 * t, 2) * ((c2 + 1) * 2 * t - c2)) / 2
                       : (std::pow(2 * t - 2, 2) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
    }

    case Ease::InElastic: {
        const double c4 = (2 * kPi) / 3;
        return t == 0 ? 0 : t == 1 ? 1 : -std::pow(2, 10 * t - 10) * std::sin((t * 10 - 10.75) * c4);
    }
    case Ease::OutElastic: {
        const double c4 = (2 * kPi) / 3;
        return t == 0 ? 0 : t == 1 ? 1 : std::pow(2, -10 * t) * std::sin((t * 10 - 0.75) * c4) + 1;
    }
    case Ease::InOutElastic: {
        const double c5 = (2 * kPi) / 4.5;
        return t == 0 ? 0
             : t == 1 ? 1
             : t < 0.5 ? -(std::pow(2, 20 * t - 10) * std::sin((20 * t - 11.125) * c5)) / 2
                       : (std::pow(2, -20 * t + 10) * std::sin((20 * t - 11.125) * c5)) / 2 + 1;
    }

    case Ease::InBounce:    return 1 - outBounce(1 - t);
    case Ease::OutBounce:   return outBounce(t);
    case Ease::InOutBounce: return t < 0.5 ? (1 - outBounce(1 - 2 * t)) / 2
                                           : (1 + outBounce(2 * t - 1)) / 2;

    case Ease::Count: break;
    }
    return t;   // 不可达；保持"未知就是 Linear"的一致兜底
}

}  // namespace adofai
