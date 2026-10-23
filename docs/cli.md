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
  --force-hitsound [TYPE] --auto-play --msaa N(默认 4) --windowed/--borderless
  --cull-block N    剔除粗块粒度（层/块，默认 4096；0 = 关闭索引走全扫）。只影响剔除成本与
                    索引内存，**不改画面**（8 种粒度实测 PNG 逐字节相同）—— 见 docs/rendering.md
  --trail-duration SEC | --trail-tiles N
  --trail-sample-rate N | --trail-target-fps N
  --trail-rate-min N --trail-rate-max N --trail-samples-per-tile N
  --trail-sagitta PX
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
`--exclusive` / `--no-exclusive` 已经**整体删除**（2026-10-19 用户拍板）：全屏一律用 osu! 的
Borderless 语义，见下节"窗口模式"与 `docs/mac-recovery.md` 里的事故记录。把 `--export` 并入 `export` 子命令，`--stitch-scale` 先是改名
`--map-stitch-scale`、现在随子命令简化成 `stitch --scale`。

Trail sampling — two independent axes, both optional and **off by default** (wizard
Visuals page / CLI):

### 砖色：`--fill` / `--stroke` 现在是"整体覆盖"

默认可视化用**谱面自己的颜色**（settings 的 trackColor/trackColorType/trackStyle + ColorTrack 事件）。
`--fill`/`--stroke`（以及向导 Colors 页）只在显式给出时把整条轨道画成一套颜色 —— 旧行为，
保留给"只想看几何/单色"的场合；不给出时按 chart 颜色逐砖着色。

### 从指定时刻开始（`--start-at SEC`）

`--start-at SEC` 让播放从关卡时间的该秒数开始，等价于"选砖后按空格"：时钟、音乐 seek、
音色游标一起跳过去。给 profiling/自动化用（配合 `ADOCAO_PROFILE=1 ADOCAO_EXIT_AFTER_MS=N`
就能量"从某个中途点开始播"的整段剖面，而不必从头播）。

### 录屏（OBS/QuickTime）建议

应用默认 `glfwSwapInterval(0)`（无 vsync）。macOS 上 **OBS 的 Window Capture** 抓无 vsync 的
GL 窗口时，可能抓到“两帧合成”的撕裂帧（轨道重影/重叠），或偶发 1–2 帧全黑；这不是渲染输出。
对比/录屏时：

```bash
ADOCAO_VSYNC=1 ./build/adocao "谱面"
```

或改用 **Display Capture**。确定性抓帧（`--capture`）始终走单帧渲染，不受此影响。

### 窗口模式（`--windowed` / `--borderless`）

**两档**（`cfg.windowMode`：0/1），默认 **Borderless**。语义就是 osu! 的 Borderless
（SDL 的 `SDL_WINDOW_FULLSCREEN_DESKTOP`）：

| 模式 | 行为 | 抓屏（WGC/OBS） |
|---|---|---|
| **Windowed**（`--windowed`） | 带边框的普通窗口，`--width×--height` | 能 |
| **Borderless**（`--borderless`，默认） | `GLFW_DECORATED=false` 的无边框窗口 + macOS 上 `toggleFullScreen:` 进**自己的 Space**（= SDL `SDL_WINDOW_FULLSCREEN_DESKTOP` / osu! 的 Borderless）：**不切显示模式**（HiDPI 保持）、**不改 presentation options**；app 退出时 Space 自动消失。两个前置条件（都踩过）：① 先补 `styleMask` 的 `Resizable｜Titled`（GLFW 的无边框窗口没有这两位，不补 `toggleFullScreen:` 会被静默拒绝）；② 过场期间不要改窗口尺寸/位置（否则被当成"用户拖窗口"，把全屏踢掉） | **能** |

* 向导 **Graphics 页**是同一个两档下拉（`Window mode`），选完即写入 `cfg.windowMode`。
* **模式在启动时定死，运行期不允许更改**（2026-10-19 用户拍板）：`Alt+Enter` 不再是快捷键。
  理由：运行期切全屏要在 Space 过场里改窗口几何，会把全屏踢掉/留下可拖窗口，属于纯风险无收益。
* `--width/--height` 只对 **Windowed** 有意义（全屏铺满显示器）。
* **Windowed 只有关闭按钮**（红）：没有最小化/缩放，也不可拖边框改大小 —— 窗口大小由启动参数决定。

#### 为什么没有 "Exclusive"（2026-10-19 的事故）

