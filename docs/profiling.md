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

**Profiler 的专用文件（2026-10-17）**：开 `ADOCAO_PROFILE=1` 后，除了日志里的 `soak` 行，
还会写一份 **`ADOCAO.prof`（CSV，每行立即 flush，运行中就能读）**：

```
# ADOCAO profiler v1 (CSV; 每行立即 flush)
kind,run,t_s,frames,work_p50_ms,work_p95_ms,work_p99_ms,work_max_ms,
frame_p50_ms,frame_p95_ms,frame_max_ms,over_16_7,over_pct,drawn_med,drawn_max,rss_mb,
segment,seg_p50_ms,seg_min_ms,seg_max_ms,note
interval,…   每个采样间隔一行（本间隔的统计）
end,…        退出时整段统计
segment,…    每个阶段（cull/upload/draw/trailSample/applyFrame…）的分位数
frame,…      整帧 p50/p95/max
```

* 文件位置：默认写在 **`ADOCAO.log` 同一个目录**（`Logger::path()` 的父目录 —— 也就是
  exe 旁，macOS `.app` / 系统日志目录那套回退规则一样）；`ADOCAO_PROFILE_FILE=<path>` 可整体改
  （相对 = 当前目录）。日志里的 `profiler: 写入 …` 打的是**绝对路径**。
  *为什么改*：原来默认是"当前工作目录"，用户从 `.bat`/快捷方式启动时文件落在别处，
  看起来就是"profiler 什么也没留下"（2026-10 真报过）。
* 采样间隔：`ADOCAO_PROFILE_INTERVAL_MS`（默认 10000，最小 100）——只影响行频率，
  逐帧采样与 `end`/`segment` 行不变。
* `ADOCAO.log` 保持原样（启动/加载/错误 + 同样的 soak 行）；`ADOCAO_LOG_FILE` 仍然可以
  整体改日志路径 —— 给了它，profiler 也跟着落到那个目录。
* **转 CSV**：`python scripts/prof_to_csv.py ADOCAO.prof`（tidy，全列）或 `--split`
  （按 `kind` 拆成 `interval/segment/frame/end` 四个 CSV，各自只留非空列）；`--summary`
  打印摘要（interval 中位/最差帧、segment 按 p50 排序）。纯标准库，Windows 上用
  `py -3` 即可；包里的 `prof2csv.bat` 是它的双击包装（默认对同目录 `ADOCAO.prof` 跑 `--split`）。

**量真实上限先关帧控**：本 app 默认按 320 fps 节流（`targetFrameTime = 1/320`），所以 `frame`
p50≈3.13 ms 是**故意**的。要量机器真实上限 / 不受节流影响的 `frame`，设 **`ADOCAO_UNCAPPED=1`**
（`GameWindow::run` 里把 target 设 0；日志会打一行"帧率上限已关闭"）。`=0` / 空 = 保持节流
（按首个字符判断，和 `ADOCAO_PROFILE` 同口径；2026-10-18 之前是"设了就开"，`=0` 也会关掉节流）。

段名与放置：

| 段 | 位置 | 量的是 |
|---|---|---|
| `setup` | `GameWindow::render` 帧头（letterbox + 两次 `glClear` + `setAspect`）| CPU 提交 |
| `vis` | `LevelScene::render` 开头 `updateTileVisibility`（TrackVis 起隐/显）| CPU |
| `cull` | `TileMesh::draw` 内的 `ensureDrawList`（④ 的筛块 + 块内判 + 拼表）| CPU |
| `upload` | `uploadIfNeeded`（实例缓冲上传）| CPU 提交 |
| `draw` | `glBindVertexArray` + `glDrawElementsInstanced` | **CPU 提交**（异步，不是 GPU 时间）|
| `trails` / `planets` | `LevelScene::render` 的拖尾 / 行星 | CPU 提交 |
| `glfinish` | 帧末 `glFinish()` | **这一帧的 GPU 等完**（提交之外的全部）|
| `other` | **不是段**：`帧自量 − 段和` | 剩余（应≈0；不≈0 就说明有东西没立段）|

