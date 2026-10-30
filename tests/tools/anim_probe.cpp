// 轨道动画探针（**诊断工具，不进 ctest、不随产品发布**）：
//   ① 瞬跳普查：逐 tick 采样 `TileAnimation::sample()`，看砖到底动没动、有没有"一步几十上百格"的阶跃
//      —— 用来回答"行星跟随 MoveTrack 之后拖尾那条直线是真实语义还是我们算错"。
//   ② 成本：模拟拖尾采样（800 Hz × 0.74 s），每点算当前砖 + 前后邻居的活位置。
//
// 复现口径（**这套是可复现的**）：`sample()`/`moveTransformAt()` 都是关于 (tile, t) 的纯函数，
// 采样网格固定、谱面固定 → 同一份二进制在本机重跑，① 的输出**逐字节相同**。
// 例外只有 ②：那是墙钟计时，只能当量级参考（每次跑都会变，且受机器负载/温度影响）。
//
// 用法：
//   anim_probe <谱.adofai>                  # ① 抽样 300 块动画砖（2 ms 步进）+ ② 成本
//   anim_probe <谱.adofai> --all            # ① 扫**全部**动画砖（慢，大谱十几秒）
//   anim_probe <谱.adofai> --step 0.001     # 改采样步长（默认 0.002 s）
//   anim_probe <谱.adofai> --quick          # 只报"砖会不会动"（10 ms 步进、60 块砖）
#include "core/level/LevelData.hpp"
#include "core/timeline/TileAnimation.hpp"
#include "core/timeline/Timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {

double msNow() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Census {
    size_t steps = 0;          // 相邻两帧都拿到样值、可以比较的步数
    size_t jumpSteps = 0;      // 单步位移 > 0.1 砖
    size_t movedTiles = 0;     // 真的动过（位移/旋转/缩放任一变化）的砖
    size_t scanned = 0;        // 扫了多少块砖
    double worstPos = 0.0, worstRot = 0.0, worstScale = 0.0;
    int64_t worstTile = -1;
    double worstT = 0.0;
    double maxAbsPos = 0.0, maxAbsRot = 0.0, minScale = 2.0, maxScaleSeen = 0.0;
};

