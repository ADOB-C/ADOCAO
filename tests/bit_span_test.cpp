// `render/BitSpan.hpp` 的单测：位图区间置值。**参照实现是朴素逐位循环**，对拍所有
// (a,b,v) 组合（含跨字节、单点、整字节、边界），外加"覆盖写入"（先全 1 再置 0、反之亦然）。

#include "render/BitSpan.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

using namespace adofai;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++g_fail; }
}
}  // namespace

int main() {
    constexpr size_t N = 137;                       // 非字节整数倍，逼出尾字节
    const size_t bytes = (N + 7) / 8;
    size_t cases = 0;
    for (int fillPass = 0; fillPass < 2; ++fillPass) {
        for (size_t a = 0; a < N; ++a) {
            for (size_t b = a; b < N; ++b) {
                for (int vi = 0; vi < 2; ++vi) {
                    const bool v = vi != 0;
                    std::vector<uint8_t> fast(bytes, fillPass ? 0xFF : 0x00);
                    std::vector<uint8_t> slow = fast;
                    bitRangeSet(fast.data(), a, b, v);
                    for (size_t k = a; k <= b; ++k) {      // 朴素参照
                        const size_t byte = k >> 3;
                        const uint8_t mask = (uint8_t)(1u << (k & 7));
                        if (v) slow[byte] |= mask; else slow[byte] &= (uint8_t)~mask;
                    }
                    if (fast != slow) { check(false, "区间置值结果与朴素实现不同"); return 1; }
                    // 区间外必须不动
                    for (size_t k = 0; k < N; ++k) {
                        if (k >= a && k <= b) continue;
                        if (bitAt(fast.data(), k) != bitAt(slow.data(), k)) { check(false, "区间外被改了"); return 1; }
                    }
                    ++cases;
                }
            }
        }
    }
    // a > b：什么都不做
    {
        std::vector<uint8_t> buf(bytes, 0x00);
        bitRangeSet(buf.data(), 10, 9, true);
        for (size_t k = 0; k < N; ++k) check(!bitAt(buf.data(), k), "a>b 时不应改动");
    }
    // 全区间：不越界写尾字节之外的位
    {
        std::vector<uint8_t> buf(bytes + 1, 0xAA);   // 多一个哨兵字节
        bitRangeSet(buf.data(), 0, N - 1, true);
        check(buf[bytes] == 0xAA, "不应写到最后一字节之外");
        for (size_t k = 0; k < N; ++k) check(bitAt(buf.data(), k), "全区间应为 1");
    }
    std::printf("bitRangeSet：%zu 组区间对拍通过（N=%zu）\n", cases, N);
    if (g_fail == 0) std::printf("全部通过\n");
    return g_fail == 0 ? 0 : 1;
}