**`other` 曾经长到 97%，那是我的错**（2026-10-15 当天被指出）：当时没给帧末的 `glFinish` 立段，
于是"等 GPU 做完这一帧"整块掉进了桶里 —— 而那恰恰是稳态帧的全部。立段之后 `other ≈ 0`，
真账变成"**GPU 96% vs CPU 提交 4%**"（见下表）。**规则：`other` 只要不接近 0，就是漏立了段，先补段。**

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

MYC 677 万层（tile 3385456，`ADOCAO_CAPTURE_FRAMES=10`）：

| 口径 | setup | vis | cull | upload | draw | trails | planets | **glfinish** | other | 帧 |
|---|---|---|---|---|---|---|---|---|---|---|
| 首帧 | 0.023 | 6.563 | 0.009 | 0.019 | 16.119 | 7.041 | 6.510 | 1.078 | 0.013 | 37.376 |
| **稳态（中位 n=9）** | 0.003 | 0.000 | 0.000 | 0.008 | 0.013 | 0.004 | 0.003 | **0.650 [0.259..3.421]** | −0.004 | 0.677 [0.281..3.472] |

读法：稳态帧 **≈96% 是 `glfinish`（GPU 等完）**，CPU 提交（setup+vis+cull+upload+draw+trails+planets）
合计只有 **0.027 ms**。首帧那 16.1/7.0/6.5 ms 的 `draw`/`trails`/`planets` 是**一次性 GL 建立**
（着色器编译、纹理/缓冲首次上传），别当帧预算；`cull=0.009`（首帧）才是剔除的账。
`glfinish` 的**区间很宽**（0.259~3.421）—— GPU 侧噪声比 CPU 侧大得多，所以更要看中位数而不是单采样。

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

## 1.5 整程浸泡（"100% 从头到尾"）

`--capture` 只能看单帧；"从头到尾跑完整首"要看**整程**统计。开 `ADOCAO_PROFILE=1` 后在**真实播放环路**
里每 10 s 打一行、结束时再打一行总结（关掉时零开销）：

```
soak(结束): 帧 65027｜work p50=0.19 p95=0.48 p99=0.71 max=30.54 ms｜
            frame p50=3.12 p95=6.15 max=35.35｜>16.7ms 2 帧(0.00%)｜drawn 中位 629 峰值 99018｜常驻 743 MB
soak(结束-分段): cull=0.008[0.000..0.883] upload=0.001 draw=0.009 trails=0.003[0..8.321] planets=0.002[0..6.326]
                other~=0.165 frame=0.192[0.015..30.535] n=65026
```

口径：`work` = `update+render`（不含 swap/pacing），`frame` = 整圈**含** `glfwSwapBuffers` 与帧控等待
（本 app 的目标是 320 fps，所以 `frame` p50≈3.12 ms 是**故意的节流**，不是性能上限）；
`>16.7ms` 那一列才是"掉不掉帧"。`drawn` 是每次 draw 的实例数。

**实测（2026-10-15，Apple M5，1280×720）**：`Won't You Make a Song with Me (no three planets)`
（6,377,689 层、205.49 s；`--auto-play --trail-sample-rate 4000 --force-hitsound Kick`；
视距由用户滚轮设定，未固定）：

* **65,027 帧跑完整首，`work` p50=0.19 / p99=0.71 ms，`>16.7 ms` 只有 2 帧（0.003%）**；
  全程 `cull` 中位 **0.008 ms**（④ 的块索引在连续平移下也是这个量级），`upload=0.001`、`draw=0.009`。
* `drawn` 中位 **629**、峰值 **99,018** —— 峰值那次仍然没掉帧（`>16.7ms` 的 2 帧与它不同时）。
* 常驻 **732 → 743 MB**（跑整首只涨 11 MB；对比：加载峰值 ~1.4 GB、建 mesh 前 1091 → 后 1151 MB，
  说明加载期临时量在播放开始前就还掉了）。