曾经照 osu! 的 Fullscreen 实现过"独占"：切显示模式 + `NSApplicationPresentationFullScreen`
（kiosk）+ 藏光标。实测后果：桌面从 2x 掉成 1x、所有窗口重排（"退出卡一下、桌面缩放还动"），
而且 kiosk presentation options 会**禁用 Mission Control / ⌘Tab** —— app 一旦卡住或被杀，
用户就被困在它的全屏 Space 里，只能在别的 app 里写字求救。**整档删除**，详见
`docs/mac-recovery.md` 的禁令清单。

### 抗锯齿：为什么默认 MSAA 4x（而不是 2×128 条带纹理）

砖的边缘是**解析几何**（`tile.vert/frag` 里按 `v` 坐标算出来的），MSAA 只抗"几何覆盖率"，
正好覆盖这种情况：超远视距下细于 1 px 的轨道描边，不开 MSAA 会碎成**虚线**，开 4x 后是连续实线
（2026-10-21 与录制版逐帧对比实测）。

这也是**不用 Re_ADOJAS 的 2×128 条带纹理**（`uTrackEdge`）的原因：条带的边缘靠**纹理 alpha**，
MSAA 抗不到，只能靠超采样；而解析几何 + MSAA 更便宜也更稳。日志里有一行
`MSAA: 请求 4x，实际 4x` —— 实际采样数被驱动降级时能看见。

注意：换 MSAA 默认值会让**像素门槛基线失效**（逐字节比较），必须用同一 MSAA 设置重存。

想量真实帧率，先确认日志里的 `swap interval:` 一行：`0` 才是无 vsync（默认），`1` 是
`ADOCAO_VSYNC=1` 打开的、上限就是显示器刷新率。另外注意 **WGC / DXGI 复制类抓屏只能看见
合成器每周期提交的那一帧**（≈ 显示器刷新率），它们报的 fps 不能用来证明 app 的实际渲染帧率
—— 那个要看 `ADOCAO_PROFILE=1` 的 `frame` 分位数，或 PresentMon（AdoFrame 的 `SourceLabel`
要是 PresentMon，不是 WGC）。

### 抓帧与关卡相机（`--capture` / `--capture-camera`）

`--capture <out.png>` 是**确定性抓帧**（不走 wall clock / 音频 / 帧时间反馈），
`--capture-time SEC` 与 `--capture-tile N` 二选一，`--capture-zoom Z` 指定缩放。

`--capture-camera` 让抓帧也走**关卡相机**：`settings.relativeTo/position/rotation/zoom` +
到目标时刻为止的全部 `MoveCamera`（含 `RepeatEvents` 展开）。这条模式下：

* **zoom 由相机决定**，`--capture-zoom` 被忽略（否则运镜的 zoom 事件会被 CLI 覆盖，等于没测）；
  注意两者**口径不同**：谱面 zoom 是 ADOFAI 语义（越大看得越远，100 → 可见高 5.47 砖）；
  `--capture-zoom` 是本程序内部的放大倍数（100 → 可见高 12 砖，既有基线用它）；
* 相机状态会打到日志：`capture-selfcheck: 关卡相机 t=… pos=(x,y) rot=… zoom=…`；
* 默认**不开**：既有 51 个像素基线是"snap 到砖心、无旋转"的口径，而 The Moon 的
  `settings.relativeTo=Tile` 会让整幅画面平移 —— 改默认口径等于一次性废掉全部基线。

播放路径（不经 `--capture`）一直走关卡相机；两份实现是同一个 `CameraDirector`，
语义见 `docs/camera.md`。

### 剔除块粒度（`--cull-block`）

`--cull-block N` 选"每 N 层一个 AABB"的粗块索引粒度（wizard 的 Graphics 页同一个开关，
env `ADOCAO_CULL_BLOCK` 可覆盖）。默认 **4096**（0.0098 B/砖，2³¹−1 外推 21 MB），这与
`.adocao` 的分块粒度、以及 Re_ADOJAS 的可见窗口半径一致。

取舍（全部实测，`docs/rendering.md`「④ 剔除」有表）：块越小 → 折返/绕圈谱的块 AABB 越紧、
剔除越快，但索引内存按比例增大（256 层 = 320 MB 外推）；块越大 → 内存越小，但折叠谱上块内
候选变多。**它不改画面**：`--cull-block 0/256/…/65536` 在同一帧上出图逐字节相同，
`drawn` 也相同。合法范围 64…1048576，超范围会钳制并在日志里 `LOG_W`。

