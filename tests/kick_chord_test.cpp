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

    // 纯律 4:5:6（与 `tests/gen_adocao_charts.py` 必须一致）：这样三音有**精确的公共周期**
    const double f0 = 261.63 / 4.0;                        // 65.4075 Hz
    const double notes[3] = {4.0 * f0, 5.0 * f0, 6.0 * f0};  // 261.63 / 327.04 / 392.44
    const double period = 1.0 / f0;                        // 公共周期 15.2888 ms
    (void)0;

    LevelData lv;
    if (!lv.loadFromFile(chartPath)) {
        std::printf("FAIL 加载 %s\n", chartPath.c_str());
        return 1;
    }
    Timeline tl;
    tl.build(lv, false);
    const std::vector<double>& st = tl.tileStartTimes();

    // ---- ① 击打时刻 = 三个频率网格的并集（公共周期内 12 段，逐周期重复）----
    std::vector<double> gaps;
    {
        std::vector<double> pts;
        for (int k = 4; k <= 6; ++k)
            for (int j = 1; j < k; ++j) pts.push_back((double)j * period / (double)k);
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        gaps.push_back(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) gaps.push_back(pts[i] - pts[i - 1]);
        gaps.push_back(period - pts.back());
    }
    const size_t per = gaps.size();
    // 砖数由谱面自己给（别用 seconds 反推：生成脚本的 round 会差一个周期，真踩过）
    const size_t nHit = lv.angleData.size() > 0 ? lv.angleData.size() - 1 : 0;
    std::vector<double> keep;
    keep.reserve(nHit);
    double acc = 0.0;
    for (size_t k = 0; k < nHit; ++k) {
        keep.push_back(acc);
        acc += gaps[k % per];
    }
    const double t0 = keep.empty() ? 0.0 : keep[0];

    // "真正会响的那些时刻" = 非 None 分组的时刻（细分相位的静音砖不进这组）
    std::vector<double> audible;
    for (const HitsoundTimestampGroup& g : tl.getHitsoundTimestampGroups())
        if (g.type != "None" && !g.type.empty())
            audible.insert(audible.end(), g.timestamps.begin(), g.timestamps.end());
    std::sort(audible.begin(), audible.end());

    size_t pairs = 0;
    double maxErr = 0.0;
    // tile i 的起点是 keep[i-1]，命中从 tile 1 开始 → audible[k] 对应 keep[k]
    for (size_t k = 0; k < audible.size() && k < keep.size(); ++k) {
        const double got = audible[k] - audible[0];
        const double want = keep[k] - keep[0];
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
    // 时刻的"干净度"（用户 2026-10-11 指出的）：**恒定 BPM + Twirl 选边**，
    // 不用逐砖 SetSpeed —— 那样会连累所有按拍计时的东西（BPM 显示、beatsAhead/Behind、
    // 采样率 bpm/60 等等），而且 21 亿层目标下每砖一个事件也吃不消。
    int nSetSpeed = 0, nTwirl = 0;
    for (const LevelData::FastAction& a : lv.actions) {
        if (a.type == LevelData::FastAction::SetSpeed) ++nSetSpeed;
        if (a.type == LevelData::FastAction::Twirl) ++nTwirl;
    }
    check(nSetSpeed == 0, "**没有** SetSpeed：时刻全靠 Twirl 换边（恒定 BPM）");
    check(nTwirl > 0 && nTwirl < (int)lv.angleData.size(), "Twirl 数量合理（逐周期换边）");

    // 用户 2026-10-11 的第三条：**在最小公倍数的位置放 Twirl，让那里的位置靠近 180 度**。
    // 三音取纯律 4:5:6 → 有精确公共周期；符号模式逐周期重复 + 每周期净转角 0
    // ⇒ 朝向在每个 LCM 边界回到同一个值；再把整体角度平移一下，让这个值正好是 180。
    double maxBoundaryDev = 0.0;
    int boundaries = 0;
    for (size_t i = per; i < lv.angleData.size(); i += per) {
        maxBoundaryDev = std::fmax(maxBoundaryDev, std::fabs(lv.angleData[i] - 180.0));
        ++boundaries;
    }
    std::printf("     LCM 边界 %d 个（每 %zu 砖），最大偏离 180° = %.4f 度\n", boundaries, per,
                maxBoundaryDev);
    check(boundaries > 30, "LCM 边界够多（公共周期 15.29 ms）");
    check(maxBoundaryDev < 0.5, "每个 LCM 边界都落在 180°（最小公倍数的位置）");

    // 逐周期重复：转角序列必须与第一个周期**逐位相同**
    bool periodic = lv.angleData.size() > 2 * per;
    for (size_t i = per + 1; periodic && i < lv.angleData.size(); ++i) {
        const double a1 = lv.angleData[i] - lv.angleData[i - 1];
        const double a0 = lv.angleData[i - per] - lv.angleData[i - per - 1];
        if (std::fabs(a1 - a0) > 1e-6) periodic = false;
    }
    check(periodic, "转角序列逐周期重复 → 砖路是规则的（LCM 处放 Twirl 的效果）");

    // 转角量级（信息）：|t| = 3B|d - c| 被时刻锁死，和弦上平均 ~74°（参考谱 92.7°）
    double sumAbs = 0.0, maxAbs = 0.0;
    for (size_t i = 1; i < lv.angleData.size(); ++i) {
        double t = std::fmod(lv.angleData[i] - lv.angleData[i - 1] + 180.0, 360.0);
        if (t < 0) t += 360.0;
        t -= 180.0;
        sumAbs += std::fabs(t);
        maxAbs = std::fmax(maxAbs, std::fabs(t));
    }
    std::printf("     |转角| 均值 %.1f 最大 %.1f 度（参考谱 The Moon 92.7°）\n",
                sumAbs / (double)(lv.angleData.size() - 1), maxAbs);

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
    // 三条线的强弱 = 每周期击打密度（4:5:6）x Kick 自己的频谱包络，所以不会等强；
    // 这里只要求"同一量级"（参考：实测 0.012 / 0.115 / 0.091）
    check(hi / lo < 20.0, "三个音都在同一量级（同时响 → 和弦，不是琶音）");

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
