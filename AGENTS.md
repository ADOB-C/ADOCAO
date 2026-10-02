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
           [--trail-samples-per-tile N]
```

Trail sampling (wizard Visuals page / CLI):
- **Fixed rate** — `--trail-sample-rate N`, or the wizard's "Sample rate" slider. The
  wizard always submits a fixed rate (it hardcodes `trailAdaptive = false`); the CLI
  leaves it adaptive unless `--trail-sample-rate` is given.
- **Adaptive** (CLI default) — the app measures real frame work and raises the rate
  (up to `--trail-rate-max`, default 4000) whenever frames fit under
  `1000/--trail-target-fps` ms, shedding toward `--trail-rate-min` (default 60) when
  frames get too expensive. Toggle from the CLI with `--trail-target-fps`,
  `--trail-rate-min`, `--trail-rate-max`.
- **Speed-aware** (optional) — `--trail-samples-per-tile N` or the wizard checkbox
  "Speed-aware sampling": raises the rate to
  `max(fixedRate, trackCoveredPerSecond × N)`, clamped to `[--trail-rate-min, --trail-rate-max]`.
  "Track covered per second" counts the step between tiles **plus the arc swept by
  the tile's relative angle** (`PositionSolver::tilePathSpeed`), because a slow tile
  can sweep 330° while barely advancing — sampling per tile alone would leave those
  arcs chunky. Being a `max()` on top of the fixed rate, it is never coarser than
  fixed mode. Measured on a 6.7M-tile chart that goes BPM 1000 → 8M: fixed 200/s
  leaves 268 tiles between samples at BPM 1.5M, speed-aware with the default 4000/s
  cap brings that to 13 tiles (raise `--trail-rate-max` for more). At those speeds
  the 0.4s window itself spans tens of thousands of tiles, so the trail still reads
  as a long beam — bounding the trail *length* in tiles is the real fix and is not
  implemented yet.

Without `--level`, falls through to the ImGui launcher.

## Audio System

### Music
`ma_decoder` (miniaudio) — supports AIFF, OGG, WAV, FLAC. File read into memory + `ma_decoder_init_memory()`. Output: stereo f32 @ 48000Hz. Device period: 1024 frames (~23ms) for best quality. `m_fileData` kept alive for decoder lifetime. Pause stops the audio device (not just sets a flag).

### Hitsounds
Pre-synthesis with 16-bit hard-clip integer mixing (matches `HitSoundGenerator.exe`):
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
