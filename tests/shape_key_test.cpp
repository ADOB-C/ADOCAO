// `render/ShapeKey.hpp` 的单测：形状表的量化档位与键打包。
//
// 这张表**装不下**时的后果极其不对称：纹理建不出来 → texelFetch 全 0 → 每块砖塌陷 → 黑屏，
// 而且不报错（Issue #2）。所以这里把三条承重性质钉死：
//   ① 档位 1（0.01°）= 改造前的算式**逐位相同**（否则像素门槛会动）；
//   ② 键打包在 ±180° × 2(mid) × 8(style) 全定义域内**无碰撞**；
//   ③ 阶梯严格递增、末档 90° 时的形状种数上界装得进**最小**的容量（阶梯一定收敛）。

#include "render/ShapeKey.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

using namespace adofai::shapekey;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

// 逐位比较（-0.0f 与 0.0f 也算不同：宁可严格）
bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// 改造前的两个算式（照抄 7.2.0 的 `TileMesh.cpp` build() 与 `TileMesh.hpp` packShapeKey）
int oldAngleKey(float deg) { return (int)std::round(deg * 100.0f); }
uint64_t oldPackKey(int sa, int ea, bool mid, uint8_t style) {
    const uint32_t qa = (uint32_t)(int32_t)sa & 0xFFFFFu;
    const uint32_t qe = (uint32_t)(int32_t)ea & 0xFFFFFu;
    return (uint64_t)qa | ((uint64_t)qe << 20) | (mid ? (1ull << 40) : 0ull) |
           ((uint64_t)(style & 0x7u) << 41);
}

}  // namespace

