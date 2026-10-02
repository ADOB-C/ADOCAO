# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 导航（先读这里，省 token）

- 目标结构蓝图 + 已拍板决策 1–10：`docs/project-structure.md`
  ✅ P1–P5 全部完成：目录搬迁+CMake 拆库（P1+P2）→ PlaybackEngine 拆 core/timeline（P3）→
  app 拆分（P4：GameWindow → `app/CameraController`/`app/LevelScene`，LauncherWindow → `app/wizard/` 分页）
  → 资产并入 `assets/` + g_sc 清理 + 文档/CI 同步（P5）
- 纯逻辑层护栏：`scripts/check-core-purity.sh`（core/ 禁 glad/GLFW/imgui/miniaudio/tinyfiledialogs/平台头，CI 已接入）
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

A WinUI 3 C# launcher is in a separate repo at `../ADOCAO_WinUI3_Launcher/`.

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
adocao.exe --level <file> --music <file> [--width N] [--height N]
           [--fullscreen] [--fill HEX] [--stroke HEX] [--bg HEX]
           [--no-auto-stroke] [--no-hitsound] [--no-trail] [--debug]
           [--force-hitsound [TYPE]] [--auto-play] [--export] [--legacy-culling]
           [--msaa N] [--exclusive | --no-exclusive]
           [--trail-duration SEC] [--trail-sample-rate N]
           [--trail-target-fps N] [--trail-rate-min N] [--trail-rate-max N]
           [--trail-samples-per-tile N] [--trail-tiles N]
           [--hitsound-drive N]
```

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

### Music
`ma_decoder` (miniaudio) — supports AIFF, OGG, WAV, FLAC. File read into memory + `ma_decoder_init_memory()`. Output: stereo f32 @ 48000Hz. Device period: 1024 frames (~23ms) for best quality. `m_fileData` kept alive for decoder lifetime. Pause stops the audio device (not just sets a flag).

### Hitsounds
Pre-synthesis into one float buffer, then a single gain for the whole track (the
old per-sample int16 hard clip was removed — it saturated 0.33% of samples on a dense
chart and cost two clamps plus two channel stores per sample, measured ~3.5x slower):
- Mixing: one float accumulation per sample on a single channel, duplicated to L/R at
  the end. Benchmarked against `ADOFAI_HitSound` (HitSoundBench) on Tempest and on a
  6.77M-hit chart.
- Loudness: peak-normalise to −1 dBFS, then a tanh soft limiter
  (`--hitsound-drive N`, default 4; 1 = off). Peak normalisation alone cannot control
  loudness — any gain is divided straight back out — so the linear mix measured
  RMS −16.06 dBFS while the old hard-clipped one was −7.06 dBFS. drive 4 lands at
  −7.62 dBFS with DR 9.1 dB (old 5.96 dB) and zero hard clipping; higher drive is
  louder still (6 → −5.75 dBFS, 8 → −4.70 dBFS).
- Optional Nyquist-style de-duplication (`HitsoundManager::setNyquistDedup`, off by
  default) drops hits landing < 41.7 µs after the previous kept hit. Measured 21.9x
  faster on the 6.77M-hit chart (91.5% of hits dropped: 18.1 s → 0.8 s), but it is
  NOT transparent — 95.0% of samples change, max |diff| 1.5x full scale, peak-
  normalised RMS +6.52 dB (Tempest: 3.15% dropped, 92.8% of samples change). Those
  hits are summed energy, so treat it as a different sound, not a free win.
- Multi-type support via `TimestampGroup` (SetHitsound events), 27 hit types
- Case-insensitive type matching (ADOFAI levels may use mixed case)
- Unknown type fallback: redirects to default type if WAV not found
- `--force-hitsound`: override "None" type → "Kick" (GUI: "Force HS" checkbox)
- Per-group WAV loading with volume scaling, cached in `s_wavCache` + `s_wavRawCache`
- `preSynthesize()` → stereo float buffer → streamed via `attachExternal()`
- Export: launcher Export button or `--export` CLI writes `<level>_hitsounds.wav`

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
`draw()` caches visible instance indices per shape group. Rebuilt when frustum bounds change (position or zoom). Camera-relative offsets recomputed each frame on cached set. Multithreaded CPU culling via `std::async` for >= 64 groups.

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
