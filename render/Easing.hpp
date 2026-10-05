#pragma once

#include <cmath>
#include <string>
#include <unordered_map>
#include <functional>

// All standard ADOFAI easing functions.
// Must match Easing.ts constants exactly:
//   bounce: n1 = 7.5625, d1 = 2.75
//   back:   c1 = 1.70158, c3 = c1 + 1
//   elastic: c4 = (2*PI)/3, c5 = (2*PI)/4.5


namespace adofai {

namespace Easing {

using Func = std::function<float(float)>;

inline float Linear(float t) { return t; }

inline float InSine(float t)   { return 1.0f - std::cos(t * 3.14159265f / 2.0f); }
inline float OutSine(float t)  { return std::sin(t * 3.14159265f / 2.0f); }
inline float InOutSine(float t) { return -(std::cos(3.14159265f * t) - 1.0f) / 2.0f; }

inline float InQuad(float t)  { return t * t; }
inline float OutQuad(float t)  { return 1.0f - (1.0f - t) * (1.0f - t); }
inline float InOutQuad(float t) { return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f; }

inline float InCubic(float t)  { return t * t * t; }
inline float OutCubic(float t)  { return 1.0f - std::pow(1.0f - t, 3.0f); }
inline float InOutCubic(float t) { return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f; }

inline float InQuart(float t)  { return t * t * t * t; }
inline float OutQuart(float t)  { return 1.0f - std::pow(1.0f - t, 4.0f); }
inline float InOutQuart(float t) { return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f; }

inline float InQuint(float t)  { return t * t * t * t * t; }
inline float OutQuint(float t)  { return 1.0f - std::pow(1.0f - t, 5.0f); }
inline float InOutQuint(float t) { return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) / 2.0f; }

inline float InExpo(float t)  { return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f); }
inline float OutExpo(float t)  { return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
inline float InOutExpo(float t) {
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                     : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;
}

inline float InCirc(float t)  { return 1.0f - std::sqrt(1.0f - t * t); }
inline float OutCirc(float t)  { return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f)); }
inline float InOutCirc(float t) {
    return t < 0.5f ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
                     : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
}

inline float InBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}
inline float OutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}
inline float InOutBack(float t) {
    const float c1 = 1.70158f;
    const float c2 = c1 * 1.525f;
    return t < 0.5f ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
                     : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
}

inline float InElastic(float t) {
    const float c4 = (2.0f * 3.14159265f) / 3.0f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
}
inline float OutElastic(float t) {
    const float c4 = (2.0f * 3.14159265f) / 3.0f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}
inline float InOutElastic(float t) {
    const float c5 = (2.0f * 3.14159265f) / 4.5f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return t < 0.5f ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f
                     : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
}

inline float OutBounce(float t) {
    const float n1 = 7.5625f;
    const float d1 = 2.75f;
    if (t < 1.0f / d1) {
        return n1 * t * t;
    } else if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    } else if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    } else {
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
}
inline float InBounce(float t)  { return 1.0f - OutBounce(1.0f - t); }
inline float InOutBounce(float t) {
    return t < 0.5f ? (1.0f - OutBounce(1.0f - 2.0f * t)) / 2.0f
                     : (1.0f + OutBounce(2.0f * t - 1.0f)) / 2.0f;
}

// Name-based lookup (matches ADOFAI event ease strings like "OutSine", "InOutBounce")
inline Func byName(const std::string& name) {
    static const std::unordered_map<std::string, Func> map = {
        {"Linear",     Linear},
        {"InSine",     InSine},     {"OutSine",     OutSine},     {"InOutSine",     InOutSine},
        {"InQuad",     InQuad},     {"OutQuad",     OutQuad},     {"InOutQuad",     InOutQuad},
        {"InCubic",    InCubic},    {"OutCubic",    OutCubic},    {"InOutCubic",    InOutCubic},
        {"InQuart",    InQuart},    {"OutQuart",    OutQuart},    {"InOutQuart",    InOutQuart},
        {"InQuint",    InQuint},    {"OutQuint",    OutQuint},    {"InOutQuint",    InOutQuint},
        {"InExpo",     InExpo},     {"OutExpo",     OutExpo},     {"InOutExpo",     InOutExpo},
        {"InCirc",     InCirc},     {"OutCirc",     OutCirc},     {"InOutCirc",     InOutCirc},
        {"InBack",     InBack},     {"OutBack",     OutBack},     {"InOutBack",     InOutBack},
        {"InElastic",  InElastic},  {"OutElastic",  OutElastic},  {"InOutElastic",  InOutElastic},
        {"InBounce",   InBounce},   {"OutBounce",   OutBounce},   {"InOutBounce",   InOutBounce},
    };
    auto it = map.find(name);
    return it != map.end() ? it->second : Linear;
}

} // namespace Easing

}  // namespace adofai
