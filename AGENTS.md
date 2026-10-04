# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 导航（先读这里，省 token）

- 目标结构蓝图 + 已拍板决策 1–10：`docs/project-structure.md`
  ✅ P1–P5 全部完成：目录搬迁+CMake 拆库（P1+P2）→ PlaybackEngine 拆 core/timeline（P3）→
  app 拆分（P4：GameWindow → `app/CameraController`/`app/LevelScene`，LauncherWindow → `app/wizard/` 分页）
  → 资产并入 `assets/` + g_sc 清理 + 文档/CI 同步（P5）
- 纯逻辑层护栏：`scripts/check-core-purity.sh`（core/ 禁 glad/GLFW/imgui/miniaudio/tinyfiledialogs/平台头，CI 已接入）
- 解析对拍测试：`tests/level_parse_test.cpp`（快路径 vs cleanJson+DOM 逐位比对，用例在
  `tests/level_fixtures/`，生成脚本 `tests/gen_level_fixtures.py`）；`ctest --test-dir build`
- 待办（已筛选）：`TODO.md`（未完成 9 条 + 遥远的未来：MoveTrack）
- 已彻底删除：GPU compute culling（2.0.0 起不需要，勿再引入）
- 脚本：`scripts/push-ci.sh`（push → gh run watch；`--watch` 默认输出平台耗时/产物）
  `scripts/run.sh --debugger` | `scripts/release.sh`（支持 x.y.z-AlphaN/-BetaN/-RcN）
  `scripts/make-app.sh`（macOS：把 `build/ADOCAO` + `assets/` 打成自包含的 `build/ADOCAO.app`，含 ad-hoc 签名；CI 的 macOS job 也用同一个脚本）
  `build.sh`（已有 build/ 缓存且不带参数 = 增量构建，不提问）

## Project

A native C++ / OpenGL "A Dance of Fire and Ice" (冰与火之舞) level viewer. Plays `.adofai` custom levels on Windows, Linux (Wayland) and macOS.

A Vulkan port exists at `../ADOCAV/` (same game logic, Vulkan 1.2 backend).

Reference implementations:
- `../ADOFAI-JS/` — Core angle parsing
- `../Re_ADOJAS/` — Three.js web player (hitsound, camera, decorations)
- `../ADOFAN_PIXI/` — PixiJS web player
- `../ADOFAI/A Dance of Fire and Ice/` — Original Unity game. Decompile `Assembly-CSharp.dll` with:
  `../ADOFAI/dnSpy/dnSpy.Console.exe -o ../ADOFAI/decomp_game "../ADOFAI/A Dance of Fire and Ice/A Dance of Fire and Ice_Data/Managed/Assembly-CSharp.dll"`
- `../ADOFAI_HitSound/` — Maicy0609's C++ hitsound generator (RapidJSON, Nyquist filter, equal-power pre-scaling, double mixing). Reference for loading performance and hitsound synthesis.

A WinUI 3 C# launcher lived in a separate repo at `../ADOCAO_WinUI3_Launcher/` —— **已废弃，
不要再为它保留 CLI 兼容性**（历史原因：早期命令行语法是它的调用约定）。

## Branches

- `master` — Current development branch (OpenGL 4.3)

## Floating-Point Precision Rules

**ALL timing and position values must use `double` (float64).** Float32 precision loss at extreme values causes artifacts.
- `angleData`: `std::vector<double>` (16 decimal places)
- `tileStartTimes[]`, `m_elapsedTime`, camera target: all `double`
- GPU uploads: `float` only at last step (camera-relative offset), values stay small

## Build & Run

**Windows:** `build.bat` (`build.bat portable` for static, `build.bat exzoom` for min zoom 0.5)
**Linux / macOS:** `chmod +x build.sh && ./build.sh` — 首次运行提问；已有 `build/` 缓存后不带参数运行即增量构建（不提问、不重新 configure；改选项需带参数如 `-U -Portable`）
**Manual:** `mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --parallel`

Minimum CMake 3.20. C++20. OpenGL 4.3+ required. Dependencies via FetchContent (GLFW, glm, RapidJSON, Dear ImGui, miniaudio, stb, miniz, tinyfiledialogs).

`--debug` flag enables debug console, disables hitsounds by default.

**Logs** (`ADOCAO.log`, written by `app/Application.cpp::logPath()`): **next to the executable** — one predictable location no matter which directory you launched from (it used to be CWD-relative, which littered whatever directory you happened to be in, `$HOME` included). The one exception is a macOS `.app`, where the executable directory is `Contents/MacOS` and writing there would break the code signature; that case uses `~/Library/Logs/ADOCAO/`. Fallbacks when the executable directory is not writable: `~/Library/Logs/ADOCAO/` (macOS), `%LOCALAPPDATA%\ADOCAO\logs` (Windows), `$XDG_STATE_HOME/ADOCAO` or `~/.local/state/ADOCAO` (Linux), then the temp directory. The first `Log file:` line in the log records the path actually used.

