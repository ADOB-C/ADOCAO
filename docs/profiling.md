# Profiling：量什么、用什么量、各自的边界

> 为什么单独一篇：2026-10-15 核对发现 `TODO.md` 里的 `[x] Frame profiler (per-section timing)`
> **是假的**（全仓找不到 per-section 计时代码，大概 P1–P5 重构时丢了）。这一篇把"现在有什么、
> 怎么用、量不到什么"写清楚，别再靠印象。

## 0. 先问"要回答什么"

| 问题 | 用什么 | 在哪 |
|---|---|---|
| 这一帧里**哪一段**贵？ | `ADOCAO_PROFILE=1` 的分段计时 | §1 |
| 整帧 / 稳态帧多少？ | `--capture` 的 `render_ms`/`first_ms` + `ADOCAO_CAPTURE_FRAMES=N` | §2 |
| **常驻**内存 / 峰值 / 谁占的？ | `app/ProcMem.hpp` 的常驻日志、`/usr/bin/time -l`、`vmmap`/`heap` | §3、§4 |
| 函数/哪一行贵？ | `sample`（本机验过）/ `xctrace` | §4 |
| 剔除策略、容器列、窗口化加载？ | 专用探针（cull_cost_probe / --codec-report / window --walk） | §5 |

## 1. 分段计时（`ADOCAO_PROFILE=1`，默认关）

段名与放置：

| 段 | 位置 |
|---|---|
| `vis` | `LevelScene::render` 开头 `updateTileVisibility`（TrackVis 起隐/显）|
| `cull` | `TileMesh::draw` 内的 `ensureDrawList`（④ 的筛块 + 块内判 + 拼表）|
| `upload` | `uploadIfNeeded`（实例缓冲上传）|
| `draw` | `glBindVertexArray` + `glDrawElementsInstanced`（GL 提交；不 glFinish）|
| `trails` / `planets` | `LevelScene::render` 的拖尾 / 行星 |
| `other` | **不是段**：`帧自量 − 段和`（ImGui、swap、present、驱动侧等待等）|

两条口径：
* `N=1` → `profile(单帧)`：该帧单次快照（**首帧**，含一次性 GL 开销）。
* `N>1` → `profile(首帧)`（单次快照）+ `profile(稳态)`：**第 2..N 帧**每段给出
  `中位[min..max]`，帧本身也给 `frame=中位[min..max] n=…`，`other~=` 用中位数相减估算。
  —— **首帧不进稳态统计**（它是一次性开销，混进去会把一切拉高）。

**为什么要多帧**：单样本在这个路径上噪声很大。实测 MYC（`ADOCAO_CAPTURE_FRAMES=10`）：
稳态帧 **0.679 ms [0.307..0.784] n=9**，而"只看最后一帧"会读到 **0.344 ms** —— 落在区间下沿，
差 2 倍。凡是拿来下结论的数字都用中位数，别用一次采样。

**自洽校验**：段和（首帧）或段**中位**和（稳态）> 该帧（中位）× 1.05 + 0.5 ms 就 `LOG_W`
（防仪表盘自己错）。这条**真响过一次**（2026-10-15 首版）：当时拿"多帧累计"去比"重复帧中位数"，
立刻被抓出来 —— 所以现在每条都跟**它自己那一帧**比。

**实测（2026-10-15，Apple M5，1280×720，zoom 25，`ADOCAO_CAPTURE_FRAMES=6/4/3`）**：

| 谱（tile）| drawn | 首帧总 | vis | **cull** | draw | trails | planets | other | **稳态帧（中位）** |
|---|---|---|---|---|---|---|---|---|---|
| MYC 677 万（3385456）| 178 | 41.87 | 7.55 | **0.008** | 16.35 | 7.07 | 6.10 | 4.76 | **0.679 [0.31..0.78] n=9** |
| Singularity 100 万（249416）| 3765 | 41.03 | 0.48 | **0.390** | 21.30 | 7.27 | 5.60 | 5.98 | **0.692**（单样本） |
| primer 6841 万（34205538）| 127 | 41.64 | 0.00 | **0.032** | 26.01 | 7.33 | 5.32 | 2.94 | **0.636**（单样本） |

（后两张是单帧口径，只作对照；要下结论请按上面的多帧方式重跑。）

**关于时钟**：本文件用的是 `std::chrono::steady_clock` —— **Windows/MSVC 上它就是 `QueryPerformanceCounter`**
（macOS/libc++、Linux 是 `clock_gettime(CLOCK_MONOTONIC)`）。要显式 QPC 得把平台头引进 `core/`，
而 `scripts/check-core-purity.sh` **禁** `core/` 里的 `windows.h`（真要做得放 `app/` 再注入）——
**但换时钟换不来精度**：实测读钟成本（Apple M5）`mach_absolute_time` **5.0 ns/次**、
`clock_gettime` **16.9 ns/次**；一次 RAII 段 2 次读钟 = 10~34 ns，一帧 7 段 ≈ 0.07~0.24 µs，
相对 0.68 ms 的稳态帧是 **0.01~0.04%**。瓶颈在**采样**（已用多帧统计解决）与 **CPU/GPU 口径**（见 §6）。

**两条最重要的读数**：

