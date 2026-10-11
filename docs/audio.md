# 音频系统（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

## Audio System

### audio-as-chart（每层一个采样的巨型谱）

`--force-hitsound raw-pcm`（向导 Override 下拉里也有）走**直通**，不做 hitsound 混音：
`Timeline::buildRawPcm()` 把"每层音量"当 PCM 采样值导出（1 层 = 1 采样，采样率 = `bpm/60`，
映射与 `Song.adofai` 的 `volume = int16 / 655.36` 严格互逆），`HitsoundManager::preSynthesizeRawPcm()`
线性重采样到设备采样率（48 kHz）再复制成双声道。249 s 的谱导出只要 9–25 ms。

为什么必须直通：那种谱是"**每个采样一个 Kick**"。Unity.wav_rate 实测 10,993,500 命中 / 249.3 s =
44.1 kHz，而 `Kick.wav` 有 9,499 帧（215 ms）→ **每个输出采样上叠 ~9,499 次相加**，而混音规则是
"每次相加都 clamp 到 ±32768"（这条对普通谱是忠实度要求，不能动）→ 必然整段顶轨，示波器上就是
一排单向尖峰。被删掉的 float + 软限幅版本当年"恰好"掩盖了它，所以这类谱在优化后听起来才崩。

**同时修掉一个长期存在、对拍看不见的 bug**：`processActions` 用 `a.val1 > 0` 判断"这个 SetHitsound
有没有写音量"，于是**负音量和 0 都被替换成 settings 默认（100）**。Unity.wav_rate 的 10,993,500 个
`hitsoundVolume` 里负 5,471,227、零 50,855 —— 全被吃掉，半个波形变成满刻度。改成用
`FastAction::flag` 携带存在性（负/零原样保留）。实测：满刻度占比 **50.25% → 0.05%**、RMS
**0.7737 → 0.4392**、min **0 → -1.0**，静音数 50,856 与文件里的零**逐一对上**。

教训：这类 bug 在**两条解析路径共用的代码**里，逐位对拍摘要永远一致 ✗。所以补了**直接语义断言**
`rawVolumeSemanticsSelfTest()`（负/零音量必须原样落到 `tileHitsoundVolumes`，旧路径+快路径都查），
并做了负向对照：把 `flag` 改回 `> 0` 时它立刻报 `floor 1: 期望 -33.50，实际 100.00` ✓。

**自测**（2026-10-11）：`tests/raw_pcm_test.cpp`（ctest `raw_pcm`）—— 用 `tests/charts/diag_wav_rate.adofai`
（**整谱就是 Do-Mi-Sol 波形**，每砖一个采样）验三层：① `buildRawPcm` 认谱（rate/采样数）；
② **负向对照**：普通打拍音谱必须被拒（否则会被当 PCM 播成噪声）；③ 重采样到设备率后，
Do/Mi/Sol 三个频率的能量**同时**远高于对照频率（实测 0.296/0.293/0.291 vs ~0）。

**另一个坑（同一天修的）**：`m_tileStartTimes` 的最后一格是 `calculateTilePositions()` 追加的
**合成 extra 砖**（无限旋转参考），它**不是谱面里的砖，不该产生打拍音** —— 混音循环按
`tileStartTimes().size()` 排时刻，于是**每张谱末尾都多响一下**（对拍看不见、像素门槛也看不见）。
现在走 `Timeline::realTileCount()`（真实层数 = `angleData.size()`），两种谱型都按它排。

### "拿 Kick 打出和弦"（相位编码节奏）
每砖放的是同一段打击音色，音高**不由音色决定，而由重复频率决定**（137 下/秒听起来就是 137 Hz 的
"蚊子声"）。所以要让耳朵听到 Do-Mi-Gol：把击打时刻放在三个频率网格的**并集**上（三条脉冲列相加
= 三条谱线）。

**关键：时刻与"看相"必须解耦**（两轮实测换来的）。角度控制时刻的公式是
（`Timeline.cpp:63-83`，CW）：

    delta    = (angleData[i-1] + 180 - angleData[i]) mod 360
    duration = (delta/360) x 2 x (60/BPM)          （delta≈0 特例成 360 → 两拍）

于是：

* **"展开"= delta ≈ 180**（砖走直线；用户的说法是"平均角度接近 180"）。若用角度管时刻，
  delta ≈ 180 就等价于"每砖恰好一拍"→ **击打间隔恒定** → 只有**一个音高**，出不了和弦；
  要出和弦，间隔得在 2.5~3.8 ms 之间变 → delta 跟着变 ±25% → 砖路必然卷成一坨
  （BPM 30000 时最大间隙要转 344°，画出来就是实心饼；BPM 1000 只是卷得小一点，仍然卷）。
* 所以分成两件事（`tests/gen_adocao_charts.py` 的 `chart_chord`）：
  **角度写一条慢速和弦波形**（均值 180、相邻差小 → 舒展的波形砖路），
  **时刻走逐砖 `SetSpeed`**，而且 BPM 要**按 delta 反算补偿**：
  `BPM_i = delta_i / (3 x gap_i)`（`duration = delta/(3 x BPM)`）—— 不补偿的话时长会被
  角度差缩放掉（实测短 1.4~7%，命中时刻整片偏移）。

**实测**（`tests/kick_chord_test.cpp`，ctest `kick_chord`，谱 `tests/charts/diag_chord.adofai`）：

* 1953 击落在 261.63 / 329.63 / 392.00 Hz 三网格的并集上，最大时刻误差 **0.0 us**；
* 混音后三条谱线各高于本底中位数 **70 倍**、三音能量同量级 = **同时**响（和弦，不是琶音）；
* 角度均值 **181.2**、每砖偏离直线 平均 **2.2°** / 最大 **13.3°** → 砖路是一条舒展的波形
  （不是卷曲的团）。

另一条路是 audio-as-chart（`diag_wav_rate`）：角度列也写波形（`180 + 180 x 样值`），
声音走 `hitsoundVolume` + raw-PCM 直通/高频 Kick 叠加 —— 真谱（Unity.wav_rate 一族，44.1~192 kHz）
用的就是这条，采样率高时相邻样值差很小，砖路自然舒展。