## CLI Usage

```
adocao                                  向导（不带 --level）
adocao <谱.adofai> [播放选项]            直接播放
adocao export --level <谱>              导出 hitsound WAV
adocao image  <out.png>    --level <谱>   导出一张图：默认矢量全景，--1px 走 1px=1tile
                                        [--size WxH] [--bg HEX|transparent] [--time-color]
                                        [--padding F] [--thickness N] [--range A-B] [--native]
                                        [--1px [--scale N] [--1bit]] [--keep-tiles <dir>]
adocao tiles  <dir>        --level <谱> [--block N] [--threads N] [--time-color]
adocao stitch <dir> <out.png>           [--scale N] [--threads N]

播放选项（不带子命令时）：
  --level <file> --music <file> --width N --height N --fullscreen
  --fill HEX --stroke HEX --bg HEX --no-auto-stroke --no-hitsound --no-trail
  --force-hitsound [TYPE] --auto-play --legacy-culling --msaa N --no-exclusive
  --trail-duration SEC | --trail-tiles N
  --trail-sample-rate N | --trail-target-fps N
  --trail-rate-min N --trail-rate-max N --trail-samples-per-tile N
  --debug（开发）

  adocao --help [--all]
```

**子命令只是把语法分层**：播放路径（含 `--level/--music/--width/...`）保持老语法 ——
它本来就只有十几个开关、也够干净，没必要为对称再加一层；而且 WinUI3 launcher 已废弃，
**没有外部调用方需要兼容**，所以这一族以后也可以随意改。无头族折进了
`image`/`tiles`/`stitch`（模式内去掉冗余的 `--map-` 前缀：`--map-size` → `image --size`），
`--map-native-all <dir>` → `image … --1px`（或 `tiles <dir>`），`--map-stitch <dir> <out>` → `stitch <dir> <out>`，
`--map-tiles` → `image --range`；而 `--map`/`--map-mono` 就是 `image` / `image --1px --1bit`。
撞到旧开关名会**直接打印新写法**再退出（码 2），不会让你去猜。

**`adocao --help`（`-h`）是开关清单的唯一权威**：分组、约 30 行（默认不列开发开关，`--help --all` 才列
`--debug`/`--legacy-culling` 与环境变量钩子）。`scripts/check-cli-help.sh` 机械校验"main.cpp 解析的每个
`--flag` 都出现在 printHelp 里"，防止帮助漂移。未知的 `--flag` 现在**直接报错退出（码 2）**而不是像以前
那样静默忽略（打错字会悄悄开 GUI）；单横线参数（macOS 的 `-psn_...`）与单独的 `--` 仍然忽略。
已删掉纯 no-op 的 `--exclusive`（`LauncherConfig::exclusiveFullscreen` 默认就是 true，只有 `--no-exclusive`
有意义），把 `--export` 并入 `export` 子命令，`--stitch-scale` 先是改名 `--map-stitch-scale`、
现在随子命令简化成 `stitch --scale`。

Trail sampling — two independent axes, both optional and **off by default** (wizard
Visuals page / CLI):

**Length** — `--trail-duration SEC` (default) or `--trail-tiles N` (wizard
"Length in tiles" + "Trail tiles"). A *time* window is unusable on charts whose BPM
spans orders of magnitude: 0.4 s is ~6,800 tiles at BPM 1.5M, so the trail sweeps
across the whole map and, whenever the path doubles back inside the window, lands
mostly in front of the planet (measured: 41,922-tile span, 12,179 tiles ahead of the
planet on the 6.7M-tile test chart). Measuring the length in tiles bounds the extent
to N tiles: the same moment with `--trail-tiles 8` spans 2 tiles and nothing ahead.
Sampling rate cannot fix that — it changes the polyline's resolution, never where the
history lies.

**Rate** — the fixed Hz rate (`--trail-sample-rate`, governor-adjusted when adaptive)
by default, optionally raised with the track covered per second
(`--trail-samples-per-tile N` / wizard "Speed-aware sampling"): rate =
`max(fixedRate, trackCoveredPerSecond x N)`, clamped to
`[--trail-rate-min, --trail-rate-max]`. "Track covered per second" counts the step
between tiles **plus the arc swept by the tile's relative angle**
(`PositionSolver::tilePathSpeed`), because a slow tile can sweep 330° while barely
advancing. In tiles-length mode the rate is derived from the window instead
(`tiles x samplesPerTile / windowSeconds`), so the point count is exactly
`tiles x samplesPerTile` at any BPM (measured: 34 / 65 / 129 points for 8 / 16 / 32
tiles at BPM 1.5M) and the sample-count cap `maxPoints` (8192) is the only backstop.