// 遍历选中的砖，按 `step` 扫整条谱；`reportWorst` 时把最坏那一处的前后值打出来（便于判定是不是
// `duration: 0` 的瞬跳：典型形态是"静止 (0,0) → 一步跳到远处"）。
Census census(const TileAnimation& anim, const Timeline& tl, double step, bool all, size_t sampleMax,
              int64_t onlyTile) {
    Census c;
    const double total = tl.totalDuration();
    std::vector<int64_t> tiles;
    if (onlyTile >= 0) {
        tiles.push_back(onlyTile);
    } else if (all) {
        tiles.reserve(anim.entries().size());
        for (const auto& e : anim.entries()) tiles.push_back(e.tile);
    } else {
        const size_t want = std::min<size_t>(sampleMax, anim.entries().size());
        tiles.reserve(want);
        for (size_t k = 0; k < want; k++)
            tiles.push_back(anim.entries()[anim.entries().size() * k / std::max<size_t>(want, 1)].tile);
    }
    c.scanned = tiles.size();
    for (int64_t tile : tiles) {
        TileAnimSample prev{};
        bool have = false, moved = false;
        for (double t = 0.0; t <= total; t += step) {
            TileAnimSample a{};
            if (!anim.sample(tile, t, a)) { have = false; continue; }
            c.maxAbsPos = std::max(c.maxAbsPos, std::hypot((double)a.posX, (double)a.posY));
            c.maxAbsRot = std::max(c.maxAbsRot, std::fabs((double)a.rot));
            c.minScale = std::min(c.minScale, std::min((double)a.scaleX, (double)a.scaleY));
            c.maxScaleSeen = std::max(c.maxScaleSeen, std::max((double)a.scaleX, (double)a.scaleY));
            if (have) {
                c.steps++;
                const double dp = std::hypot((double)a.posX - prev.posX, (double)a.posY - prev.posY);
                const double dr = std::fabs((double)a.rot - prev.rot);
                const double ds = std::max(std::fabs((double)a.scaleX - prev.scaleX),
                                           std::fabs((double)a.scaleY - prev.scaleY));
                if (dp > 1e-6 || dr > 1e-6 || ds > 1e-6) moved = true;
                if (dp > 0.1) c.jumpSteps++;
                c.worstRot = std::max(c.worstRot, dr);
                c.worstScale = std::max(c.worstScale, ds);
                if (dp > c.worstPos) {
                    c.worstPos = dp;
                    c.worstTile = tile;
                    c.worstT = t;
                    if (onlyTile >= 0)
                        std::printf("   [最坏] t=%.4f→%.4f  dpos=%.4f  before(%.4f,%.4f) after(%.4f,%.4f) "
                                    "rot %.4f→%.4f scale %.3f→%.3f\n",
                                    t - step, t, dp, (double)prev.posX, (double)prev.posY,
                                    (double)a.posX, (double)a.posY, (double)prev.rot, (double)a.rot,
                                    (double)prev.scaleX, (double)a.scaleX);
                }
            }
            prev = a;
            have = true;
        }
        if (moved) c.movedTiles++;
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <谱.adofai> [--all] [--step S] [--quick] [--tile N]\n", argv[0]);
        return 2;
    }
    bool all = false, quick = false;
    double step = 0.002;
    int64_t onlyTile = -1;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--all")) all = true;
        else if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--step") && i + 1 < argc) step = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--tile") && i + 1 < argc) onlyTile = std::atoll(argv[++i]);
    }

    LevelData lv;
    if (!lv.loadFromFile(argv[1])) { std::printf("读不了 %s\n", argv[1]); return 1; }
    Timeline tl;
    tl.build(lv, false);
    TileAnimation anim;
    const double tb0 = msNow();
    anim.build(lv, tl);
    const double tb1 = msNow();

    std::printf("# 谱面 %s\n", argv[1]);
    std::printf("# 砖 %lld / 有动画的砖 %zu / 关键帧 %zu / 建帧 %.1f ms（墙钟）%s\n",
                (long long)tl.tileCount(), anim.animatedTiles(), anim.keyframeCount(), tb1 - tb0,
                anim.truncated() ? "（被上限截断）" : "");
    if (anim.entries().empty()) {
        std::printf("# 这张谱没有任何轨道动画 → 行星跟不跟都一样\n");
        return 0;
    }

    if (quick) {
        const Census c = census(anim, tl, 0.010, false, 60, -1);
        std::printf("quick: 扫 %zu 砖 / 动过 %zu / 阶跃 %zu / Δpos %.3f / Δrot %.4f / Δscale %.4f\n",
                    c.scanned, c.movedTiles, c.jumpSteps, c.worstPos, c.worstRot, c.worstScale);
        return 0;
    }

    const size_t sampleMax = all ? 0 : 300;
    const Census c = census(anim, tl, step, all, sampleMax, -1);
    std::printf("① 瞬跳普查（整条谱 %.2f s / 步长 %.0f ms / 扫 %zu 块动画砖%s）\n",
                tl.totalDuration(), step * 1000.0, c.scanned, all ? "，--all 全扫" : "（抽样）");
    std::printf("   步数 %zu；动过的砖 %zu/%zu；>0.1 砖/步 的阶跃 %zu\n", c.steps, c.movedTiles,
                c.scanned, c.jumpSteps);
    std::printf("   最大单步位移 %.6f 砖（砖 %lld @ t=%.4f）；最大单步旋转 %.6f rad；"
                "最大单步缩放 %.6f\n", c.worstPos, (long long)c.worstTile, c.worstT, c.worstRot,
                c.worstScale);
    std::printf("   动画幅度：|pos| ≤ %.6f 砖，|rot| ≤ %.6f rad，scale ∈ [%.4f, %.4f]\n",
                c.maxAbsPos, c.maxAbsRot, c.minScale, c.maxScaleSeen);

    // 最坏那一处的前后值（判定 duration:0 瞬跳的形态）
    if (c.worstTile >= 0) {
        std::printf("   ——最坏处明细（砖 %lld）——\n", (long long)c.worstTile);
        census(anim, tl, step, false, 0, c.worstTile);
    }

    // ② 成本：量级参考，**墙钟不可复现**
    const int samples = 592;   // 0.74 s × 800 Hz
    const int64_t mid = anim.entries()[anim.entries().size() / 2].tile;
    auto bench = [&](int reps) {
        volatile double sink = 0;
        const double t0 = msNow();
        for (int r = 0; r < reps; r++) {
            const double base = tl.startTimeAt(std::max<int64_t>(mid - 8, 0));
            for (int s = 0; s < samples; s++) {
                const double t = base + (double)s * 0.00125;
                for (int64_t idx = mid - 1; idx <= mid + 1; idx++) {
                    TileAnimSample a{};
                    if (anim.sample(idx, t, a)) sink += a.posX;
                    sink += lv.moveTransformAt(idx, t).moveX;
                }
            }
        }
        return std::make_pair((msNow() - t0) / reps, (double)sink);
    };
    bench(3);
    const auto [ms, sink] = bench(20);
    std::printf("② 成本（**墙钟，不可复现**，只当量级）：%d 采样点 × 3 砖 × (sample+moveTransformAt)"
                " = %.3f ms/帧\n", samples, ms);
    (void)sink;
    return 0;
}
