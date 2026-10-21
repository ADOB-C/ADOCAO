#include "core/timeline/Keyframes.hpp"

#include <algorithm>
#include <cmath>

namespace adofai {

void Channel::addKeyframe(double t, float v) {
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), t,
                               [](const Key& k, double x) { return k.time < x; });
    if (it != m_keys.end() && it->time == t) { it->value = v; return; }
    m_keys.insert(it, Key{t, v, 255});
}

void Channel::instantKeyframe(double t0, float v) {
    // 删掉 t0 之后的全部关键帧，再写一个瞬时帧（等价于 Re_ADOJAS 的 instantKeyframe）。
    while (!m_keys.empty() && m_keys.back().time > t0) m_keys.pop_back();
    addKeyframe(t0, v);
}

void Channel::addTween(double t0, double t1, float from, float to, Ease e, float fallback) {
    if (m_keys.empty()) addKeyframe(t0, fallback);
    // Kill(complete)：旧 tween 在 t0 **跳到它自己的终点**（t0 之前的历史不受影响）。
    // 实现方式：在 t0 插一个"跳变后的值"的关键帧作为新 tween 的起点（ease = 新缓动）。
    float startValue = from;
    {
        auto it = std::lower_bound(m_keys.begin(), m_keys.end(), t0,
                                   [](const Key& k, double x) { return k.time < x; });
        if (it != m_keys.begin()) {
            const size_t i = (size_t)(it - m_keys.begin()) - 1;
            const Key& prev = m_keys[i];
            if (prev.ease != 255 && i + 1 < m_keys.size()) startValue = m_keys[i + 1].value;  // 跳到旧终点
            else startValue = prev.value;
        }
    }
    if (t1 <= t0) {          // 零时长：瞬时置位
        auto at = std::lower_bound(m_keys.begin(), m_keys.end(), t0,
                                   [](const Key& k, double x) { return k.time < x; });
        if (at != m_keys.end() && at->time == t0) { at->value = to; at->ease = 255; }
        else m_keys.insert(at, Key{t0, to, 255});
        return;
    }
    // **缓动挂在起点帧上**（= 这一段 tween 的 ease）；终点帧是纯值。
    auto at0 = std::lower_bound(m_keys.begin(), m_keys.end(), t0,
                                [](const Key& k, double x) { return k.time < x; });
    if (at0 != m_keys.end() && at0->time == t0) { at0->value = startValue; at0->ease = (uint8_t)e; }
    else m_keys.insert(at0, Key{t0, startValue, (uint8_t)e});
    // 被 Kill 掉的旧 tween 在 (t0, t1) 之间的关键帧一并删除（否则旧终点会截断新 tween）
    m_keys.erase(std::remove_if(m_keys.begin(), m_keys.end(),
                                [&](const Key& k) { return k.time > t0 + 1e-12 && k.time < t1 - 1e-12; }),
                 m_keys.end());
    auto at1 = std::lower_bound(m_keys.begin(), m_keys.end(), t1,
                                [](const Key& k, double x) { return k.time < x; });
    if (at1 != m_keys.end() && at1->time == t1) { at1->value = to; at1->ease = 255; }
    else m_keys.insert(at1, Key{t1, to, 255});
}

float Channel::sample(double t) const {
    if (m_keys.empty()) return 0.0f;
    if (t <= m_keys.front().time) return m_keys.front().value;
    auto it = std::upper_bound(m_keys.begin(), m_keys.end(), t,
                               [](double x, const Key& k) { return x < k.time; });
    const size_t i = (size_t)(it - m_keys.begin()) - 1;   // t 之前最后一帧
    const Key& cur = m_keys[i];
    if (cur.ease == 255 || i + 1 >= m_keys.size()) return cur.value;
    const Key& next = m_keys[i + 1];
    const double span = next.time - cur.time;
    if (span <= 0.0) return next.value;
    const double p = std::clamp((t - cur.time) / span, 0.0, 1.0);
    const double q = easeApply((Ease)cur.ease, p);
    return (float)((double)cur.value + ((double)next.value - (double)cur.value) * q);
}

}  // namespace adofai