The window/rate maths lives in core (`PositionSolver::trailWindow`,
`sampleTrailRange`) so it can be exercised headlessly.

Without `--level`, falls through to the ImGui launcher.

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
- Export: 向导的 Export 按钮，或 `adocao export --level <谱>`（写出 `<level>_hitsounds.wav`）

## 关卡加载（Level loading）

一次加载分两步：解析 JSON → `Timeline::build()`。磁盘 I/O 与 JSON 解析占了几乎全部。

**容器**：除明文 `.adofai`，还支持 `.adofai.xz`（xz/LZMA2）与 `.adofai.zst`（zstd）——
`../Song.adofai`（audio-as-chart 编解码器）就用这两个格式存谱面（4 分钟谱 ~1.5 GB 文本 → 50–80 MB）。
`core/level/LevelArchive.cpp` 按 **magic** 识别（不看扩展名），在**内存里**解压后交给下面的解析路径，
不写任何临时文件。xz 用 `lzma_stream_decoder_mt`（与 `../Song.adofai` 的 `xz.c` 同配置）：
那些谱是多 block 压出来的，单线程只有 ~0.5 GB/s，MT 到 ~2.8 GB/s（1.18 GB 实测 2549 ms → 534 ms）。
实测直接读 `.adofai.xz`：43 MB → 1.18 GB 谱面 ~2.4 s、50 MB → 1.40 GB 谱面 ~2.9 s（含解码，跑动区间 ±0.15 s）。
内存上有三处专门处理（都是量出来的，1.18 GB 输出 / 10 核 / 64 MiB 字典 / 19 blocks）：
  * `mt.memlimit_threading` 给 1 GiB 上限（`../Song.adofai` 那边是 UINT64_MAX）：解码 475 → 585 ms，
    解码器峰值 1.67 → 1.04 GB，+110 ms 换 0.6 GB；
  * 解压前从 xz 流尾的 footer + index 反推出解压后总大小并一次 `reserve` 到位（zstd 用
    `ZSTD_getFrameContentSize`）。靠 `std::string` 自己增长时新旧缓冲同时存在，峰值会白多 ~2.4 GB
    （实测 1.67 → 4.06 GB）；多留一个 chunk 免得最后一次 resize 又触发全长拷贝；
  * 于是"解码 + 1.18 GB 输出"峰值 4.06 → 2.19 GB；整份加载（含解析结构）峰值 5.32 → 3.20 GB
    （1.40 GB 那张：5.92 → 4.41 GB）。明文路径不受影响。
`tests/level_parse_test.cpp` 会把每个明文 fixture 在内存里压成 xz/zstd 再加载一遍，要求逐位一致，
并检查截断的流是干净失败而不是崩。

**流式窗口（压缩输入默认路径）**：`core/level/LevelArchive.hpp` 的 `ArchiveStream` 用两块**固定地址**的半窗
（默认 4 MB，`ADOCAO_WINDOW_KB` 可调）交替解压；消费方 `WindowParser`（同在 `LevelData.cpp`）按根成员
逐个处理、把残缺的值 carry 到下一块开头，于是 10 GB 文本不再需要 10 GB 匿名内存——那 10 GB 匿名页装不下
时会被系统压缩/写 swap，实测吞吐 1.35 GB/s → 0.12 GB/s，而且每次访问都要换回来。

- **默认启用**（压缩输入）；`ADOCAO_WHOLE_DECOMPRESS=1` 强制退回整份解压，`ADOCAO_WINDOW_KB`
  调半窗大小。半窗大小实测（TNR 1.18 GB）：2 MB 1894 ms、4 MB 1928 ms、8 MB 1944 ms、16 MB 1960 ms、
  96 MB 2062 ms——越小越贴缓存，取 **4 MB**（纯速度/内存权衡：窗口大小不再受"单个值多大"限制，
  `skip`/`settings`/`path` 都是可续扫描器，超长 `levelDesc`、超大 `decorations` 数组照样跨窗流过去）。
- 已验证：39 个 fixture + 284 KB 合成谱（2000 个跨窗 action）在 **4 KB 半窗**下与整份解压逐位一致；
  256 KB 不可压伪数据的半窗拼接（0 / 1 B / 1 KB 三种 carry）逐字节一致；"单个值大于半窗"能报
  `stuck()` 而不是死循环；1.18 GB / 1.40 GB 两张真实 `.xz` 在 96 KB / 1 MB / 8 MB 半窗下与整份
  解压**逐位一致**（13 个节的 hash 全同）。`ADOCAO_WINDOW_REQUIRE=1` 把"窗口路径放弃"变成失败，
  用来证明用例真的覆盖到它（这个坑踩过一次：静默回退让测试全绿）；`ADOCAO_FAST_REQUIRE=1` 界定
  "哪些 fixture 本来快路径就吃不下、因此允许两条路都放弃"。
