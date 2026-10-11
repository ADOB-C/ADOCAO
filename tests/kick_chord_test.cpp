// "拿 Kick 打出一个和弦"的自测：这是**打拍音**那条路能听见和弦的唯一办法。
//
// 为什么：每砖放的是同一段打击音色（Kick），它的音高不由音色决定，而由**重复频率**决定
// （137 下/秒 = 137 Hz 的"蚊子声"就是这个道理）。所以要让耳朵听到 Do-Mi-Gol：
//   把击打时刻放在三个频率网格的**并集**上 —— 三条脉冲列相加 = 三条谱线 = 和弦。
//
// 时刻怎么拧出来（`Timeline.cpp:63-83`，CW 时）：
//     delta    = (angleData[i-1] + 180 - angleData[i]) mod 360
//     duration = (delta/360) x 2 x (60/BPM)          （delta≈0 特例成 360 → 两拍）
// 于是**角度序列就是相位的累积** —— 这正是"别人那几张能听到和弦的谱"的砖路为什么
// 画成一圈圈花环（方向转得快 = 音高高，转得慢 = 低音）。
// 顺带一个反直觉的实测结论：想让砖路**舒展**（像标准傅里叶谱），要**把 BPM 调低** ——
// 转角 = 3 x BPM x 该砖时长，BPM 越低每砖转得越少、半径越大；BPM 30000 时最大间隙要转
// 344°，方向几乎每砖翻一圈，画出来是一坨实心饼（实测过）。
//
// 本用例验三件事：
//   ① 击打时刻确实落在并集网格上（0.1 ms 级）；
//   ② 混音后 Do/Mi/Sol 三条谱线**同时**远高于本地中位数（= 真的听到和弦，不是单音）；
//   ③ 能导出 WAV 给耳朵复核。
//
// 用法：adocao_kick_chord_test <和弦谱> <hitsound 目录> [<输出.wav>]

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "audio/HitsoundManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

