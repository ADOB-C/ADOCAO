// audio-as-chart（raw-PCM 直通）自测：整谱**就是**波形 —— 这条路的产物必须真的能听出和弦。
//
// 为什么要有这个用例：
//   * "打拍音"那条路**不可能**出和弦 —— 每砖放的都是同一段打击音色（Kick），
//     音高只由**重复频率**决定，所以 `diag_fourier` 里那个和弦只存在于 angleData 这个
//     信号里（频谱图看得见），耳朵听到的永远是"嗡"。
//   * 要真的听见 Do-Mi-Gol，只能走 `Timeline::buildRawPcm`（1 层 = 1 采样、
//     采样率 = bpm/60、样值 = hitsoundVolume/50）+ `preSynthesizeRawPcm`（重采样到设备率）。
//     这条链此前**没有任何测试**，而它决定"能不能听见"。
//
// 断言分三层，每层都能单独失效：
//   ① `buildRawPcm` 认出这张谱（rate/采样数对得上）；
//   ② **负向对照**：普通谱（`diag_dict`，BPM 341）必须被拒 —— 否则任何谱都会被当 PCM 播成噪声；
//   ③ 重采样后的缓冲里，Do/Mi/Sol 三个频率的能量必须**同时**远高于对照频率
//      （同时 = 和弦，不是琶音；对照 = 200 Hz / 700 Hz 这些没有分音的位置）。
//
// 用法：adocao_raw_pcm_test [<和弦谱> [<普通谱> [<输出.wav>]]]

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

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

// 单频能量（Hann 窗 + Goertzel 式的直接求和）：够用且不引第三方库
double tone(const float* buf, size_t frames, int channels, int sr, double freq,
            double t0, double t1) {
    const size_t i0 = (size_t)(t0 * sr), i1 = (size_t)(t1 * sr);
    if (i1 <= i0 || i1 > frames) return 0.0;
    const size_t n = i1 - i0;
    const double w = 2.0 * 3.14159265358979323846 * freq / (double)sr;
    double re = 0.0, im = 0.0, norm = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double)k / (double)n);
        const double s = (double)buf[(i0 + k) * (size_t)channels] * hann;   // 左声道足够（L==R）
        re += s * std::cos(w * (double)k);
        im += s * std::sin(w * (double)k);
        norm += hann;
    }
    if (norm <= 0.0) return 0.0;
    return 2.0 * std::sqrt(re * re + im * im) / norm;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string chordPath = argc > 1 ? argv[1] : "tests/charts/diag_chord.adofai";
    const std::string plainPath = argc > 2 ? argv[2] : "tests/charts/diag_dict.adofai";
    const std::string wavPath   = argc > 3 ? argv[3] : "";

    // ---- ① 和弦谱：buildRawPcm 必须认它 ----
    LevelData lv;
    if (!lv.loadFromFile(chordPath)) {
        std::printf("FAIL 加载和弦谱 %s\n", chordPath.c_str());
        return 1;
    }
    Timeline tl;
    tl.build(lv, false);

    std::vector<float> pcm;
    double rate = 0.0;
    const bool ok = tl.buildRawPcm(pcm, rate);
    std::printf("     buildRawPcm=%d rate=%.0f 采样=%zu 层=%zu\n", (int)ok, rate, pcm.size(),
                lv.angleData.size());
    check(ok, "audio-as-chart 被 buildRawPcm 认出来");
    if (!ok) return 1;
    // 采样率 = bpm/60，由谱面自己决定（别硬编码：换个采样率就会误报，真踩过）
    check(std::fabs(rate - lv.settings.bpm / 60.0) < 1e-6, "采样率 = bpm/60");
    check(pcm.size() == lv.angleData.size() + 1, "1 层 = 1 采样（含末尾那块 extra 砖）");

    // 时间轴必须与采样时钟一致：每砖恰好一拍，否则画面与声音会错开
    {
        const double beat = 60.0 / (double)lv.settings.bpm;
        const std::vector<double>& st = tl.tileStartTimes();
        const double span = st.size() > 2 ? st[2] - st[1] : 0.0;
        std::printf("     每砖 %.6f s（一拍 %.6f s，1/rate %.6f s）\n", span, beat, 1.0 / rate);
        check(std::fabs(span - beat) < beat * 1e-3, "每砖恰好一拍（= 1 个采样周期）");
    }

    // ---- ② 负向对照：普通谱不能被当成 PCM ----
    {
        LevelData plain;
        Timeline pt;
        if (plain.loadFromFile(plainPath)) {
            pt.build(plain, false);
            std::vector<float> p2;
            double r2 = 0.0;
            const bool ok2 = pt.buildRawPcm(p2, r2);
            std::printf("     普通谱 %s → buildRawPcm=%d\n", plainPath.c_str(), (int)ok2);
            check(!ok2, "普通打拍音谱面必须被拒（否则会当 PCM 播成噪声）");
        } else {
            std::printf("     （跳过负向对照：加载不了 %s）\n", plainPath.c_str());
        }
    }

    // ---- ③ 重采样到设备率，再验三个分音**同时**存在 ----
    HitsoundManager hm;
    hm.init();
    const bool okRaw = hm.preSynthesizeRawPcm(pcm, rate);
    check(okRaw, "preSynthesizeRawPcm 成功（不是回退到打拍音混音）");
    if (!okRaw) return 1;
    std::printf("     设备缓冲 %zu 帧 @ %d Hz（%.3f s）\n", hm.totalFrames(), hm.sampleRate(),
                (double)hm.totalFrames() / (double)hm.sampleRate());
    const int srDev = hm.sampleRate();
    const double expectedFrames = (double)pcm.size() * (double)srDev / rate;
    check(std::fabs((double)hm.totalFrames() - expectedFrames) < expectedFrames * 0.01,
          "缓冲长度 = 采样数 x 设备率/源率");

    const float* buf = hm.buffer();
    const int sr = hm.sampleRate();
    const size_t frames = hm.totalFrames();
    // 取曲子中段（躲开淡入/淡出），三个音必须在这一窗里**同时**有能量。
    // 窗口按谱面实际时长算 —— 谱可变长，硬编码就会量到静音（真踩过）。
    const double dur = (double)pcm.size() / rate;
    const double t0 = dur * 0.25, t1 = dur * 0.85;
    const double notes[3] = {261.63, 329.63, 392.00};              // Do Mi Gol
    const double ctrl[2]  = {200.0, 700.0};                        // 没有分音的位置
    double noteE[3], ctrlE[2];
    for (int i = 0; i < 3; ++i) noteE[i] = tone(buf, frames, 2, sr, notes[i], t0, t1);
    for (int i = 0; i < 2; ++i) ctrlE[i] = tone(buf, frames, 2, sr, ctrl[i], t0, t1);
    std::printf("     能量 Do %.5f  Mi %.5f  Sol %.5f | 对照 200Hz %.5f  700Hz %.5f\n",
                noteE[0], noteE[1], noteE[2], ctrlE[0], ctrlE[1]);
    const double ctrlMax = std::fmax(ctrlE[0], ctrlE[1]);
    check(noteE[0] > ctrlMax * 20.0, "Do（261.63 Hz）远高于对照");
    check(noteE[1] > ctrlMax * 20.0, "Mi（329.63 Hz）远高于对照");
    check(noteE[2] > ctrlMax * 20.0, "Sol（392.00 Hz）远高于对照");
    check(std::fabs(noteE[0] - noteE[2]) < noteE[0] * 0.5,
          "三个音能量同量级 → 是和弦（同时发声）而不是琶音");

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