- 覆盖范围：真有事件多样性的谱面也验过——6 张社区谱（Tempest 31.7 万事件、The Moon、Singularity、
  Fledgling、DONE 等）与 641 MB / 618 万事件 / **7 种事件类型**的 MYC，都对每个文件在内存里压成
  xz 与 zstd、再用 **4 KB 半窗 + 禁止回退**走窗口路径，与整份解压逐位一致；两张 `.xz` 巨谱
  （1.18 / 1.40 GB）同样逐位一致。`archiveRoundTrip()` 就长在 `tests/level_parse_test.cpp` 里，
  喂任何真实文件（含压缩谱）都会跑这套比对 —— 这一条正是被"你造的大谱只有一种事件"问出来的。
- 第三个坑：`skip`/`settings`/`path` 明明带状态可续，驱动却按"从该项起点重放"处理，于是
  **任何大于半窗的单个值都会放弃回退**（一个超大 `decorations` 元素、200 KB 的 `levelDesc` 都会）。
  现在这三类扫描器整窗提交、跨窗续扫，`angleData`/`actions` 才按元素边界 carry。
  永久用例：`windowHugeValueSelfTest()`（200 KB 字符串 + 500 个装饰物 / 4 KB 半窗）。
- 踩过的两个坑（都是"只在窗口中途发生状态转移时才出现"，小 fixture 掩盖）：
  1. 驱动把"相对当前游标的已提交字节数"当成了 `ArchiveStream::consume()` 要的**窗口内偏移**；
     第一次调用两者恰好相等，所以只在 actions 数组不从窗口开头起时才暴露 → carry 退到已解析
     对象中间，回放时重复解析（1.18 GB 谱 8 MB 半窗多出 9 个角度值 / 11660 个 action）。
  2. zstd：帧结束时半窗正好填满 → 该块 `eof` 未置位，下一块只含 carry 又被判"做不完"直接丢弃，
     结果**丢了末尾 1 KB**。现在尾块会交付一次。
- 实测（默认 4 MB 半窗 vs 强制整份解压，`loadFromFile` + 结构，都不换页）：

  | 谱面 | 窗口（默认 4 MB） | 整份解压 |
  |---|---|---|
  | TNR 1181 MB（.xz 43 MB） | **1912 ms / 峰值 3.22 GB** | 2247 ms / 4.27 GB |
  | Unity SC 1403 MB（.xz 50 MB） | **2277 ms / 峰值 3.42 GB** | 2671 ms / 4.77 GB |

  两个方向都赢：少一个"整份文本"的分配（峰值 −1.0~1.4 GB），也快 ~15%（不再分配+二次扫描整份
  文本，解析全程在缓存热的小窗口上做）。
- **试过并删掉**：解码线程与解析线程重叠（每半窗留 carry slack，生产线程提前解好另一半）。
  实测在三个半窗尺寸上**一致地慢 20~30 ms**（TNR 2082 vs 2062、1968 vs 1939、1957 vs 1926 ms）：
  xz 的 MT 解码本身就在吃内存带宽，和解析线程互相抢，重叠省不下时间，slack 还白削窗口容量。
  按"实测无收益就删"的规矩回退了，只留下真正的赢家（窗口本身）。
- **也试过并删掉：窗口内分段并行**（常驻线程池 + 按对象边界切段，段 0 主线程、其余派给 workers，
  逐位一致但更慢）。TNR 1.18 GB 实测：串行 1843 / 1838 / 1912 ms（16 / 32 / 64 MB 半窗）vs 并行
  2292 / 2331 / 2410 ms——一致更慢，且窗口越大差距越大。原因不是同步开销（那会随批次变小而消失）：
  **liblzma 的 MT 解码器本身已经把 10 个核吃满**（`mt.threads = lzma_cputhreads()`），再开 7 个解析
  线程只是互相抢核；这也解释了为什么"解码与解析重叠"同样没有收益。压缩输入的并行空间已经被解码器
  用掉了，除非哪天把解码降到单线程再把核让给解析（那会先亏掉 534 ms → 2549 ms 的解码时间）。

**每层结构（内存天花板在这里）**：TNR 1.18 GB / 915 万层，窗口路径峰值 **2.76 GB ≈ 300 B/层**
（含 xz 解码器的 1 GB 上限）。做掉的三块、以及各自的折算规则：

