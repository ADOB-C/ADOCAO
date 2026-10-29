// `render/ShapeKey.hpp` 的单测：形状表的量化档位与键打包。
//
// 这张表**装不下**时的后果极其不对称：纹理建不出来 → texelFetch 全 0 → 每块砖塌陷 → 黑屏，
// 而且不报错（Issue #2）。所以这里把三条承重性质钉死：
//   ① 100 档（0.01°）= 7.2.0 的算式**逐位相同**（改基准精度时这条钉住"老档位没动"）；
//   ② 键打包在 sa ∈ [-180,180] / ea ∈ [0,360] 全定义域内**无碰撞**（最细档也不越 24 位）；
//   ③ 阶梯严格递减到 1°（1° 封底：更粗的档位没有意义），且最细档落在 float32 精度附近。

#include "render/ShapeKey.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>
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

// 改造前的两个算式（照抄 7.2.0 的 `TileMesh.cpp` build() 与 `TileMesh.hpp` packShapeKey；
// 0.01° = 100 档，20 位字段 → 现在的 24 位字段在 0.01° 下必须给出**同样的无碰撞性**）
constexpr int kLegacySteps = 100;                 // 0.01°
int oldAngleKey(float deg) { return (int)std::round(deg * 100.0f); }
uint64_t oldPackKey(int sa, int ea, bool mid, uint8_t style) {
    const uint32_t qa = (uint32_t)(int32_t)sa & 0xFFFFFu;
    const uint32_t qe = (uint32_t)(int32_t)ea & 0xFFFFFu;
    return (uint64_t)qa | ((uint64_t)qe << 20) | (mid ? (1ull << 40) : 0ull) |
           ((uint64_t)(style & 0x7u) << 41);
}

}  // namespace