* 音频（`--force-hitsound Kick`）：`Synthesized 6377687 hits from 1 groups into 215.4s buffer`
  —— 6.38M 次打拍音一次合成 ≈76 MB PCM（44.1k 立体声 float）。**这就是"音频墙"那笔常驻 PCM。**

**从某个中途点开始 + 到点自动退出**（"定点整段"）：

```
ADOCAO_PROFILE=1 ADOCAO_EXIT_AFTER_MS=20000 build/adocao <谱> \
    --music <音乐> --start-at 49 --width 1920 --height 1080 --trail-sample-rate 64000
```

`--start-at SEC` = 选砖后按空格（时钟/音乐 seek/音色游标一起跳）；`ADOCAO_EXIT_AFTER_MS=N` 会
**正常退出**（`windowShouldClose`），这样 `reportSteady()` 的 `soak(结束-分段)` 才不会被 SIGTERM
丢掉。2026-10-17 起分段里还有 `playback-sync` / `applyFrame` / `camera`（`GameWindow::update`）与
`trailWindow` / `trailSample`（`LevelScene::applyFrame`），能直接区分"时间步进 / 拖尾取样 / 相机"。

### 逐采样路径别碰 block 0（2026-10-17）

The Moon（60 万层，BPM 14817）**结尾段**每帧 1.06 s、沙滩球。分段计时直接给出
`trailSample=1067 ms`（8194 个采样点 → **128 µs/点**；正常是 0.1 µs/点）。

`sample` 显示热点在 `totalAngleAt` / `angleDirBefore`（**假线索**：`positionAtTile` 被内联后
符号归到了它们身上）。真因是 `positionAtTile()` 顶部那句 `if (t < timeline.startTimeAt(0))`：
`startTimeAt` 走**单块缓存**，每个采样点先把它撕回 block 0，紧接着的 `startTimeAt(tileIdx)`
又撕回来 → **每点两次 4096 层重算**。t=20 时 tileIdx 就在 block 0，所以看不出来；t≥90 就炸。

修法：`Timeline::firstStartTime()`（建表时存的标量），逐采样路径不再访问 block 0。实测
`sampleTrailRange` 1050 ms → **0.4~1.1 ms**（同一 8194 点），整帧 1069 → **2.2 ms**。

**通用规则**：单块缓存的窗口访问器（`startTimeAt` / `tilePositionAt` / `tileDirectionAt`）在
**逐采样 / 逐砖循环**里只能命中"当前块"；跨块访问一次就是一次 4096 层物化。常量（首层时间、
总时长）要预先存成标量。

**注意**：这张谱的 `settings.hitsound` 是 `"None"`（谱师防普通播放器炸）—— 要听/要测打拍音必须
`--force-hitsound Kick`。另外这类谱的打拍音**本身就是曲子**，别再加 `--music`（会叠一层）。

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

## 4.5 跨版本对照（"是不是退化了"该怎么量）

老标签现在也能编出来量（2026-10-15 试通 v2.1.0）。步骤与**三个必须的修补**（都在一次性副本里做，
不进仓库）：

```bash
mkdir -p build/wt-v2.1.0 && git archive v2.1.0 | tar -x -C build/wt-v2.1.0
cd build/wt-v2.1.0
# ① 当年的 glad 要系统的 GL/glcorearb.h（今天没有）→ 拉两个头到本地 include
mkdir -p build/glinc/GL build/glinc/KHR
curl -sS -o build/glinc/GL/glcorearb.h  https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GL/glcorearb.h
curl -sS -o build/glinc/KHR/khrplatform.h https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/KHR/khrplatform.h
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-I$PWD/build/glinc" -DCMAKE_C_FLAGS="-I$PWD/build/glinc"
# ② 老 CMake 没链 macOS 的 OpenGL.framework → 补上
cmake -S . -B build -DCMAKE_EXE_LINKER_FLAGS="-framework OpenGL"
# ③ 当年窗口创建失败后会 glfwSetWindowPos(NULL,…) 崩掉；要量稳态就在
#    `LevelLoader.cpp` 的 `releaseMemory()` 之后插一个 env 门控的驻留点（SCRATCH ONLY）
cmake --build build --parallel 8
```