1. `tileHitsoundVolumes`：`unordered_map<int,float>` → 稠密 `vector<float>`（NaN = 无覆盖，空
   vector = 整谱无覆盖）。那张谱 457 万条覆盖 ≈ 209 MB → 37 MB：**峰值 3.22 → 3.06 GB**
   （1912 → 1860 ms）。折算 **44 B/条覆盖** —— 普通社区谱 0~2 条，等于没收益。
2. `FastAction`：内联 `std::string str`（**32 B，空的时候也占**）→ 16 位驻留 id
   （`internActionStrIn`/`actionStr`）。结构 48 → 16 B、actions 417 → 139 MB：
   **峰值 3.06 → 2.76 GB**、1860 → **1809 ms**。折算 **32 B/条 action**，所以普通谱也有收益：
   MYC 明文 618 万条 ≈ −198 MB（实测峰值 1.40 → 1.25 GB）、Tempest 31.7 万条 ≈ −10 MB
   （0.06 → 0.05 GB）、音频谱 912 万条 −292 MB。
   实现要点（两次踩坑换来的）：锁与 thread-local 缓存放 **.cpp 文件作用域**、按**驻留表地址**做
   缓存键 —— 类里不能有 `std::mutex` / `static inline thread_local` **数据成员**（MSVC 不接受，
   而且 mutex 让类不可拷贝）；别用"函数内局部静态 + `(this,strGen)` 代次"的变体（能编译、
   fixtures 全过，但 `windowBoundarySelfTest` 会报窗口路径与整份解压的 actions 不一致）；
   类名是 **`struct LevelData`** 而不是 `class`（改文件前先打印要改的确切文本）。
3. 流式窗口本身只服务压缩输入 → 对明文 0。

明文加载时间在这轮改动前后**无可测变化**（Tempest 35 ms、MYC 613 ms，3 次跑 ±几 ms）。
剩下的结构：`Tile` 32 B x 915 万 = 293 MB（`index` 可由在 vector 里的位置隐含 → 可省 37 MB；
angle/direction 是 float、位置是 double，精度规则不允许再压）、angleData 8 B/层 = 73 MB
（只在加载后用于 midspin 检测 → 可压成 1 bit/层）、timeline ~28 B/层。

**actions 分块并行解析**（`parseActionRegionParallel`）：actions 数组里每个对象彼此独立，
所以先扫一遍找切分点（只做括号/字符串配对，不做字段提取，0.44 s/GB），再切 N 段并行解析，
写进**同一块缓冲的各自槽位**（先按扫描时数出的对象数一次 `resize`，末尾再由各段实际
保留数压实；没有丢弃 action 时位移为 0，零拷贝），最后 `resize` 到位。
- 必须写进同一个缓冲：早先每段各一个 vector 再合并，等于把整份 actions 写两遍，
  1.18 GB 谱面实测峰值 3.20 → 5.18 GB，在内存紧张的机器上直接翻车。
- 分段解析器（`parseActionRangeInto`）会校验"这一段正好停在下一个切分点上"，不对就整段
  退回单线程，宁可慢也不出错。
- 纯内存合成基准（全 SetHitsound 的 fat action，本机）：142 MB 文本 162.8 → 94.2 ms（8 段），
  305 MB 文本 335.9 → 201.3 ms，约 **1.7x**；其中约 2/3 是找切分点那趟扫描，所以再往上要
  么把这趟扫描也并行/流水化，要么换更快的切分点启发式。
- 真实谱面（301 MB，无换页）：整份加载 439 → 341 ms，峰值 RSS 1.43 → **1.13 GB**。
  1.2/1.4/1.5 GB 那三张上的对比测不出来：本机当时已被反复的大谱加载压出 3.4 GB swap，
  8 个线程读 1.18 GB 文本互相抢页，数字不可信（且并行/顺序都逐位一致，已核对）。
- 阈值：actions 区间在 **32 MB ~ 768 MB** 之间才自动分块。更小的用融合单遍更快；
  更大的反而更慢——1.18 GB 谱面上 2 段 900 ms、4 段 980 ms、8 段 1474 ms（而 611 MB 的
  谱面 8 段只要 80 ms），多线程读不同区段把内存带宽/局域性吃满了，加上找切分点那趟
  0.5 s 的扫描，得不偿失。1.2 GB 以上退回融合单遍解析（实测 TNR 2630 ms vs 分块 2942 ms）。
  真正的解法是让解析在小的常驻窗口上做（滑窗流水线），那时并行分段才是免费的。