### 路径语义 / 从 `.app` 启动

所有路径（`--level`、`--capture`/`--image` 的输出、`--export-dir`…）都按**进程当前目录**解析，
**从 `build/ADOCAO.app/Contents/MacOS/ADOCAO` 启动也一样** —— 这条不是自动成立的：GLFW 的
Cocoa init 默认会 `chdir` 到 bundle 的 `Contents/Resources`（`GLFW_COCOA_CHDIR_RESOURCES`，
默认开；见 glfw `src/cocoa_init.m`：main bundle 存在且其 resources 目录末级正好叫 `Resources`
时直接 `chdir`）。那会让 `.app` 路下"相对谱面路径"报 `Failed to load level`，而相对输出路径的
PNG **悄悄落到 `Contents/Resources/` 里**（2026-10-15 实测）。
我们在 `glfwInit()` 之前显式关掉它（`app/Application.cpp::initGlfwHints()`）—— 资产定位不依赖
那次 chdir（`app/AssetSetup.cpp` 已把 `exe 目录/../Resources` 显式加进 `options().roots`），
所以关掉之后，`.app` 路与直跑 `build/ADOCAO` 的相对路径行为**完全一致**
（实测：相对谱面 + 相对输出、绝对路径、从没有 `assets/` 的 CWD 启动，三者出图逐字节相同）。

**Length** — `--trail-duration SEC` (default) or `--trail-tiles N` (wizard
"Length in tiles" + "Trail tiles"). A *time* window is unusable on charts whose BPM
spans orders of magnitude: 0.4 s is ~6,800 tiles at BPM 1.5M, so the trail sweeps
across the whole map and, whenever the path doubles back inside the window, lands
mostly in front of the planet (measured: 41,922-tile span, 12,179 tiles ahead of the
planet on the 6.7M-tile test chart). Measuring the length in tiles bounds the extent
to N tiles: the same moment with `--trail-tiles 8` spans 2 tiles and nothing ahead.
Sampling rate cannot fix that — it changes the polyline's resolution, never where the
history lies.

**Rate** — the fixed Hz rate (`--trail-sample-rate`, **默认 800**；governor-adjusted when adaptive)
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

**Sagitta 判据**（`--trail-sagitta PX`，默认 0=关）：按**屏幕像素**反推采样率，而不是按 BPM。
圆弧上每步 `s`（砖）的折线偏离 ≈ `s²/(8R)` 砖 → 换算成像素是 `s²·pxPerTile/(8R)`；令其 ≤ PX：

```
rate = max(base, tilePathSpeed(t) / sqrt(8·R·PX/pxPerTile))
R = max(tileStartDist, tileEndDist)（砖，行星轨道半径）
pxPerTile = 视口高 / 视野高（=12 砖 × 100/zoom）
```

它**随 zoom 变**：拉近看曲线才需要更密，远看不白烧点数。实测（BPM 1200、1.0 s 拖尾、
`pxPerTile=90`）：`PX=0.2` → 385 Hz；`PX=1` → 仍取 base 200 Hz（亚像素，看不出折线）；
`PX=4` → 200 Hz。判据只**抬高**采样率，从不压低用户给的 fixedRate。

默认值 800 Hz（2026-10-21 起，原 200）：按 sagitta ≈0.5 px 判据，默认视距下覆盖到 BPM 2000 左右；
0.4 s 拖尾只有 320 点（上限 8192），帧时间影响可忽略。

经验值：**粗拖尾（带宽 ~ 0.1 砖）在 PX≈1 时已经看不出多边形**；BPM 1200 下 60 Hz 那种
（≈30°/样本、sagitta≈7 px）是唯一肉眼可见的失真。

The window/rate maths lives in core (`PositionSolver::trailWindow`,
`sampleTrailRange`) so it can be exercised headlessly.

Without `--level`, falls through to the ImGui launcher.


**无头子命令的日志**：`Logger` 的 sink（`ADOCAO.log` 或 `--debug` 的控制台）只有 GUI 的
`Application` 会 `init()`，所以 `image` / `tiles` / `stitch` / `export` / `pack` 以前把加载与解析
路径上的 `LOG_W`/`LOG_E` **整个丢掉**，失败时只剩一句 `Failed to load level`。现在没有 sink 时
**Warning 及以上退回 stderr**（`core/util/Logger.cpp`，Debug/Info 不跟，免得刷屏）——
`docs/2e9.md` §4 ① 的验收就靠这条能看见"流式窗口放弃（原因）—— 退回整份解压"。