然后用**同一张谱、同一个 `--no-hitsound`、同一口径**（`vmmap -summary` 的 physical footprint +
`heap` 的 live）各量一次。**实测（MYC 原版 6,770,912 层，Apple M5，加载后静置）**：

| | v2.1.0 | v7.1.0（今） |
|---|---|---|
| 稳态 footprint | **1.4 GB** | **675 MB** |
| 峰值 footprint | **4.9 GB**（`actions` 是 nlohmann DOM）| **1.2 GB** |
| live 分配 | **928 MB** | **528 MB** |
| 最大单块 | 413 MB（未定名）+ `tiles`(32 B/层) 207 MB | `tiles`(24 B/层) 155 MB + `FastAction` 94 MB |

→ **"退化"不成立**：同口径下今版稳态/峰值都只有 v2.1.0 的一半左右。用户记的"Windows 300-400 MB"
更可能是 ①Mem Reduct 挤过之后的**工作集**（≠ macOS `footprint`）、②Windows 的**私有工作集/提交大小**
口径、或 ③另一张谱/另一个版本 —— 要坐实必须固定"版本 + 谱 + 口径"三件事再比。

## 4.8 两个 env 门控的**数据转储**（③ 窗口化排查用）

* `ADOCAO_MESH_DUMP=1`：`TileMesh::build` 末尾把 `n / shapes / chunks / tileShape / posX / posY /
  iconBits / chunkX / chunkY` 的 FNV-1a 打一行 —— 用于"按块物化 provider 是否改变了网格装配"的 A/B
  （改输出前先比这一行，能立刻把差异关进/关出网格）。
* `ADOCAO_TRAIL_DUMP=1`：拖尾采样处打 `startTime/rate/t/点数/红蓝点哈希`（并前 6 个蓝点全精度）——
  用于"差异只在拖尾"时的定位（实测就是这样把 21 像素差异定到"蓝行星开窗起点 2 个采样"）。
* `ADOCAO_TRAIL_INPUTS=1`：`sampleTrailRange` 前 3 个采样打**全部输入**（tileIdx / tt / startTimeAt /
  pivot 位置 / startAngle / totalAngle / 上一层 direction），把差异从"23 个量里哪一个"直接缩到一格。
  实测它一眼指出 `totalAngleAt` 不同 ⇒ `angleData` 被 `releaseMeshTemporaries()` 释放过（先在 bug）。
两者默认关、零开销；开关本身不改行为。

## 4.9 `angleData` 紧凑存储的账（③ 切片 4）

`releaseAngleData()` 会把 `angleData` 编成"取值字典 + 位打包下标"（`core/level/AngleStore.hpp`），
日志一行给出账：`angle-store: 52 个取值 → 0.75 B/层（51.7 MB → 4.8 MB）`。
判定"要不要换"看 **B/层 vs 8**：真实谱面 0.75 B/层（MYC 52 个取值、The Moon 49 个），
但**小谱/取值集大的谱**会被字典本体拖累（`diag_glide` 1024 个取值 → 9.25 B/层 > 8）→ 护栏直接不换。

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
4. **`draw`/`trails`/`planets` 量的是 CPU 提交时间**（异步），GPU 侧现在只有帧末 `glfinish` 一个
   **总数**。要**按 pass** 拆 GPU 耗时得上 **`GL_TIMER_QUERY`**（`glBeginQuery(GL_TIME_ELAPSED)`；
   GL 3.3+，本机 4.3 ✓），代价是查询结果要**延后一帧**回读（双缓冲查询），且查询本身有开销。**未做。**
5. `run()` 的真实播放环路里还有 `glfwSwapBuffers`（vsync/pacing 等待）——它**不在** `--capture` 的口径里
   （抓帧不 swap），所以 `--capture` 的帧时**不含** vsync 等待。