- 测试钩子：`ADOCAO_PARSE_PIECES=N` 强制 N 段（`=1` 退回单线程，对拍用）、
  `ADOCAO_PARSE_DBG=1` 打印找切分点/并行解析/压实的耗时。fixture 都低于阈值，
  所以对拍工具会额外用 `ADOCAO_PARSE_PIECES=4` 再加载一遍比对（否则这条路径 CI 跑不到）。

**两条解析路径，产出必须逐位一致**：

| 路径 | 实现 | 何时用 |
|---|---|---|
| 快路径 | `LevelData::tryFastParse()`：在原文上流式扫描，不建 DOM、不复制 | 默认 |
| 旧路径 | `LevelData::parseLegacy()`：`cleanJson()` + RapidJSON DOM | 快路径复现不了时回退；也是 A/B 基准 |

`cleanJson()`（`core/level/JsonCleaner.cpp`）就是这些年攒下来的**事实格式规范**：删 `\r`、
删前置/重复/尾随逗号、给漏写逗号的老文件补逗号。里面没有任何版本分支，所以快路径**逐条复现
这些规则**而不是去猜版本号；只要遇到复现不了的东西就返回 false、交回旧路径（此时对象还没被改
动过）。已知会回退的情况：字符串里的转义或裸 CR（RapidJSON 非 in-situ 也会解转义）、
`angleData` 里的嵌套数组/非数字、非整数 `floor`、DOM 会拒绝的 token（`True`/裸词/`+1`）等。
`ADOCAO_FORCE_DOM_PARSE=1` 强制走旧路径（对拍用）。

快路径的层次（顺序即数据流）：

1. `app/FileMap.hpp` —— **mmap** 只读映射（app 层：`core/` 禁平台头，见 purity 脚本）。
   `LevelData::loadFromFile()` 仍保留可移植的 `ifstream` 读法给测试/嵌入方。
2. `scanRootMembers()` —— 只遍历根对象直接成员：`angleData` / `settings` / `pathData` 记下
   区间，`decorations` 和其它未知成员整体跳过，**`actions` 边扫边解析**（解析器自己在配对的
   `]` 处停下并回报结束位置），于是整段 actions 只走一遍。
3. `parseAngleDataRegion()` —— 整数走快路径（≤15 位十进制整数在 double 里精确），带小数点/
   指数/超长的一律 `strtod`，与旧路径 `parseAngleDataFast` 同值。
4. `parseActionRegion()` / `parseActionObject()` —— 按 key 首字符分派（谱里 action 对象常带
   一堆不关心的键），字段先收集、最后按 eventType 组装，语义等同旧路径的逐分支读取。
5. `settings` 只有几 KB，仍交给 `cleanJson` + RapidJSON；两条路径共用 `readSettings()`，
   `finishLoad()` 也是共用的收尾（pathData→angleData、tile 位置、processActions、位置偏移）。

实测（同一台机器，双方都 `-O3 -march=native`，只算 `loadFromFile`，不含 `Timeline::build`）：

| 谱面 | 大小 | 旧 | 新 | 提升 | 旧 RSS | 新 RSS |
|---|---|---|---|---|---|---|
| Tempest | 25 MB | 253 ms | 43 ms | 5.9x | 0.25 GB | 0.06 GB |
| Won't You Make a Song with Me | 301 MB | 2733 ms | 461 ms | 5.9x | 2.85 GB | 1.43 GB |
| …MYC（fat actions，89 B/个） | 611 MB | 4772 ms | 695 ms | 6.8x | 5.08 GB | 1.71 GB |
| Thousand Nights Remembered | 1181 MB | 12845 ms | 1838 ms | 6.9x | 8.12 GB | 3.34 GB |
| Unity (SoundCloud) | 1403 MB | 14795 ms | 2764 ms | 5.3x | 8.30 GB | 3.82 GB |
| Unity.wav_rate | 1503 MB | 15639 ms | 2798 ms | 5.5x | 8.80 GB | 4.14 GB |

旧路径的耗时构成（301 MB）：`ifstream+stringstream` 723 ms、`cleanJson` 1250 ms、`angleData`
strtod 260 ms、DOM+抽取 438 ms、tile 位置 27 ms、`processActions` 50 ms。所以真正的大头是
**字符串复制 + cleanJson + DOM 分配**，不是"解析和算时间线没重叠"：`Timeline::build` 只有
160~300 ms，快路径已经把 actions 和 angleData 合成一遍扫描，无须"增量算前缀时间线 + 回头重算"。

**回归测试**：`tests/level_parse_test.cpp` 对每个用例加载三遍（强制旧路径 / 快路径 / 快路径再
一次），按 13 个节（angleData、actions、settings、tiles、tileBPMs、tileHasTwirl…）比对 FNV
hash 与元素个数，逐位相同才通过；`ctest --test-dir build`。`tests/level_fixtures/` 里 39 个用例
每个对应 cleanJson 的一条规则，另有"双方都应拒绝"的畸形用例。也可以直接喂真实谱面：