1. **首帧的 `draw` / `trails` / `planets`（16~26 / 7.3 / 5.3~6.2 ms）是一次性的**（着色器编译、
   纹理与缓冲首次建立）。所以**别拿"首帧 37~42 ms"当帧预算** —— 稳态是 **0.34~0.69 ms**，其中大头是
   `other`（ImGui/swap/present）。此前 ④ 的 A/B 表里那些"36/38/156 ms"都是**首帧**口径，差值仍然有效，
   但表述要按"首帧"读。
2. **`cull` 那一格才是剔除的账**：块索引之后首帧 **MYC 0.011 / Singularity 0.390 / primer 0.032 ms**；
   同为首帧口径，**全扫时代**约为 **2~3 / 0.45 / 113 ms**（`ADOCAO_CULL=scan` 可复现）。

## 2. 整帧口径（`--capture`）

`--capture` 打一行：`render_ms=… first_ms=… frames=… drawn=…`。
`render_ms` 默认 = **首帧**；设 `ADOCAO_CAPTURE_FRAMES=N` 后 `render_ms` = 重复帧的**中位数**、
`first_ms` = 首帧。像素只取最后一次（同一状态重复渲染，逐位相同 —— 抓帧自检也在守着）。

## 3. 内存口径（两把尺子，别混）

* **峰值**：`/usr/bin/time -l build/ADOCAO <谱> --capture …`。**大谱的峰值在加载期**
  （MYC 212 B/层 → 1429 MB），渲染侧的改动在峰值上**看不见**（实测：删掉 4 B/砖 的数组，峰值只动 0.6 MB）。
* **常驻**：`app/ProcMem.hpp::currentResidentBytes()`（macOS `task_info(MACH_TASK_BASIC_INFO)` /
  Linux `/proc/self/statm`），`LevelScene` 在建 `TileMesh` 前后各打一行
  `RSS: 建 TileMesh 前 X → 后 Y（+Z，常驻口径）`。**这才是 D1 要的口径**。
* **加载期瞬态**：峰值被后面的更大峰值盖住时，用 `operator new` 记账
  （`docs/scale-1e8-to-2e9.md` §5）或系统的 `heap`/`vmmap`（§4）。

## 4. 系统 profiler（macOS；`sample` 本机验过）

```bash
# 采样栈（需要进程活着够久：本 app 是 sleep 帧控，随机采多半落在 idle —— 采"加载期"或重帧）
sample <pid> 3 -file build/prof/sample.txt          # 验过：退出码 0，文本调用树 + "Sort by top of stack"
# 内存归因（替代当年手写的 operator-new 记账）
vmmap -summary <pid> | head -30
heap <pid> | head -40
leaks <pid>
# Instruments（Time Profiler / Allocations / Leaks）
xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 20s -o build/prof/t.trace
```

**边界**：① 都要一个**活着的**进程 → 短命的 `--capture`（<1 s）采不到，得先用
`ADOCAO_CAPTURE_FRAMES=200` 拉长、或直接跑 GUI 播放；② 本 app **帧控会 sleep**，随机采样容易只看到
`__workq_kernreturn`/`mach_msg2_trap` 这类 idle 栈（实测就是这样）——要采**加载**（`adocao image` 那条
只加载不出图的路）或正在 pan 的帧；③ `xctrace` 在本机**未验证**（可能需要提权/开发者工具授权）。

## 5. 专用探针（回答"策略该选哪个"，不是"现在多快"）

| 探针 | 回答 |
|---|---|
| `./build/tests/adocao_cull_cost_probe <谱> --blocks 64,…` | 剔除策略（全扫 / 层号窗口 / 块 AABB / 网格+排序）与块粒度的**候选数/耗时/索引体积** |
| `./build/ADOCAO pack <谱> out.adocao --codec-report` | 容器每一列"长什么样"（值集/环绕率/整数域 Δ² varint/网格）|
| `./build/tests/adocao_window_test --walk out.adocao` | 窗口化加载路径的**峰值 RSS**（与层数解耦的证据）|
| `ctest --test-dir build -R cull_index` | 块索引与全扫**逐元素相同**（含反面对照）|

## 6. 已知盲区（要补，别当已经能测）

1. **抓帧不 pan** → 同一视锥重复渲染，稳态里 `cull=0`（列表缓存命中）。真播放每帧视锥都在变，
   所以"**每帧**剔除成本"目前只能读首帧那一格。要测连续 pan 的每帧成本，得加一个"平移抓帧"模式
   （在 `--capture` 里让相机按固定步长走 N 帧、每帧都重建列表）。
2. `profile` **只覆盖渲染线程**：异步建 mesh / 加载期在别的线程（用 §3 的口径）。
3. `other` 是个**混合桶**（ImGui + swap + present + 驱动等待），要拆它得上 `sample`/`xctrace`。
4. **`draw` 段量的是 CPU 提交时间，不是 GPU 时间**（`glDrawElementsInstanced` 是异步的，帧末的
   `glFinish` 被算进 `other`）。要看 GPU 真耗时得上 **`GL_TIMER_QUERY`**（`glBeginQuery(GL_TIME_ELAPSED)`；
   GL 3.3+，本机 4.3 ✓），代价是查询结果要**延后一帧**回读（双缓冲查询），且查询本身有开销。**未做。**