double goertzel(const float* buf, size_t frames, int channels, int sr, double freq,
                double t0, double t1) {
    const size_t i0 = (size_t)(t0 * sr), i1 = std::min((size_t)(t1 * sr), frames);
    if (i1 <= i0 + 8) return 0.0;
    const size_t n = i1 - i0;
    const double w = 2.0 * 3.14159265358979323846 * freq / (double)sr;
    double re = 0.0, im = 0.0, norm = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double)k / (double)n);
        const double s = (double)buf[(i0 + k) * (size_t)channels] * hann;
        re += s * std::cos(w * (double)k);
        im += s * std::sin(w * (double)k);
        norm += hann;
    }
    return norm > 0.0 ? 2.0 * std::sqrt(re * re + im * im) / norm : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string chartPath = argc > 1 ? argv[1] : "tests/charts/diag_chord.adofai";
    const std::string hsDir     = argc > 2 ? argv[2] : "assets/hitsounds";
    const std::string wavPath   = argc > 3 ? argv[3] : "";

    const double notes[3] = {261.63, 329.63, 392.00};      // C4 E4 G4
    const double seconds = 2.5, minGap = 0.00035;

    LevelData lv;
    if (!lv.loadFromFile(chartPath)) {
        std::printf("FAIL 加载 %s\n", chartPath.c_str());
        return 1;
    }
    Timeline tl;
    tl.build(lv, false);
    const std::vector<double>& st = tl.tileStartTimes();

    // ---- ① 击打时刻 = 三个频率网格的并集 ----
    std::vector<double> target;
    for (double f : notes) {
        for (int k = 0; (double)k / f <= seconds; ++k) target.push_back((double)k / f);
    }
    std::sort(target.begin(), target.end());
    std::vector<double> keep;
    for (double t : target)
        if (keep.empty() || t - keep.back() >= minGap) keep.push_back(t);
    const double t0 = keep.empty() ? 0.0 : keep[0];

    // "真正会响的那些时刻" = 非 None 分组的时刻（细分相位的静音砖不进这组）
    std::vector<double> audible;
    for (const HitsoundTimestampGroup& g : tl.getHitsoundTimestampGroups())
        if (g.type != "None" && !g.type.empty())
            audible.insert(audible.end(), g.timestamps.begin(), g.timestamps.end());
    std::sort(audible.begin(), audible.end());

    size_t pairs = 0;
    double maxErr = 0.0;
    for (size_t k = 0; k < audible.size() && k < keep.size(); ++k) {
        const double got = audible[k] - audible[0];
        const double want = keep[k] - t0;
        maxErr = std::fmax(maxErr, std::fabs(got - want));
        ++pairs;
    }
    std::printf("     并集点 %zu，出声砖 %zu，总砖 %zu，最大时刻误差 %.1f us\n", keep.size(),
                audible.size(), lv.angleData.size(), maxErr * 1e6);
    if (!audible.empty())
        std::printf("     首击 %.4f s，末击 %.4f s\n", audible.front(), audible.back());
    check(audible.size() == keep.size(), "会响的击数 = 并集点数（一击不多一击不少）");
    check(pairs > 1500, "配上的击打数够");
    check(maxErr < 0.00005, "每一击都落在和弦的并集网格上（< 50 us）");
    // 砖路"舒展"的判据：**每砖转角小**（半径 = 1/(2 sin(θ/2))，转 10° 才有几十砖的半径；
    // 转 300° 就成了一坨实心饼）。转角 = 3 x BPM x 该砖时长，所以是**低 BPM** 让它舒展的。
    // 注意转角不是"两个存储角度之差"：`Timeline.cpp:63` 是
    //     delta = fmod(angle[i-1] + 180 - angle[i], 360)
    // 存储角每砖都会加 180，所以差值恒在 180 附近，真正的"转了多少"是它偏离 180 的量。
    double maxTurn = 0.0;
    for (size_t i = 1; i < lv.angleData.size(); ++i) {
        double d = std::fmod(lv.angleData[i - 1] + 180.0 - lv.angleData[i], 360.0);
        if (d < 0.0) d += 360.0;
        maxTurn = std::fmax(maxTurn, d);
    }
    std::printf("     每砖最大转角 %.1f 度\n", maxTurn);
    check(maxTurn < 15.0, "每砖转角 < 15° → 砖路舒展（展开的傅里叶谱）");
    check(lv.settings.bpm < 5000.0,
          "BPM 取得低 → 每砖转角小 → 砖路舒展（提高 BPM 反而卷成一坨实心饼）");

    // ---- ② 混音后确实是三音同响 ----
    HitsoundManager hm;
    hm.init(hsDir);
    hm.setEnabled(true);
    const bool mixed = hm.preSynthesize(tl.getHitsoundTimestampGroups(),
                                       (float)tl.totalDuration());
    check(mixed, "Kick 混音成功");
    if (!mixed) return 1;
    std::printf("     混音 %d 击 / %.2f s 缓冲（%zu 帧 @ %d Hz）\n", hm.lastMixedHits(),
                tl.totalDuration(), hm.totalFrames(), hm.sampleRate());

    const float* buf = hm.buffer();
    const int sr = hm.sampleRate();
    const size_t frames = hm.totalFrames();
    const double w0 = 0.8, w1 = 1.8;                    // 取中段 1 s
    double noteE[3];
    for (int i = 0; i < 3; ++i) noteE[i] = goertzel(buf, frames, 2, sr, notes[i], w0, w1);
    // 本地中位数（对数分布取 60 个频率）当本底：三条谱线必须远高于它
    std::vector<double> bg;
    for (int i = 0; i < 60; ++i) {
        const double f = 120.0 * std::pow(1600.0 / 120.0, (double)i / 59.0);
        bg.push_back(goertzel(buf, frames, 2, sr, f, w0, w1));
    }
    std::sort(bg.begin(), bg.end());
    const double median = bg[bg.size() / 2];
    std::printf("     能量 Do %.5f  Mi %.5f  Sol %.5f | 本底中位 %.5f\n", noteE[0], noteE[1],
                noteE[2], median);
    check(noteE[0] > median * 6.0, "Do（261.63 Hz）高于本底中位数 6 倍以上");
    check(noteE[1] > median * 6.0, "Mi（329.63 Hz）高于本底中位数 6 倍以上");
    check(noteE[2] > median * 6.0, "Sol（392.00 Hz）高于本底中位数 6 倍以上");
    const double lo = std::fmin(noteE[0], std::fmin(noteE[1], noteE[2]));
    const double hi = std::fmax(noteE[0], std::fmax(noteE[1], noteE[2]));
    check(hi / lo < 4.0, "三个音能量同量级 → 是**同时**响的和弦");

    if (!wavPath.empty()) {
        const bool w = hm.writeWav(wavPath);
        std::printf("     %s %s\n", w ? "已写出" : "写不出", wavPath.c_str());
        check(w, "能导出 WAV（给耳朵复核用）");
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