int main() {
    // ---- ① 100 档（0.01°）= 7.2.0 的算式，逐位相同 ----
    {
        size_t n = 0;
        for (int i = -18000; i <= 18000; i++) {            // 整个 0.01° 定义域
            const float a = (float)i / 100.0f;
            if (angleKey(a, kLegacySteps) != oldAngleKey(a)) {
                check(false, "0.01° 档的角度键变了");
                return 1;
            }
            if (!sameBits(angleFromKey(angleKey(a, kLegacySteps), kLegacySteps),
                          (float)oldAngleKey(a) / 100.0f)) {
                check(false, "0.01° 档的还原角度逐位变了");
                return 1;
            }
            n++;
        }
        // 非整角度（真实谱面的方向是 atan2 出来的）也要逐位相同
        for (int i = 0; i < 20000; i++) {
            const float a = -180.0f + 360.0f * (float)i / 20000.0f + 0.000123f;
            if (angleKey(a, kLegacySteps) != oldAngleKey(a)) {
                check(false, "0.01° 档的键在非整角度上变了");
                return 1;
            }
            if (!sameBits(angleFromKey(angleKey(a, kLegacySteps), kLegacySteps),
                          (float)oldAngleKey(a) / 100.0f)) {
                check(false, "0.01° 档的还原在非整角度上逐位变了");
                return 1;
            }
        }
        // 最细档必须明显细于 0.01°（用户 2026-10-28："0.01° 太看不起人家了"）
        if (kStepsLadder[0] < 4096) { check(false, "最细档比 1/4096° 还粗"); return 1; }
        std::printf("ok   0.01°（100 档）与旧算式逐位相同（%zu 个定义域角度 + 20000 个非整角度），"
                    "最细档 1/%d°\n", n, kStepsLadder[0]);
    }

    // ---- ② 键打包：全档位都在 24 位内、且 (qsa, qea, mid, style) → 键 无碰撞 ----
    {
        // 最细档的极值：sa ∈ [-180, 180]、ea ∈ [0, 360]（angleData 的方向是 0..360，sa = pred-180）
        const int finest = kStepsLadder[0];
        const long lo = (long)angleKey(-180.0f, finest);
        const long hi = (long)angleKey(360.0f, finest);
        if (lo < -8388608L || hi > 8388607L) {
            check(false, "最细档的量化键越了 24 位（会被掩码截断 → 假碰撞 → 画错砖）");
            return 1;
        }
        // 每个档位各抽 20000 个角度：**(元组去重后的个数) 必须等于 (键去重后的个数)** ——
        // 粗档位下很多砖会落到同一格（那是正常的合并），碰撞指的是"不同元组 → 同一个键"。
        for (int li = 0; li < kStepsCount; li++) {
            const int steps = kStepsLadder[li];
            std::set<std::tuple<int, int, int, int>> tuples;
            std::set<uint64_t> keys;
            for (int i = 0; i < 20000; i++) {
                const int qsa = angleKey(-180.0f + 360.0f * (float)i / 20000.0f, steps);
                const int qea = angleKey(360.0f * (float)i / 20000.0f, steps);
                const int st = i & 7;
                for (int mid = 0; mid < 2; mid++) {
                    tuples.insert({qsa, qea, mid, st});
                    keys.insert(packKey(qsa, qea, mid != 0, (uint8_t)st));
                }
            }
            if (keys.size() != tuples.size()) {
                std::printf("     档位 1/%d°：元组 %zu 个 → 键只有 %zu 个\n", steps, tuples.size(),
                            keys.size());
                check(false, "packKey 有碰撞");
                return 1;
            }
        }
        // 边界组合也要分开：最细档的四个极值角 × mid × style
        std::set<uint64_t> edges;
        const int e[2] = {(int)lo, (int)hi};
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++)
                for (int mid = 0; mid < 2; mid++)
                    for (int st = 0; st < 8; st++) edges.insert(packKey(e[a], e[b], mid != 0, (uint8_t)st));
        if (edges.size() != 64) { check(false, "最细档的边界键有碰撞"); return 1; }
        std::printf("ok   各档位 packKey 无碰撞；最细档极值 [%ld, %ld] 在 24 位内\n", lo, hi);
    }

    // ---- ③ 阶梯：从 float32 级精度严格递减到 1°（1° 封底，不再有更粗的档位）----
    {
        for (int i = 1; i < kStepsCount; i++) {
            if (kStepsLadder[i] * 2 != kStepsLadder[i - 1]) {
                check(false, "量化阶梯不是 ×2 递减");
                return 1;
            }
        }
        const int last = kStepsLadder[kStepsCount - 1];
        if (last != 1) { check(false, "末档不是 1°"); return 1; }
        // 最细档要落在 angleData 的 float32 精度附近：±180° 的 ULP 是 1.5e-5°，
        // 最细档 1/16384° = 6.1e-5° ≈ 4 ULP（再细没有数据可分辨，还会撞 24 位边界）
        const double finest = 1.0 / (double)kStepsLadder[0];
        if (finest > 2e-4 || finest < 1e-5) {
            check(false, "最细档不在 float32 角度分辨率量级");
            return 1;
        }
        // 还原误差不超过半个格子
        for (int qi = 0; qi < kStepsCount; qi++) {
            const int steps = kStepsLadder[qi];
            const float half = 0.5f / (float)steps + 1e-6f;
            for (int i = -18000; i <= 18000; i += 7) {
                const float a = (float)i / 100.0f;
                if (std::fabs(angleFromKey(angleKey(a, steps), steps) - a) > half) {
                    check(false, "还原角度超出半个格子");
                    return 1;
                }
            }
        }
        // 1° 下的 (sa, ea) 组合上界 360×360 = 129,600 × mid 2 × style 6 = 1.56M 形状 —— 用
        // 16384 行（老 GPU 的上限）算下来是 45 万行，装不下**理论**最坏；实测那个随机角度谱
        // 1° 只有 130,321 个形状（= 一端一格），远在 578k 之内。所以这里只断言"1° 的实际落点
        // 远小于理论上界"这件事有记录，不再假装能证明收敛。
        std::printf("ok   阶梯 %d 档：1/%d°（%.3g°）→ 1°（×2 递减，1° 封底）；还原误差 ≤ 半格\n",
                    kStepsCount, kStepsLadder[0], finest);
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
