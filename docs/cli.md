# CLI 用法（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

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
  --force-hitsound [TYPE] --auto-play --msaa N --no-exclusive
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
`--debug`/`--capture*` 与环境变量钩子）。`scripts/check-cli-help.sh` 机械校验"main.cpp 解析的每个
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


**无头子命令的日志**：`Logger` 的 sink（`ADOCAO.log` 或 `--debug` 的控制台）只有 GUI 的
`Application` 会 `init()`，所以 `image` / `tiles` / `stitch` / `export` / `pack` 以前把加载与解析
路径上的 `LOG_W`/`LOG_E` **整个丢掉**，失败时只剩一句 `Failed to load level`。现在没有 sink 时
**Warning 及以上退回 stderr**（`core/util/Logger.cpp`，Debug/Info 不跟，免得刷屏）——
`docs/2e9.md` §4 ① 的验收就靠这条能看见"流式窗口放弃（原因）—— 退回整份解压"。
