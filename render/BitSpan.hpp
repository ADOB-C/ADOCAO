#pragma once

// 位图的小工具（③ 切片 4 用：TrackVis 的逐砖可见位从 1 B/砖 改成 1 bit/砖）。
// 只做"闭区间置值"和"取一位"两件事，**不持有存储**（调用方给 `uint8_t*`），这样纯逻辑、
// 可以脱离 GL 单测（`tests/bit_span_test.cpp` 拿朴素逐位实现当参照对拍全部区间组合）。

#include <cstddef>
#include <cstdint>

namespace adofai {

inline bool bitAt(const uint8_t* bytes, size_t i) {
    return (bytes[i >> 3] >> (i & 7)) & 1u;
}

// 闭区间 [a, b] 全部置为 v。要求 b <= 位数-1（调用方裁剪）；a > b 时什么都不做。
inline void bitRangeSet(uint8_t* bytes, size_t a, size_t b, bool v) {
    if (a > b) return;
    const size_t ba = a >> 3, bb = b >> 3;
    if (ba == bb) {
        const uint8_t lo = (uint8_t)(0xFFu << (a & 7));
        const uint8_t hi = (uint8_t)(0xFFu >> (7 - (b & 7)));
        const uint8_t mask = (uint8_t)(lo & hi);
        if (v) bytes[ba] |= mask; else bytes[ba] &= (uint8_t)~mask;
        return;
    }
    const uint8_t lo = (uint8_t)(0xFFu << (a & 7));
    const uint8_t hi = (uint8_t)(0xFFu >> (7 - (b & 7)));
    if (v) bytes[ba] |= lo; else bytes[ba] &= (uint8_t)~lo;
    if (bb > ba + 1) {
        const uint8_t fill = v ? 0xFF : 0x00;
        for (size_t k = ba + 1; k < bb; ++k) bytes[k] = fill;
    }
    if (v) bytes[bb] |= hi; else bytes[bb] &= (uint8_t)~hi;
}

}  // namespace adofai
