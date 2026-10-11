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
= 三条谱线）。时刻由角度控制（`Timeline.cpp:63-83`，CW）：

    delta    = (angleData[i-1] + 180 - angleData[i]) mod 360
    duration = (delta/360) x 2 x (60/BPM)          （delta≈0 特例成 360 → 两拍）
    ⇒ 反解   angleData[i] = (angleData[i-1] + 180 - 3 x BPM x gap_i) mod 360

于是**角度序列 = 相位的累积**，砖路画成一圈圈花环 —— 这就是"别人那几张能听到和弦的谱"的样子。

* 音高只看击打时刻，**与 BPM 无关**；BPM 只决定"每砖最多能占多久"和**看相**。
* 实测（`tests/kick_chord_test.cpp`，ctest `kick_chord`，谱 `tests/charts/diag_chord.adofai`）：
  1953 击落在并集网格上（误差 **0.1 us**）、混音后三条谱线各高于本底中位数 **70 倍**、
  三音能量同量级 = 同时响（和弦，不是琶音）✓。
* **想"展开"要调低 BPM**（反直觉，别搞反）：转角 = `3 x BPM x 该砖时长`，BPM 30000 时最大间隙
  要转 344° → 方向几乎每砖翻一圈、画成一坨实心饼；BPM 1000 时同一间隙只转 ~9° → 半径几十砖的
  舒展花环（实测每砖最大转角 **8.7°**）。

### Music
`ma_decoder` (miniaudio) — supports AIFF, OGG, WAV, FLAC. File read into memory + `ma_decoder_init_memory()`. Output: stereo f32 @ 48000Hz. Device period: 1024 frames (~23ms) for best quality. `m_fileData` kept alive for decoder lifetime. Pause stops the audio device (not just sets a flag).

### Hitsounds
**Two product rules, both learned the hard way — do not relax them:**
1. **Every hit in the chart is mixed.** Do NOT adopt `ADOFAI_HitSound`'s Nyquist
   de-duplication (dropping hits < 41.7 µs apart). Measured: it is 20x faster on dense
   charts but changes 95% of the output samples, so it is a different sound.
2. **16-bit accumulation with the sum clamped on every addition** (`HitSoundGenerator`
   semantics) is the ONLY mixing path. A float path with a soft limiter and a global
   gain was tried and deleted: it changed 94.5% of the samples, and in listening the
   loud sections became a distorted plateau while quiet sections lost level. Peak or
   1/sqrt(N) normalisation also lets the densest moment set the gain — the quiet 10 s
   of the test chart fell from -26 dBFS to -78 dBFS. Faithfulness wins; the only thing
   allowed to change is *where* the work happens, never the result.

What may change (and did):
- Mixing is restructured in three output-neutral ways, all verified byte-identical
  against a coarse single-thread reference (`ADOCAO_MIX_THREADS=1`) and against a
  repeat synthesis in one process:
  1. the per-sample scale `(int)(sample * volume)` is hoisted out of the hit loop and
     computed once per group (millions of hits reuse the same 11k scaled samples);
  2. the accumulation buffer is MONO — in the authentic mix both channels always hold
     the same value, so one channel plus a final duplicate is exact and halves the
     read-modify-write traffic;
  3. the output is swept in L2-sized blocks that only visit the hits overlapping them,
     and blocks are handed to worker threads (`ThreadPool`), each writing only its own
     range and visiting hits in timestamp order.
  The inner loop is a saturating int16 add — `vqaddq_s16` (NEON) / `_mm_adds_epi16`
  (SSE2), with a scalar fallback — which is exactly the per-addition clamp, so SIMD
  costs no fidelity. Blocks are scheduled by a local `std::thread` loop, not by
  `core/util/ThreadPool`, so `adocao_audio` stays free of that dependency and external
  harnesses that compile this file alone (HitSoundBench's `adocao_gen`) keep working.
  Falls back to an int32 path when a scaled sample does not fit in int16 (volume > 100).
  Measured synthesis: 6.77M-hit chart 36.3 s -> 0.74 s (49x), Tempest 207 ms -> 18 ms.
  HitSoundBench, same machine, 3 runs, both sides -O2:
    Tempest  total 263.4 ms vs ref 404.6 ms  -> we are 1.54x FASTER (synth 17.9 vs 286.6)
    level    total 3669 ms  vs ref 2192 ms   -> we are 1.67x slower, and the whole gap
             is now the PARSER (2580 vs 1385 ms) plus timeline (251 ms): our synthesis
             is already faster than the reference's (738 vs 807 ms) while mixing 11.8x
             more hits. Next lever if wanted: the level JSON parse.
- Fixed a cache bug that made any *second* synthesis in one process wrong: `readWav`'s
  cache-hit path reported `channels = 1` while the cache keeps the file's layout, so a
  cached stereo hit became 2x too long (buffer 217.582 s instead of 217.350 s) and had
  its interleaved L/R mixed as consecutive frames. The cache now stores and returns the
  real (rate, channels).
- Multi-type support via `TimestampGroup` (SetHitsound events), 27 hit types
- Case-insensitive type matching (ADOFAI levels may use mixed case)
- Unknown type fallback: redirects to default type if WAV not found
- `--force-hitsound`: override "None" type → "Kick" (GUI: "Force HS" checkbox)
- Per-group WAV loading with volume scaling, cached in `s_wavCache` + `s_wavRawCache`
- `preSynthesize()` → stereo float buffer → streamed via `attachExternal()`
- Export: 向导的 Export 按钮，或 `adocao export --level <谱>`（写出 `<level>_hitsounds.wav`）。
  **`adocao export` 必须给 `--music`**（导出的是 hitsound **混音**）：不给会**明确报错并退 2**，
  不再是崩（2026-10 真崩过：`exportOnly=true` 的加载不建 `tiles`，而导出分支去读
  `tiles[i-1].direction` → EXC_BAD_ACCESS，且日志只写文件、终端看不见）。
  另外注意产物**很大**：3 分钟的谱就是 ~1 GB WAV —— 别留在仓库里。