```
./build/tests/adocao_level_parse_test ~/Documents/Charts/*/*.adofai
```

改动解析器后**必须**跑它；负向对照（故意丢 action）应当报 14 处不一致，否则说明用例没走到快路径
（这个坑真踩过：fixture 写成 `{"floor":...}` 而真实谱面是 `{ "floor": ...}`，一个多余的 `++p`
因此只在 fixture 上暴露）。

## 图像导出（`adocao image`，真无头）

`adocao image out.png --level <谱> [--size WxH] [--bg HEX|transparent] [--time-color]
[--padding F] [--thickness N]` 把整条路径
（红/蓝两星 ✓）按世界坐标包围盒**等比**铺进一张 PNG ✓ —— **不碰 GL、不开窗口、不需要 ffmpeg** ✓，
所以在没有显示的机器上也能出图 ✓，CI 里也不用 Xvfb ✓。

- 位置取自 `PositionSolver::positionAtTile` ✓（和游戏里同一套解算 ✓），颜色用渲染器默认那对
  `fill`/`stroke` ✓，起点绿点、终点红点 ✓，沿进度从 stroke 渐变到 fill 便于看走向 ✓；
  `--time-color` 改成按**谱面时间**六档彩虹（`timeColorAt`，与 1px=1tile 瓦片同一函数 ✓）。
- **`--padding F`**：画布留白比例（默认 0.02 = 四边各留 2%，是"看得见的黑边"的来源；
  `--padding 0` = 内容**贴边**）。实测四边留白与 `padding` 精确对应（16K 上 2% → 左右 327 px、
  上下 131 px；`--padding 0` → 墨包围盒 `x[0,16383] y[0,6615]`，四边 0.00%）。
- **线宽 / `--thickness N`**：默认 `halfW = clamp(scale x thicknessScale x 0.5, 0.5, 6.0)`。
  注意 `thicknessScale` 只在 native 1px=1unit 下才起效；普通分辨率下永远撞 **0.5 px 下限**，
  即名义上是 1 px 的线，实际整条线只有抗锯齿渗出 —— 16K 实测宽度在 **2~3 px 之间抖**
  （亮度阈值 50%：2 px x 2810 行 / 3 px x 3791 行）。
  **想更细只能关 AA**：`blend()` 是 alpha 叠加，而一个像素会被 ~382 段路径穿过 → 邻像素
  （覆盖率 0.5）被反复叠加到饱和，于是**名义 1 px 的 AA 线，实心部分量出来是 3.6 px**。
  所以 **`--thickness 1`（或任何 ≤1）= 无 AA 的 1 px 硬线**（`drawSegmentHard`：DDA 沿长轴
  每步点亮一个像素，直接写满色）：16K 实测墨点 **27,331**（AA 版 62,293，**2.3x 少**），
  水平游程 1-3 px（AA 版 4-7 px）。代价是高倍放大有锯齿 —— 1 px 线的固有性质。
  `--thickness N>1` 仍走 AA 加粗（N=2 → 核心 3.6 px，N=8 → 9.3 px；渲染出来比名义宽约 1 px，
  同为重叠饱和所致）。
  对照：瓦片拼接那条路（`stitch --scale 120` 的最大值降采样）是**均匀 2 px**（6536/6543 行），
  但由 120 px 源块量化而来 → 高倍看是阶梯。
- 为巨谱设计：**两遍扫描**（先包围盒、再逐段画 ✓）不保存点位 ✓；逐段解析式抗锯齿 ✓，
  亚像素的段落也能正确堆叠 ✓。实测 **6,770,913 层 → 4096² 6.3 s / 685 KB** ✓、
  **16384x6616（108 MPix）5.0 s / 4.2 MB** ✓、Singularity 997,665 层 → 2048² 0.9 s ✓。
  内存护栏 `maxPixels`（默认 **256 MPix**：16K 是 108 MPix = 434 MB 缓冲的正当请求；1px=1unit 的
  1.5 Tpix 仍会被挡 ✓）。
- 实现：`core/map/LevelMap.{hpp,cpp}`（纯 core ✓，只有 RGBA8 缓冲 ✓）+ `app/MapExport.cpp`
  （stb 写 PNG ✓）；CLI 分支在 `app/main.cpp`，**在任何 GL 初始化之前**返回 ✓。
- **一条命令覆盖两条路线**：默认矢量（5 秒出 16K）；`--1px` 内部自己出瓦片→拼接→**清掉中间产物**
  （`--keep-tiles <dir>` 保留、`--scale N` 降采样、`--1bit` 单色谱写索引色）。`map`/`mono` 两个
  子命令已并入 `image`（撞到会打印新写法）；`tiles`/`stitch` 保留为进阶零件（只出瓦片 / 只拼瓦片，
  便于检视或复用已有瓦片）。
