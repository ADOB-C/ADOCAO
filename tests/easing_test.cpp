// 缓动模块（core/util/Easing）对拍测试。
//
// 金标来源：`~/Repos/Re_ADOJAS/src/lib/Player/Easing.ts` —— 在 25 个 t 采样点上用 node 直接
// import 那份 .ts（类型擦除）算出来的 double，`toPrecision(17)` 落成 `easing_gold.inc`。
// 名字就是契约：谱面 `ease` 字段的字符串直接查表，共 32 个（含 `Linear`/`Unset`）。
//
// 口径：
//   * 名字 → 枚举 → 名字 必须双向还原（`easeName(easeFromName(n)) == n`），未知名字回退 Linear；
//   * 取值容差 1e-12（**相对**）。为什么不是逐位：这里跨了 JS 的 `Math.sin/cos/pow` 与 libm，
//     这几个函数本身允许 1 ulp 级差异（1e-16），而公式里的 +-*/ 是完全一致的。1e-12 足以抓住
//     "公式抄错/分支写反"，又不会被 libm 的 ulp 差异误报；
//   * 另外单独验一条**反向**护栏：`InBack(0.5) < 0` —— 缓动越界是 ADOFAI 的预期行为
//     （回弹/过冲），实现里绝不能在返回前再 clamp 一次。

#include "core/util/Easing.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "easing_gold.inc"

namespace {

int g_fail = 0;

void fail(const char* what, const char* name, double got, double want) {
    if (++g_fail <= 20)
        std::fprintf(stderr, "FAIL %s: %s  got=%.17g want=%.17g\n", what, name, got, want);
}

}  // namespace

int main() {
    using namespace adofai;

    static_assert(sizeof(kEaseGold) / sizeof(kEaseGold[0]) == (size_t)kEaseCount,
                  "金标行数必须与 Ease 枚举数一致（新增/删除缓动时重新生成 easing_gold.inc）");

    // 1) 名字 ↔ 枚举：32 个名字全部认得，且能双向还原。
    for (int i = 0; i < kEaseCount; i++) {
        const Ease e = (Ease)i;
        const char* n = easeName(e);
        if (!n || !*n) { std::fprintf(stderr, "FAIL easeName(%d) 为空\n", i); ++g_fail; continue; }
        if (easeFromName(n) != e) { std::fprintf(stderr, "FAIL 名字还原失败: %s\n", n); ++g_fail; }
    }
    // 金标里出现的每个名字必须真的是"已认得的名字"（拼错会退回 Linear，这里能抓住）。
    for (const auto& row : kEaseGold) {
        const Ease e = easeFromName(row.name);
        if (std::strcmp(easeName(e), row.name) != 0) {
            std::fprintf(stderr, "FAIL 金标名字不认得: %s\n", row.name);
            ++g_fail;
        }
    }

    // 2) 未知 / 空名字 → Linear（Re_ADOJAS 的 `|| Linear` 回退）。
    if (easeFromName("") != Ease::Linear) { std::fprintf(stderr, "FAIL 空名字没回退 Linear\n"); ++g_fail; }
    if (easeFromName("NotARealEase") != Ease::Linear) { std::fprintf(stderr, "FAIL 未知名字没回退 Linear\n"); ++g_fail; }
    if (easeFromName("inSine") != Ease::Linear) { std::fprintf(stderr, "FAIL 大小写不该被容忍\n"); ++g_fail; }

    // 3) 取值对拍。
    for (const auto& row : kEaseGold) {
        const Ease e = easeFromName(row.name);
        double maxDiff = 0;
        for (int k = 0; k < kEaseGoldT; k++) {
            const double t = kEaseGoldTs[k];
            const double got = easeApply(e, t);
            const double want = row.v[k];
            const double diff = std::fabs(got - want);
            if (diff > maxDiff) maxDiff = diff;
            const double tol = 1e-12 * std::fmax(1.0, std::fabs(want));
            if (!(diff <= tol)) fail("取值", row.name, got, want);
        }
        // 留痕：正常应 ≤ 1e-15 量级（libm 的 ulp 差）。
        if (maxDiff > 1e-13)
            std::printf("note %-14s max|diff| = %.3g\n", row.name, maxDiff);
    }

    // 4) 越界是预期行为，实现不许再 clamp。
    if (!(easeApply(Ease::InBack, 0.5) < 0.0)) {
        std::fprintf(stderr, "FAIL InBack(0.5) 应该是负的（过冲），说明被 clamp 了\n");
        ++g_fail;
    }
    if (!(easeApply(Ease::OutBack, 0.5) > 1.0)) {
        std::fprintf(stderr, "FAIL OutBack(0.5) 应该 >1（过冲），说明被 clamp 了\n");
        ++g_fail;
    }

    if (g_fail) { std::fprintf(stderr, "easing: %d 处失败\n", g_fail); return 1; }
    std::printf("easing: 32 个缓动 × %d 个采样点全部对上（容差 1e-12）\n", kEaseGoldT);
    return 0;
}
