// "自然弯曲"档的护栏：纯 angleData 的赫兹谱（`diag_arpeggio` / `diag_duet`）。
//
// 这一类谱面成立的三个条件，缺一个就变成"毛线团"或"直线"：
//   ① **零事件**：时长是角度的纯函数（duration = (180-Δa)/(3B)），所以角度自己就能排时刻；
//   ② **载波 ≥ 最大间隙**（一拍 c = 60/BPM）：于是 Δa = 180 x (1 - gap/c) 的符号恒定
//      ⇒ 砖路同向弯 = "自然弯曲"；（载波落在间隙中间时会左右抖，只能靠 Twirl 硬掰）
//   ③ **击打落在音符网格上**：这些谱的音都是 f0 的整数倍（4/5/6 f0），
//      所以每个击打时刻 t 必须满足 `t x 60 f0 ∈ ℤ`（公共格 1/(60 f0) = 0.2548 ms）。
//
// 用法：adocao_hz_grid_test <谱> [<谱2> ...] [--wav 输出.wav] [--hitsounds 目录]
//   --wav：把这张（最后一张）谱的打拍音混成 WAV 存下来 —— 这一族谱的"音高就是击打频率"，
//          想听就得听混音结果（Kick 以音符速率叠出来）。

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "audio/HitsoundManager.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("ok   %s\n", what.c_str());
    } else {
        std::printf("FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

const double kF0 = 261.63 / 4.0;    // 与 tests/gen_adocao_charts.py 一致（纯律 4:5:6 的根音）

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> charts;
    std::string wavPath, hsDir = "assets/hitsounds";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--wav" && i + 1 < argc) { wavPath = argv[++i]; }
        else if (a == "--hitsounds" && i + 1 < argc) { hsDir = argv[++i]; }
        else charts.push_back(a);
    }
    if (charts.empty()) {
        std::printf("用法: adocao_hz_grid_test <谱> [<谱2> ...] [--wav 输出.wav] [--hitsounds 目录]\n");
        return 1;
    }
    for (size_t ci = 0; ci < charts.size(); ++ci) {
        const std::string path = charts[ci];
        LevelData lv;
        if (!lv.loadFromFile(path)) {
            std::printf("FAIL 加载 %s\n", path.c_str());
            ++g_fail;
            continue;
        }
        Timeline tl;
        tl.build(lv, false);
        std::printf("── %s（%zu 砖，BPM %.1f，%zu 个 action）\n", path.c_str(), lv.angleData.size(),
                    (double)lv.settings.bpm, lv.actions.size());

        // ① 零事件
        check(lv.actions.empty(), "  零事件（时刻全靠角度；Twirl/Pause 都不需要）");

        // ② 转角全部同号（自然弯曲）
        int pos = 0, neg = 0;
        double maxAbs = 0.0;
        for (size_t i = 1; i < lv.angleData.size(); ++i) {
            double t = std::fmod(lv.angleData[i] - lv.angleData[i - 1] + 180.0, 360.0);
            if (t < 0) t += 360.0;
            t -= 180.0;
            if (t > 0) ++pos; else if (t < 0) ++neg;
            maxAbs = std::fmax(maxAbs, std::fabs(t));
        }
        std::printf("     转角：正 %d / 负 %d，|max| %.1f°\n", pos, neg, maxAbs);
        check(pos == 0 || neg == 0, "  转角全部同号 → 同向弯（自然弯曲，不来回抖）");
        check(maxAbs < 180.0, "  没有 180° 的掉头（那会变成折返/双线段）");

        // ③ 每个击打时刻都落在音符网格 1/(60 f0) 上
        const std::vector<double>& st = tl.tileStartTimes();
        const std::vector<HitsoundTimestampGroup> groups = tl.getHitsoundTimestampGroups();
        double worst = 0.0;
        size_t hits = 0;
        for (const HitsoundTimestampGroup& g : groups) {
            if (g.type == "None" || g.type.empty()) continue;
            for (double t : g.timestamps) {
                if (t <= 0.0) continue;
                const double grid = t * 60.0 * kF0;
                worst = std::fmax(worst, std::fabs(grid - std::round(grid)));
                ++hits;
            }
        }
        std::printf("     击打 %zu 个（砖 %zu），网格 1/(60f0)=%.4f ms，最大偏离 %.2e 格\n", hits,
                    st.size(), 1e3 / (60.0 * kF0), worst);
        check(hits > 50, "  击打数够");
        check(worst < 1e-3, "  每个击打都落在音符网格上（1/(60 f0)）");

        // 角度 → 时长：逐块算出来的总时长必须与时间轴一致（只比总量，避免逐点对齐写错 —— 
        // 逐点对齐踩过：`st[i]-st[i-1]` 是 tile i-1 的时长，用同下标比会一路错位）。
        double acc = 0.0;
        for (size_t i = 1; i + 1 < lv.angleData.size(); ++i) {
            double delta = std::fmod(lv.angleData[i - 1] + 180.0 - lv.angleData[i], 360.0);
            if (delta < 0) delta += 360.0;
            if (delta < 0.0001) delta = 360.0;
            acc += delta / (3.0 * (double)lv.settings.bpm);
        }
        const size_t last = std::min(st.size(), lv.angleData.size()) - 1;
        const double span = st[last] - st[1];
        std::printf("     角度反推总时长 %.4f ms vs 时间轴 %.4f ms（差 %.2f us）\n", acc * 1e3,
                    span * 1e3, (acc - span) * 1e6);
        check(std::fabs(acc - span) < 5e-5, "  角度 ↔ 时刻 总量自洽（< 50 us）");

        // 想听：把打拍音混成 WAV（音高 = 击打频率，所以必须听混音结果）
        if (!wavPath.empty() && ci + 1 == charts.size()) {
            HitsoundManager hm;
            hm.init(hsDir);
            hm.setEnabled(true);
            const bool mixed = hm.preSynthesize(tl.getHitsoundTimestampGroups(),
                                               (float)tl.totalDuration());
            const bool wrote = mixed && hm.writeWav(wavPath);
            std::printf("     %s %s（%d 击 / %.2f s 缓冲）\n", wrote ? "已写出" : "写不出",
                        wavPath.c_str(), hm.lastMixedHits(), tl.totalDuration());
            check(wrote, "  混音并写出 WAV");
        }
    }
    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