- 已知可续：按 tile 区间取景（`--range A-B` ✓）用来看密集"结"的内部结构 ✓。

## Playback Engine

Direction-based angle algorithm matching ADOFAI-JS `_parseAngle`. BPM propagation via pre-indexed SetSpeed events (O(n+m)). Twirl toggles `isCW` before computing angle — affects current tile. Full rotation only when `delta < 0.0001`. Double precision throughout.

`precalculateTiming()` split into 4 phases: (0) actions-by-floor index, (1) sequential state propagation (isCW/BPM/angleDir/extraRot), (2) parallel tile geometry via `std::async` (>= 256 tiles threshold), (3) sequential prefix sum → `tileStartTimes`, (4) last-tile handling.

`startAt(wallClockSec, audioPosSec, offsetSec)` supports mid-playback start from any tile. `findTileIndex()` binary-searches `m_tileStartTimes`. Bookmark navigation (Ctrl+←/→, stopped only) uses `jumpToTile()`. Track selection: click tile while stopped, Space starts from selected tile.

`--auto-play` auto-starts playback 0.5s after loading.

## Coordinate System

OpenGL world space: X right, Y up. View matrix always at origin — camera-relative offsets computed in double, converted to float for GPU.

## Rendering

### Shader files
GLSL source in `assets/shaders/` — loaded from files at runtime via `Shader::compileFile()` (asset lookup: CWD → exe dir → macOS `.app` `Contents/Resources` → up to 3 parents above the exe, see `app/LevelScene.cpp`). Hitsound WAVs live in `assets/hitsounds/` (`audio/HitsoundManager.cpp::findAssetsDir`, same order). On macOS the exe path is `realpath`-resolved first, so launching through a symlinked `.app` (e.g. `/Applications/ADOCAO.app`) still finds the bundle's own assets. `scripts/make-app.sh` copies `assets/` into the bundle. Embedded fallback strings remain in `render/Shaders.hpp`.

### Z-depth render order
Tiles and icons use depth test ON with per-instance Z values encoding far-to-near order. Ortho far plane reduced to 200 for depth precision (~755K steps, supports 7M-tile levels). Z allocation:
- Tile fill: `tileZ(i, n)` (0.0 far → 9.0 near), vertex Z +0.001
- Tile stroke: `tileZ(i, n)`, vertex Z 0.0
- Icons: `tileZ + 0.002` (twirl) / `+0.003~0.005` (SetSpeed)
- Planets: Z = 9.5 (always in front)
- Trails: depth test OFF, always visible
- Highlight: depth test OFF, always visible

### Per-instance color attributes
Vertex shader: `aType` (0=stroke, 1=fill) mixes `iColor`/`iBgColor` per-instance.
- Vertex VBO: `[x, y, z, type]` — 4 floats per vertex
- Instance pos VBO: `[offX, offY, offZ]` — 3 floats, uploaded per-frame
- Instance color VBO: `[fillR,fillG,fillB, strokeR,strokeG,strokeB, opacity]` — 7 floats, static

### Visibility cache
`draw()` caches visible instance indices per shape group. Rebuilt when frustum bounds change (position or zoom). Camera-relative offsets recomputed each frame on cached set. Multithreaded CPU culling through a **persistent** `ThreadPool` (`render/TileMesh.cpp` 的 `getPool()`，Meyers 静态：首次 cull 时创建、活到进程退出；worker 数 = `hardware_concurrency()`）for >= 64 groups (每组一个任务)；`cullAndOffsetGroups` 每帧都跑，但只有缓存的视锥范围不再包含当前视野时才重新 SIMD culling，否则只按 (dx,dy) 平移相机相对偏移。

### Memory management
`LevelData::releaseMemory()` frees angleData, actions/decorations JSON, tilePositionOffsets after loading. `tileBPMs` kept — needed by `buildIcons()` for SetSpeed icon coloring.

### Frame pacing
Sleep-based: `sleep_for(remaining - 1ms)` + spin last 1ms for precision. 320 FPS soft cap. DPI awareness + CPU pin to performance cores on Windows.

### Event icons
Twirl (purple), SetSpeed up (red), SetSpeed down (blue). Per-tile icon instances with depth-sorted Z.

### Highlight
Selected tile drawn with inverted colors via dedicated highlight shader (`1.0 - vColor` in fragment shader). Same vertex layout as tile shader, reads per-instance colors.

## File extensions

All project headers use `.hpp`. Third-party includes (miniaudio, imgui, tinyfiledialogs) retain their original extensions.