int main() {
    // ---- ① 档位 1 = 旧算式，逐位相同 ----
    {
        size_t n = 0;
        for (int i = -18000; i <= 18000; i++) {            // 整个 0.01° 定义域
            const float a = (float)i / 100.0f;
            if (angleKey(a, 1) != oldAngleKey(a)) { check(false, "档位 1 的角度键变了"); return 1; }
            const float back = angleFromKey(angleKey(a, 1), 1);
            const float want = (float)oldAngleKey(a) / 100.0f;
            if (!sameBits(back, want)) { check(false, "档位 1 的还原角度逐位变了"); return 1; }
            n++;
        }
        // 非整角度（真实谱面的方向是 atan2 出来的）也要逐位相同
        for (int i = 0; i < 20000; i++) {
            const float a = -180.0f + 360.0f * (float)i / 20000.0f + 0.000123f;
            if (angleKey(a, 1) != oldAngleKey(a)) { check(false, "档位 1 的键在非整角度上变了"); return 1; }
            if (!sameBits(angleFromKey(angleKey(a, 1), 1), (float)oldAngleKey(a) / 100.0f)) {
                check(false, "档位 1 的还原在非整角度上逐位变了");
                return 1;
            }
        }
        std::printf("ok   档位 1 与旧算式逐位相同（%zu 个定义域角度 + 20000 个非整角度）\n", n);
    }

    // ---- ② 键打包：全定义域无碰撞；且档位 >= 27 之外仍装得进 20 位 ----
    {
        std::set<uint64_t> keys;
        size_t expect = 0;
        for (int sa = -18000; sa <= 18000; sa++) {
            for (int ea = -18000; ea <= 18000; ea += 997) {    // 逐对全扫是 1.3e9，抽样足够
                const uint64_t k = packKey(sa, ea, true, 3);
                if (k != oldPackKey(sa, ea, true, 3)) { check(false, "packKey 与旧公式不同"); return 1; }
                keys.insert(k);
                expect++;
            }
        }
        if (keys.size() != expect) { check(false, "packKey 有碰撞"); return 1; }
        // 20 位字段：全定义域必须装得下（超出会被掩码截断 → 假碰撞 → 画错砖）
        for (int q = 1; q < kQuantumCount; q++) {
            for (int i = -18000; i <= 18000; i++) {
                const int k = angleKey((float)i / 100.0f, kQuantumLadder[q]);
                if (k < -524288 || k > 524287) { check(false, "量化键超出 20 位"); return 1; }
            }
        }
        std::printf("ok   packKey 无碰撞（%zu 个键），各档位量化键都在 20 位内\n", expect);
    }

    // ---- ③ 阶梯：严格递增、收敛到 90°，且末档的形状种数装得进最小容量 ----
    {
        for (int i = 1; i < kQuantumCount; i++) {
            if (kQuantumLadder[i] <= kQuantumLadder[i - 1]) {
                check(false, "量化阶梯不是严格递增");
                return 1;
            }
        }
        const int last = kQuantumLadder[kQuantumCount - 1];
        if (last != 9000) { check(false, "末档不是 90°（阶梯不保证收敛）"); return 1; }
        std::set<int> distinct;
        for (int i = -18000; i <= 18000; i++) distinct.insert(angleKey((float)i / 100.0f, last));
        // 末档每种角度最多 5 个取值（±90/±45/0）→ (sa, ea) 至多 25 种；× mid 2 × style 8 = 400
        const size_t worst = distinct.size() * distinct.size() * 2 * 8;
        // 驱动把 GL_MAX_TEXTURE_SIZE 报到下限 64 行时，CPU 回退路（142 texel/形状）仍有 461 个位置
        const size_t smallest = capacity(64, 142, 1024);
        if (worst > smallest) { check(false, "末档仍可能装不下（阶梯不收敛）"); return 1; }
        // 误差：还原角度偏离原角度不超过半个量子
        for (int qi = 0; qi < kQuantumCount; qi++) {
            const int q = kQuantumLadder[qi];
            const float half = (float)q * 0.01f * 0.5f + 1e-3f;
            for (int i = -18000; i <= 18000; i += 7) {
                const float a = (float)i / 100.0f;
                if (std::fabs(angleFromKey(angleKey(a, q), q) - a) > half) {
                    check(false, "还原角度超出半个量子");
                    return 1;
                }
            }
        }
        std::printf("ok   阶梯 %d 档收敛到 %.0f°；末档最坏 %zu 种形状 ≤ 最小容量 %zu\n", kQuantumCount,
                    (double)last / 100.0, worst, smallest);
    }

    // ---- ④ style 字段只有 3 位：**颜色绝不能编进键**（0x000000..0xFFFFFF 装不下）----
    // 这条是"特征化测试"：它把键里 style 的宽度钉住 —— 谁哪天为了按颜色缓存几何而把颜色塞进
    // style（16.7M 个值），键会静默碰撞成"画错砖"；要扩宽就必须同时改这里、容量推理与文档。
    {
        for (int st = 0; st < 8; st++)
            for (int st2 = st + 1; st2 < 8; st2++)
                if (packKey(0, 0, false, (uint8_t)st) == packKey(0, 0, false, (uint8_t)st2)) {
                    check(false, "style 字段在 0..7 内就有碰撞");
                    return 1;
                }
        if (packKey(0, 0, false, 8) != packKey(0, 0, false, 0)) {
            check(false, "style 字段不再是 3 位（文档/容量推理要同步改）");
            return 1;
        }
        std::printf("ok   style 字段 3 位：0..7 无碰撞、8 与 0 重合（颜色不许进键）\n");
    }

    // ---- ⑤ 容量口径（与日志里的数字同源，改常数时这里会响）----
    {
        if (capacity(4096, 29, 1024) != 144631) { check(false, "GPU 路容量口径变了"); return 1; }
        if (capacity(4096, 142, 1024) != 29537) { check(false, "CPU 回退路容量口径变了"); return 1; }
        if (capacity(0, 29, 1024) != 0 || capacity(4096, 0, 1024) != 0) {
            check(false, "非法参数没有返回 0");
            return 1;
        }
        // 真实谱面实测：MYC 294 / Singularity 9349 / The Moon 215 个形状 → 档位 1 就装得下
        // （容量如果掉到 9349 以下，正常谱面也会被放粗精度 —— 那是**回归**）
        if (capacity(4096, 29, 1024) < 9349) { check(false, "容量比真实谱面的形状数还小"); return 1; }
        std::printf("ok   容量：GPU 4096 行 = 144631 个形状，CPU 回退 4096 行 = 29537 个\n");
    }

    if (g_fail) {
        std::printf("shape_key: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("shape_key: 全部通过\n");
    return 0;
}
