# TODO

> **当前目标见 `docs/2e9.md`**（21 亿层：内存与 n 解耦 / 任意 seek / 不掉帧，并把 32 位假设清算干净）。
> 本文件只是任务清单 —— 目标、DoD 与阶段顺序以那份为准。

> 未完成清单已定稿（移除：~~GPU compute culling~~——2.0.0 起已不需要，彻底删除；~~Decoration~~）。
> MoveTrack 保留，归入「遥远的未来」，仅记录不排期。已完成项保留在文末作历史。

## 并行模型（`docs/parallelism.md`，2026-10-16 拍板）

- [ ] ① 调度器骨架（`core/util/Scheduler`：模板 `parallelFor` + 两条车道 + 嵌套护栏 + `ADOCAO_JOBS=1`）
      并把 `TileMesh` 的剔除迁过去；**窗口物化改成并行预派生只读表**（现在被"主线程 pin"串行化，
      `sample` 量到 `materialize` 占主线程 36%）→ 预期宽视野重建帧 160~240 ms → 20~30 ms
- [ ] ② 收编 `LevelData`（每次调用新建池 ✗）与 `MapExport`/`PngBand`（自造线程 ✗）→ 消过订阅
- [ ] ③ 位置提供者加 **512 层细锚点**（现在每窗要走 4096 层 trig）→ 再 ~8×
- [ ] ④ 后台车道 + 取消（换谱时掐掉旧网格构建/预载）
- [ ] ⑤ 加载期并行（位置/时间/图标/索引/角度存储）
- [ ] ⑥ per-worker 暂存（热点内不再分配，边际优化）
- [ ] LOD 的第一遍加"扫描预算 + 外推"（现在 p50 12~21 ms 全花在扫候选上；正解 = 扫够 N 块就停、
      按 `已见 × 候选/已扫` 外推，再进第二遍；确定性不变）
- [ ] 新增 `tests/scheduler_determinism_test.cpp`（`JOBS=1/2/4/8` 输出逐字节相同）

## 未完成

### 容器 / 格式
- [x] **`.ilytx` 读取**（Re_ADOJAS 自研容器，2026-10-16，见 `docs/ilytx.md`）：xz(tar(ilybin)) →
  翻译成等价 `.adofai` 明文，复用唯一解析路径；ctest `ilytx` 逐位比对 + 出图逐字节相同。
- [ ] `.ilytx` 的**导出（写）**未做：要先跟对方规范对齐，且 ADOCAO 已有列式 `.adocao`。
- [ ] `.ilytx` 内置成员 `audio/…`、`decor/…`、`bg/…` 目前**跳过**（要定"抽到哪、何时清"）。
- [ ] 裸 tar（未压缩的 `.ilytx`）不认 —— Re_ADOJAS 导出总是 xz 压缩，等真出现再说。

### 打拍音
- [ ] ColorTrack（COLOR_FUNCS / FLOOR_FUNCS / RecolorTrack）
  - 蓝图决策 8/9：只做静态；颜色逻辑归 render；几十万事件走 GPU/GLSL。

### 渲染
- [x] **TileMesh 改 GLSL 几何 + 图标并入**（2026-10 完成，蓝图决策 10）：
      1 实例 = 1 砖、几何在 VS 按形状表展开、图标并入实例流；draw 次数 4838/8192/294 → **1**。
      顺手修掉一个 bug：旧顺序"砖 → 拖尾 → 行星 → 图标"让不透明的图标把半透明拖尾擦掉一块
      （图标现在跟砖一起画，在拖尾之前；`*_under_trail` 8 个状态覆盖，老顺序会红 13700~29241 px）。
      三层验收：`tests/tile_expansion_test.cpp`（CPU 逐位 3.3e7 坐标）、
      `tests/geom_probe_test.cpp`（GPU 逐位 4.7e7 坐标）、`scripts/capture-gate.sh`（51 状态逐字节）
- [ ] 备选：形状表改成"每槽位直接存顶点"（fetch 模式）—— 只有"不想把 GL 上下文提到 4.1（`fma()` 需要
      GLSL 4.00+）"或"换编译器后 CPU/GPU 逐位对拍对不上"时才需要；架构（实例流/canonical 表/图标并入/
      剔除/测试）都不变，只换 `packShapeTable` 的内容与 VS 主体

- [x] **CPU 几何作为 3.3 回退路**（2026-10-17 落地）—— 但落地方式与下面这条老计划**不同**：
      老计划是"恢复 74cc5e0 那版每形状一次 draw 的 CPU 顶点汤"，那会丢掉窗口化派生 / 两级剔除索引 /
      LOD / 容量闸，等于把 2026-10-15~16 的成果分叉成第二套渲染器 ✗。实际改成**只换坐标来源**：
      `assets/shaders/tile_cpu.vert`（`#version 330 core`）不在 GPU 上算几何，而是查
      `TileShape::expand()` 在 CPU 上展开的槽位表（RG32F，每形状 175 texel）。一支 VS、同一个
      VAO/实例流/索引表/绘制序/剔除/LOD 全都共用 ⇒ 出图逐字节相同：`ADOCAO_CPU_GEOMETRY=1
      scripts/capture-gate.sh check` → **51/51 逐字节**；单帧 A/B（angles360、4838 形状）也相同。
      选择靠**试编 410**（失败才切，不按版本号猜）；`tests/tile_geometry_test.cpp` 加了
      "回退路必须用 `TileShape::expand`"的护栏。本机造不出真 3.3 上下文（macOS 地板 4.1），
      所以端到端只能靠强制开关验。以下是原计划的细节，仅作历史参考：
      ~~**CPU 几何作为 3.3 回退路**（2026-10 用户拍板方案 B：4.1 拿不到 → 3.3 core 上下文 + CPU 几何）~~
      已落地的前提（两步行为中性、已验）：`tile.frag`/`highlight.frag` 降到 `#version 330 core`
      （**只有 `tile.vert` 必须留 410** —— 它是唯一用 `fma()` 的）；`app/GameWindow.cpp` 有
      4.1 → 3.3 的重试链（`ADOCAO_GL_VERSION=4.6` 可验重试真的会触发）+ `m_glMajor/gpuTileGeometryUsable()`。
      平台事实：macOS 的核心 profile 地板是 4.1（请求 3.3 也只会给你 4.1），**本机造不出真 3.3 上下文**，
      所以端到端只能靠"强制 CPU 路"验 —— 别以为在这台机器上点一下 3.3 就算验过。
      剩下的（**从历史恢复，不要新写** —— 位精确正是恢复出来的最大理由）：
      1. `render/TileMeshCpu.{hpp,cpp}` ← `git show 74cc5e0:render/TileMesh.cpp`（改造前最后那版：
         逐顶点 `(pos, type)` 交错 stride 4*float、逐实例 offset/颜色/opacity、`buildIcons()`、每形状一次 draw）；
      2. `assets/shaders/tile_cpu.vert` ← `git show 0998a0d:assets/shaders/tile.vert`（`#version 330 core`，
         `aPos/aType/aInstOffset/iColor/iBgColor/iOpacity`）；同步 `render/Shaders.hpp` 内嵌串，
         并把 `scripts/check-shader-fallback.sh` 的配对数 7 → 8；
      3. `render/TileGeometryReference.cpp` 从"只进测试"改成也进 `adocao_render`（改 `render/CMakeLists.txt`
         那句注释 —— 它现在会挡住回退路）；
      4. 切换用**能力探测**（试编 410 的 tile 管线，失败才切 CPU 路），**不要按版本号猜** —— 同 `lzma_mt`
         那个教训；再加 `ADOCAO_CPU_GEOMETRY=1` 强制切（验收 + 救急）；
      5. 护栏 `tests/tile_geometry_test.cpp:154` 改成"旧几何调用只许出现在 `TileMeshCpu.cpp`"
         （保留"防无意退回旧路"，放行**故意**的回退）；
      6. 验收：`ADOCAO_CPU_GEOMETRY=1` 下 `scripts/capture-gate.sh check` 必须 **51/51 逐字节相同**。
         两个坑：(a) 回退路必须按 `m_drawOrder` 遍历（深度 24 bit 下相邻砖同深度，顺序变就变赢家，
         实测 102~278 px）；(b) `74cc5e0` 那版把图标画在拖尾**之后**（已知 bug，`d1bfc47` 才修），
         恢复时必须一并改成"图标在拖尾之前"，否则 8 个 `*_under_trail` 状态会红 13700~29241 px。
- [x] ~~备选：按 mode 分 4 次 draw~~ —— **2026-10 量过，不值得做**：稳态帧时间
      （`render()` + `glFinish`，30 帧取中位；`ADOCAO_CAPTURE_FRAMES=30`）
      145 实例 0.37 ms → 2509 实例 0.67 ms → 5564 实例 0.95 ms → **17569 实例 1.09 ms**，
      而 320 fps 的预算是 **3.13 ms**。canonical 表取并集确实多跑不活动的 part（约 2~7× 的
      顶点调用数），但每次调用只有几个指令且零片元 —— 即使老实现（顶点数更少）快 2~4×，
      绝对差也在 1 ms 以内，换不回"一次 draw + 绘制顺序已逐位对拍"这两条。
      第一帧另有约 35 ms 的一次性开销（形状表纹理上传 + VAO/pipeline 首次绑定），稳态里没有。

### 功能
- [x] 无头出图（地图全景）：`--map out.png [--map-size WxH] [--map-bg HEX|transparent]`
      真无头（不碰 GL/窗口），整条路径等比铺满 —— 6.77M 层的 MYC 出 4096² 只要 6.3 s
- [ ] 无头视频（构想）：那个 fork `rech114/ADOCAO` 用离屏 FBO + ffmpeg 管道做过（Apache-2.0，可参考）
  - core 侧已有先例：headless 探针直接跑 PositionSolver/sampleTrail（见拖尾调研用的 trailprobe）；缺口在离屏 GL 上下文（headless GLFW / EGL）+ 渲染循环的时间源——无音频设备时不能依赖音频回调推进 m_readCursor，需固定步长驱动
- [ ] MoveCamera（5 种 relativeTo 模式）—— **八步依赖链 2026-10-17 全部落地，详见 `docs/camera.md` §12**
      ✅ 已完成（都有独立验收）：32 个缓动（`core/util/Easing`，金标由 `Easing.ts` 直接跑出来，32×25 点）、
      `angleOffset`、`RepeatEvents` 展开（Beat/Floor + `entryBeat` 的 -1 哨兵 + 多 tag）、`MoveCamera`
      富事件（快路径/分块并行/流式窗口/DOM 四条路径 + `.adocao` 段 9/10，容器往返逐字段对拍）、
      相机时间线（`tileStartTimes[floor] + angleOffset/180*60/bpm`，`(time,id,文件序)` 排序）、
      两层 `CameraDirector`（**跨语言逐帧金标**：同一批事件喂给 Re_ADOJAS 的 `CameraController.ts`
      与 C++，8 个场景 × 每 15 帧，容差 1e-9）、`render/Camera` 旋转（`proj = ortho·R(-θ)`）、
      剔除按旋转矩形 AABB 扩边、`--capture-camera`、暂停时导航反旋转（`screenToWorld`/拖动）。
      剩下的是**决策与接线**，不是移植：
      ① **视野基准**：我们 12 砖全高 @100 vs Re_ADOJAS 5.47 → 所有 zoom 事件的**视觉尺度差 2.19×**
         （语义/数值是对的）。改基准要重存全部像素基线，需拍板 —— `docs/camera.md` §12.2 第 1 行。
      ② `holdOffset`：控制器接口已就位（`setHoldOffset`），app 恒写 0，等 Hold 事件落地后接上。
      ③ 抓帧门槛的运镜状态：`tests/charts/camera_basic.adofai` 已备好（rotation / tween 中点 / 终点三个
         时刻），基线待 store：`capture-gate.sh store --only '^camera_'`（写 `~/Projects/.ADOCAO` 要一次提权）。
      ④ `.adocao` 的 `Preserved` 段仍未做 → MoveTrack/Decorations 等**未知事件打包后仍然会丢**
         （运镜已经不再受影响，但它和运镜是同一类问题）。
      ⑤ 砖上图标在 `rotation ≠ 0` 时**跟着世界转**，Re_ADOJAS 会反向补偿保持正立（`updateTileChildSpriteRotations`）。
         现有像素基线全在 0°，抓不到；要对齐得把图标拆成独立绘制 + 传逆旋角（见 docs/camera.md §12.2）。
- [ ] PositionTrack：relativeTo、rotation、scale、opacity、stickToFloors
      （`justThisTile` **已完成** 2026-10-11：官方语义 = **只作用于本层**（点加）；缺省/`"Disabled"` 才是
      "本层及之后所有层"。消费侧在 `applyPositionTrackOffsets()`，回归用例 `tests/position_offset_test.cpp`。
      三份来源一致：编辑器指南 "Only change this tile" + Re_ADOJAS 的 `pointX[floor] += …` +
      在 The Moon 上量出的"花瓣环半径 ÷ 星半径 = 1.5~2.2"（点加预测 1.46/2.28 ✓，累积是 3~5 ✗）。
      教训：定结论要落在**尺度无关的可量值**上，别用"哪张图更像"。）
- [ ] PositionTrack 剩下的官方对照点：状态类字段（rotation/scale/opacity/stickToFloors）在 `justThisTile` 时
      是否**不更新持续状态**（编辑器指南说 Enabled = "only affect the event tile"）；`relativeTo` 的目标层要用
      官方 `vector`（累积偏移）—— 见 `~/Repos/Re_ADOJAS/src/lib/Player/PositionTrackManager.ts`
- [ ] **PositionTrack 同层多事件只生效最后一个**（潜在 fidelity bug）：`tilePositionOffsets` 是
      `unordered_map<int, TilePositionOffset>`（`LevelData.hpp:101`），而 `LevelData.cpp:1558` 是**赋值** ——
      同一层出现两个 PositionTrack 时前者被覆盖，于是"文件里最后一条赢"。
      官方向照：真值应是**每个事件各作用一次**（Re_ADOJAS 的 `pointX[floor] += …` / `bitAdd` 是逐事件的）。
      顺带：`applyPositionTrackOffsets()` 里"同层多个事件累加"那句注释与实际数据结构不符。
      影响面：v3 checkpoint 的不变量 3（边界层 `floor == i0` 撞上多事件会少算）。
- [ ] **ScaleRadius**（继承状态 + 逐层累积偏移）：官方 `scnGame 1294/1380/758` —— `radiusScale = scale/100`
      默认 1、继承到下一个事件；`val4 += direction(entryangle) * ((1 - radiusScale) * tileSize)` 逐层累加后
      加到砖位，**叠加在 PositionTrack 偏移之上**。
      参照 Re_ADOJAS（本机 `~/Repos/Re_ADOJAS`）`1edfd69`；**幂等陷阱**（他们真踩过，`5a5f5b7`）：
      这个偏移必须在**每次 `copyTo` 之后恰好应用一次**（`_tileOffsetsApplied` 类标志 + 每次 copyTo 复位），
      否则会在没有 copyTo 的路径上重复叠加 —— 探针期望 `base+[5,3]`，实际得到 `base+[9,3]`。
- [ ] **Hold / Release**（DLC 长按事件）：长按带渲染（每个 hold 一个 mesh、uniform 驱动填充）+ `holdLength`
      预计算与时长 + 手动判定（按下并保持 / 松手即判定 / 提前松手算 fail / 按键跨连续 hold 链保持）+
      相机跟随滑动（官方 `camy.SetHoldOffset` + `UpdateFollowCam`）+ 带子跟随起点砖的运行时 MoveTrack。
      参照 Re_ADOJAS `5437c09`（素材+时序）→ `b7f6e2d`（渲染）→ `b480570`/`b7de28d`（判定）→ `3b13075`（相机）→ `de517a9`（MoveTrack）
- [ ] **MultiPlanet**（N 星布局）：运行时行星池 + 逐砖活动列表 + 2..8 星布局与枢轴模拟；行星数变化时播
      `VehiclePositive/Negative` 打拍音。
      参照 Re_ADOJAS `81a04b2`（数据层/枢轴）→ `18761a9`（打拍音）→ `d809709`（池与布局）
- [ ] Bloom / Flash 特效：官方 Hidden/VideoBloom shader，`MediumAmount` 定标 = **0.28**（参照 Re_ADOJAS `5931618`/`c34dcf6`）

### 架构
- [ ] **加载期常驻内存**（"十亿层"的真正瓶颈，2026-10 实测）：MYC 走真无头 `adocao image`
      （只加载 + 解算、不建 mesh）峰值 **1436 MB / 212 B/层**（2026-10-08 复测；早期记的 1560 MB 已过时），
      而 mesh 只占 ~430 MB —— 下一仗在
      `Tile`/`tileBPMs`/`FastAction`/timeline 的窗口化与 checkpoint
  - [x] **重算侧**（2026-10-15，`archive/AdocaoWindow.*` + `ctest adocao_window`）：从 checkpoint +
        一块 angleData + 这一块的事件重算出 `Tile::position/direction`、`tileBPMs`、`tileStartTimes`，
        与全量加载**逐位 memcmp**：The Moon 147 窗 / MYC 1654 窗 **0 不一致**。见 `docs/adocao-format.md` §8.13
  - [x] **装配侧**（同日）：`FloorWindowSource` 从 `.adocao` **按块**取（段载荷按需解压 + 分块列 +
        `ActionIndex` 的稀疏列 used 起点），角度窗口/事件窗口/位置/时间/BPM **全部 0 不一致**；
        `--walk` 实测峰值 RSS：MYC（677 万层）**6.4 MB**、The Moon（60 万层）**5.6 MB**（层数 11×，峰值 +0.8 MB）
  - [ ] **消费侧（③ 剩下的）**：`LevelData` 仍是全量加载 —— 按需开窗的消费方（渲染 / 剔除 / 时间线 /
        `TileMesh` 的实例缓冲）还没接，所以 **D1 对"产品常驻内存"还不能宣称**（能宣称的只是"加载路径与 n 解耦"）
  - [ ] 每砖事件派生结构（`tileHasTwirl`/`tileHasSetSpeed`/`tileHitsounds`/`tileHitsoundVolumes`/
        `atStates`/`bookmarkFloors`）的窗口化：现在只做了位置/BPM/时间三条前缀和；
        要做的话得把 `processActions` 的逐事件 switch 抽成可复用核心（**别重抄一份**）
- [ ] 帧率上限从编译期常量改为运行时配置（LauncherConfig 选项）
- [ ] 缩放范围可配置（min/max zoom）
- [ ] 剔除边距可配置

### 容器（`.adocao` v3：无损压到尽量小）

> 设计全文见 `docs/adocao-format.md` §8（三条不变量 / 变换×后端 / 残差规则 / 分块 / 判据 / checkpoint）。
> 硬约束只有一条：**逐位无损**；于是"最小"永远由**实编字节数**裁决（候选逐一真编取最小、`Raw` 兜底、
> 编完解回来逐位 `memcmp`，不一致就丢候选）——**加候选最坏是零收益，不会让任何谱变差**。

- [x] ① `--codec-report` 加**判据统计**（纯诊断、不改行为）—— **已完成 2026-10-11**：
      `archive/AdocaoDiagnostics.{hpp,cpp}` + CLI 打印 + `tests/adocao_diagnostics_test.cpp`（ctest
      `adocao_diagnostics`，含负向对照）。落地时改了三处口径（都写进 §8.4）：①判据要**分浮点域/整数域**
      （浮点域的 Δ² 基数被量化噪声淹没，只有整数域才看得见 WrapDelta 的收益：3.29 → 1.00 B/值）；
      ②二进制网格**上限 2^-20**（任何 double 都在某个 2^-N 上，不设限则每列都"落在网格上"）；
      ③十进制网格必须**精确判定** `k/10^e == v`，不能容差。
      测试谱七张：`tests/charts/diag_{dict,glide,irregular,fourier,midspin,mixed,overlay}.adofai`
      （生成器 `tests/gen_adocao_charts.py`，确定性）
- [x] ①' **哨兵掩码先行**（②③的前置）—— **已完成**：`adocao::maskSentinels()` + 判据里 raw/masked
      两套环绕率（`diag_midspin` 实测 0.537 → 0.361，差的就是哨兵那一份）。**② 落地时仍要接上它**
      —— 判据在用，编码器还没用
- [ ] ② **`WrapDelta(mod 360)`**（滑音/螺旋：环上的量）—— 实测 13.60 MB → **0.62 MB（22×）**，
      且**没有它 `Dict` 反而更大（18.03 > 13.60）**；抖动谱退到 1.6~3.2×。
      **真实谱的量**：`diag_glide` 整数域 raw 3.29 → wrapped **1.00 B/值**、`|Δ²|max` 3.6e6 → 39
      （判据表已经能量出这个收益了，所以这条是"照判据实现"）
- [ ] ②' **新增正交整数后端 `Varint` / `RleVarint`**（**不做差分**）：v2 的 `DeltaVarint` 自己会再差一次
      （`AdocaoColumns.cpp:383`），拿它接已差分/残差流 = 二阶/三阶差分、语义错位；§8.2④ 的"zigzag varint"
      指的就是这两个新后端
- [ ] ③ **`Motif`（窗口字典）**（傅里叶/epicycle/单元重复）—— 合成实测 **75~220×**
- [ ] ④ **`Predict(k)` + 残差后端**（漂移+周期毛刺 **86×**）；残差只在**整数域**算、**闭链**、**每块复位**；
      定点缩放的溢出要显式判（`|v| > INT64_MAX/S` 就放弃该候选，别走进 UB）
- [ ] ⑤ `FixedPoint`（二进制网格 100×+）/ `XorFloat`（全精度平滑漂移 ~1.9×）
- [x] ⑥ **分块**（块自包含 + 块内自选，间隔与 checkpoint 对齐 4096）—— **已完成 2026-10-15**
      （`archive/AdocaoBlocked.*` + `ctest adocao_blocked`，见 `docs/adocao-format.md` §8.11）。
      两条待定都按"实编取最小"落地了：帧 256 KB / ≤256 块（**压缩帧与 seek 粒度解耦**）；
      **决策放在段级**（v2 是整个 Actions 段压一帧更省）→ **v3 默认文件大小与 v2 完全相同**
      （MYC 6120=6120、The Moon 3544=3544、Unity 21,079,240）。
      ⚠️ **口径要记住**：§8.3 的"分段型 1.66~3.29×"是**编码级**；段级 zstd 会把它在**存储级**吃掉
      （同族合成列实测编码级 3.87/2.19/1.70×、存储级 1.00/0.97/0.96×），真谱上分块一律更大
      （The Moon angleData 0.47×、MYC 0.29×、Unity val1 0.67×）→ 所以规则是
      "**裸数据 ≥128 MB 强制分块**（窗口化是要求不是优化）、其下取最小"。
      **中间规模（1e7 层）想要可寻址性，得先做 §8.3 选项②：字典段跨块共享 + 下标流按块**
- [x] ⑦ `Preserved`（未知成员原样保留 → ADOCAO-E 无损回写）—— **仍未做**（下面 ⑧ 已做）
- [x] ⑧ `Derived` = **checkpoint：PositionTrack 累积量优先** —— **已完成 2026-10-15**
      （`archive/AdocaoCheckpoints.*` + `ctest adocao_checkpoints`，见 `docs/adocao-format.md` §8.12；
      `adocao pack --checkpoints`，**默认关** —— 快照天生 O(n/4096)，实测 MYC 0.0045 B/层、2³¹ 外推 ≈9.8 MB）。
      **验收 = §8.10⑥ 的逐位 memcmp**：The Moon 147 条 / 重算 602,112 层 0 不一致、MYC 1654 条 /
      重算 6,774,784 层 0 不一致；**反面对照**：baseX 动 1 ULP → 1697/4096 层立刻变红。
      记录 96 B（比 §8.8 估的 88 B 多 8 B：中旋跨窗要 `sentinelRun`）。
      原设计说明（保留）：详见 `docs/adocao-format.md` §8.8；
      前置的 `justThisTile` 语义**已定案并落地**（见本文件上面那条 PositionTrack：
      **只作用于本层 = 点加**，`applyPositionTrackOffsets()` 已读 `o.justThisTile`）→
      点加是**一次性**效果、不跨块，快照里**不需要**额外字段；必需的只有"窗口事件扫描必须含 `floor == i0`"。
      **快照种子一律 f64**（`radiusScale`/`bpm`/最后一个真角度，f32 过不了逐位对拍）。字段表见 §8.8）
- [ ] ⑨ 段级后端取 `min(zstd, xz)`；零拷贝（`Raw` + 对齐直读）
- [x] ~~已实测否掉，别再提~~：赫兹隐式位图（差 13×）、手写熵编码下标流、朴素 `Δ²` 作用在 double 上
      （16.9~20.4 MB，负收益）、`Dict` 打高基数（18.06 vs 13.60）、LPC+varint 残差、`XorFloat` 打随机

### 启动器
- [ ] WinUI3：`../ADOCAO_WinUI3_Launcher` — 完成 UI + 自包含部署（独立仓库）
- [ ] SwiftUI：macOS 启动器（构想，参照 WinUI3 版立项）😂

### 遥远的未来
- [ ] MoveTrack（tile 位置/旋转/缩放动画）——仅记录不排期；届时 `core/timeline` 扩展 + `render` 消费

---

## 已完成（历史记录）

### 打拍音
- [x] Phase 1-2: per-instance color + dirty check
- [x] Multi-type support (TimestampGroup, SetHitsound)
- [x] 16-bit hard-clip matching HitSoundGenerator.exe
- [x] Case-insensitive hitsound type matching
- [x] Unknown type fallback to default
- [x] `--force-hitsound` (GUI + CLI): None → Kick
- [x] Export button in launcher + `--export` CLI
- [x] ma_decoder: AIFF, OGG, WAV, FLAC support

### 渲染
- [x] Per-instance color (iColor/iBgColor/iOpacity)
- [x] Split instance VBO: pos (3f per-frame) + color (7f static)
- [x] Visibility cache: rebuild only on frustum change (position + zoom)
- [x] Sort instances descending (Re_ADOJAS draw order)
- [x] Camera-relative offsets on CPU (removed uCam from shader)
- [x] Event icons: Twirl (purple), SetSpeed (red/blue)
- [x] depthWrite + renderOrder (Z-depth: far=200, tile Z 0~9, 755K depth steps)
- [x] `setPoints` trail: buffer under-allocation fixed (segsPerPoint=4 factor)
- [x] Multithreaded CPU culling (>= 64 groups → std::async parallel)
- [x] 中旋砖（angleData=999）：五边形（0..0.275 的方块 + 朝来路 0.275 的**尖角**，沿 −a1 平移 0.04）
      —— `render/TileGeometry.cpp::createMidSpinMesh(a1, sc)`，a1 = 上一砖 direction−180；
      参考 A4GDX → AdoCpp → Re_ADOJAS 的 `createMidSpinMesh`（同源）；**尖角是对的**（已确认，别去找"圆角/curvaturePoints"版本）。
      旧实现 `createTileMesh(eA,eA)`（ang==0 分支 = 圆 r=0.30 + 方块，"大圆+菱形像发卡弯"）已删除。
      注意：本地谱的中旋是**数字 999**（The Moon - Coal 1307 个、Singularity 442 个），不是 `"!"`

### 功能
- [x] JSON cleaner: Python literals, missing commas
- [x] angleData double precision (16 decimal places)
- [x] Memory: releaseMemory() after loading
- [x] OpenGL 4.3 upgrade + compute shader functions（GPU culling —— 已废弃：2.0.0 起不再需要，功能已彻底移除）
- [x] `--auto-play` CLI flag
- [x] DPI awareness + CPU pin to big cores
- [x] Planet trail: Catmull-Rom spline + GPU rendering
- [x] CMake options: ZOOM_LEVEL (7 levels), PORTABLE
- [x] **Re_ADOJAS 摄像机语义调研**（2026-10-16）：`docs/camera.md` —— 两层模型（rig + 跟随层）、4 条独立
      tween、5 种 `relativeTo`、`camspeed = 2 拍`。用 node 的类型擦除直接跑 `CameraController.ts` 做了
      13 组探针，把 3 个反直觉行为（`Kill(complete)` 瞬跳、无 position + relativeTo 的怪路径、切砖那一帧
      就吃掉 dt）钉成了实测金标。**这只是调研**：实现仍在「未完成 → 功能」的 MoveCamera 条目，依赖链见那里。

### 架构
- [x] GameWindow 重构为类（init/update/render 分离）
- [x] P1-P5 结构重构：P1+P2 目录搬迁+CMake 拆库；P3 PlaybackEngine → core/timeline；P4 app 拆分（CameraController/LevelScene + wizard 分页）；P5 资产并入 assets/ + g_sc 清理（见 docs/project-structure.md）
- [x] dirty check 跳过静止帧 GPU 上传（已回退：导致静止帧黑屏）
- [x] Spatial grid 加速大关卡剔除（已回退：queryGrid 从未接入 draw，复杂度和 O(n) 遍历无差距）

### 性能
- [x] Visibility cache fix
- [x] **Frame profiler（按段计时）** —— 2026-10-15 补上（`core/util/Profile.hpp`，`ADOCAO_PROFILE=1`）：
      段 = `vis / cull / upload / draw / trails / planets` + `other`（帧自量 − 段和），
      `ADOCAO_CAPTURE_FRAMES=N` 时**首帧/稳态**两条口径，自带自洽校验（段和 > 帧×1.05+0.5 ms 就 `LOG_W`）。
      实测 MYC 首帧 37.2 / 稳态 **0.34** ms、Singularity 41.0 / 0.69、primer 41.6 / 0.64；
      首帧那 16~26 ms 的 `draw` 是**一次性** GL 初始化（别当帧预算）。用法/边界/系统侧配方见 `docs/profiling.md`
  - [ ] **平移抓帧模式**：`--capture` 现在重复**同一视锥** → 稳态里 `cull=0`（列表缓存命中），
        所以"**每帧**剔除成本"读不到。要的是：让相机按固定步长走 N 帧、每帧重建列表，量连续 pan
        下的 cull/total（这是 D3 帧预算真正要看的口径）
- [x] High-FPS build option (1000fps)
- [x] Sleep-based frame pacing (CPU-efficient)
- [x] processActions() O(n+m) optimization (removed O(n*m) per-event fills)
- [x] Multithreaded CPU culling (parallel frustum test + offset compute)
- [x] precalculateTiming 多线程化

### 音频
- [x] Pause stops audio device (not just music)
- [x] Remove 100MB skip-loading-window threshold
- [x] 16-bit hard-clip hitsound mixing

### CI
- [x] GitHub Actions: Windows (MinGW) + Linux, build + release


---

## [进行中] Timeline 检查点化（2^31−1 层目标）

**目标**：把 Timeline 从"全量存储"改成"稀疏检查点 + 按需重算"。现状 Timeline ≈ 24~32 B/层
→ 2^31−1 层就是 51~69 GB，是 2.1e9 目标里剩下的三笔之一（另两笔：`FastAction` 16 B/事件、
`Tile` 24 B/层，见 `docs/scale-1e8-to-2e9.md` §7/§8）。

**依据（别重新推导，先读它）**：`../ADOCAO-E/docs/design.md` 的 §4.1「上游数据流的真实依赖图」。
那份草案是给编辑器做"编辑→增量重算"的，但它逐段判过依赖形状并给了 `文件:行` 证据，
结论可以直接拿来定"哪些量必须留检查点"：

| 派生量 | 形状 | 能否检查点化 |
|---|---|---|
| `m_tileStartTimes` | durations 的**纯前缀和**（`Timeline.cpp:210-221`，末尾整体平移）| ✅ 最干净 |
| `tileDisappear/AppearTimes` | **前缀扫描**（Phase 5，curDA/curAA/curBB/curBA + flag2 单向锁存）| ✅ |
| `tileBPMs` | SetSpeed **前缀扫描**（`*=` 或 `=`）| 是输入，多半得留（4 B/层）|
| `m_tileDurations`/`TotalAngles`/`StartAngles` | **每层独立**（Phase 2，可并行）| ✅ 按层重算 |
| `isCW` / `angleDir` | 局部（999 段内回溯；`angleDir[i]` 只依赖自己）| ✅ |

**步骤**
1. ① 进程内探针量清 Timeline 每条数组的实际字节数（B/层），别再拿 24~32 这个区间估。
2. ② 设计 `m_tileStartTimes` 的检查点方案。**硬约束**：它现在被 `findTileIndex()` 二分查找
   （播放定位、`--range`、拖尾都走它）—— 改成"检查点 + 重算"后随机访问会变贵，
   所以方案必须**同时**给出替代的定位结构（例如稀疏索引 + 段内二分）。
3. ③ 实现原型。
4. ④ **验收口径**（照 `docs/scale-1e8-to-2e9.md` 的 ⚠️ 规则）：整条下游逐位/逐样本对拍
   （`tileStartTimes`、`durations`、hitsound 时间戳）+ `ctest` + 像素门槛 51/51 逐字节。
   量加载期瞬态**不要**用 peak RSS（会被后面的峰值盖住），用 operator new 记账（见该报告 §5）。
5. ⑤ 更新 `docs/scale-1e8-to-2e9.md` 与 `AGENTS.md`。

**相关文件**：`core/timeline/Timeline.{hpp,cpp}`、`core/timeline/PositionSolver.cpp`、
`app/LevelScene.cpp`（消费方）、`tests/level_parse_test.cpp`（对拍与自检）。

**别混淆（三处"增量"说法是不同的事）**
* `../ADOCAO-E/docs/design.md` §4：**编辑后**少算（延迟），未实现；
* 本任务：**少存**（内存），机制同样是检查点；
* `AGENTS.md` 里"无须增量算前缀时间线 + 回头重算"：那是当年讨论**加载耗时**时否掉的方案，
  跟上面两件都不是一回事。

### 进展（2026-10）

**① 已实测**（一次性探针读公开访问器，MYC 6,770,913 层）：

| 数组 | B/层 |
|---|---|
| `m_tileStartTimes` | 8.000 |
| `m_tileDisappearTimes` | 8.000（只在需要 TrackVis 时才 assign，否则 0）|
| `m_tileAppearTimes` | 8.000（同上）|
| `m_tileDurations` | 4.000 |
| `m_tileTotalAngles` | 4.000 |
| `m_tileStartAngles` | 4.000 |
| `m_tileBPMs` | 4.000 |
| `m_tileIsCW` | 0.125 |
| **合计** | **40.125**（带 TrackVis）/ **24.125**（不带）|

→ 2^31−1 层：带 TrackVis **86 GB**、不带 **52 GB**。报告里"24~32 B/层"的区间偏乐观，
定稿时要换成这两个实测值。

**② 设计结论（重要，改变了优先级）**

**(a) `double` 前缀和无法"逐位"检查点化** ✗：检查点存的是 `prefix[j*K]`，
而段内值若写成 `ckpt + 段内部分和`，与原来的"从 0 起左到右累加"在低位上**不同**
（浮点加法不结合）。所以 `m_tileStartTimes`（8 B/层）只有两条路：
容忍一个**有据可查的** ≤ULP 偏差（远小于 48 kHz 的 20.8 µs，但原则上可能让导出 WAV 里
个别命中跨采样边界），或者不动它。**这条要用户拍板**，别自己放宽验收口径。

**(b) 但 Phase 2 的三个数组可以"逐位"重算** ✅ —— 这是更划算的第一步：
`m_tileTotalAngles` / `m_tileStartAngles` / `m_tileDurations`（合计 **12 B/层**）来自
**每层独立的公式**（§4.1 的 Phase 2），不是累加 → 用同样的输入、同样的运算顺序重算，
**逐位相同** ✅。所以这三个数组可以整个不存，改成按层重算 → 2^31−1 层省 **26 GB**，
而且**不需要任何精度让步**。
输入从哪来：`m_tileBPMs`（4 B/层，留着）、`m_tileIsCW`（1 位，留着），以及那个**局部角度** ——
注意它要的是 `angleData[i]` 而不是 `tiles[i].direction`（后者丢了旋转）。
正好 `Tile::angle` 是**死数据**（全仓库无读者，见报告 §7 第 6 条），而且删它也不缩小
`sizeof(Tile)`（24 B）—— 那就把它当作这个局部角度的落脚点，于是这 12 B/层是**净赚**。
（实现前先确认 `LevelData.cpp:1347` 往 `Tile::angle` 里写的到底是什么。）

**(b) 的结论是错的 ✗（2026-10 实测，已回退）**

上面说"Phase 2 的三个数组可以**逐位**重算、零精度让步" —— **不成立** ✗。原因不在公式：
三个量**数学上**确实逐层独立 ✓，但**浮点的位模式不独立** ✗ —— 原式在 `precalcTileRange`
里被 `-O3` 融合成一套指令，原样搬进 `Timeline` 的访问器后编成另一套，于是**不是同一个 float**。
本仓库在"砖块几何"一节早写过这个坑（"同一个算式在不同 part 里可能被编成不同结构"），
这次是它在**时间线**上的同一副面孔。

**怎么发现的（可照着重跑）**：把三个数组换成 `durationAt()/startAngleAt()/totalAngleAt()`
访问器（Phase 2 整块删除）之后：

| 检查 | 结果 |
|---|---|
| `ctest` / 39 fixture 对拍 | 全绿 ✓（**抓不到** ✗）|
| `adocao image` MYC 64×64 md5 | **相同** ✓（太粗，**抓不到** ✗）|
| **像素门槛** | **20 相同 / 31 不一致** ✗✓ |
| 同一二进制两次抓帧 | 逐字节相同 ✓（先排除不确定性 ✓）|
| 同一状态 A/B（改动前 vs 后）| PNG 不同 ✗，而日志 `t=1195.4196s` **一样** ✓ |

最后一行是关键：`t=` 一致说明**前缀和的大势**没变，但帧变了 → 差在**低位**（日志只有 4 位小数）。
**差异的量级我没有测** ✗（别再写成"1 ULP" —— 那是当时的推测，不是测量）。回退后门槛立刻恢复
**51/51 逐字节相同** ✓，A/B 由此闭合（51/51 → 31 异 → 51/51）。

**要重启这一笔，第一步不是写代码，而是先量化** ✗：造一个临时 A/B 工具，把两次构建的
`tileStartTimes` / 三个派生数组**逐层 diff**（最大绝对差 + 出现在第几层），拿到量级之后再决定：
* 容忍 → 12 B/层 = 26 GB ✓，但**必须**重存像素基线，并在 `docs/scale-1e8-to-2e9.md` 的 ⚠️ 口径里
  写明"时间线派生量允许 ≤X"（与 (a) 的决定一起拍板）；
* 不容忍 → 只能存（不许重算）✗，或只给 2^31 档启用重算（分档行为，慎用）。

#### 2026-10 追加：把谜底缩到只剩一个候选 ✗

前一轮只知道"值逐位相同，但门槛 31 个状态变色"。为了把**值**与**代码形状**分开，做了两个对照：

| 对照 | 值（全精度 dump，二进制级）| 像素门槛 |
|---|---|---|
| **只改代码形状**：`PositionSolver` 里"直接读数组" → "调一个原样返回数组值的非内联函数" | **逐字节相同** ✓ | **51/51 不变** ✓ |
| **完整 #1**（数组换访问器 + 删并行阶段）| **逐字节相同** ✓ | **20 相同 / 31 不一致** ✗ |

结论：**"编译器在别处换了写法"这个解释被否掉** ✗ —— 形状变了、像素却没变。于是变色的来源
只可能在 #1 里**既不是值、也不是调用形状**的那部分，候选只剩一个：**被删掉的那个并行阶段
（`std::async` + `precalcTileRange` 的分块）** ✗。

**下次怎么定性（一两轮就够）**：保留并行阶段、其余照改（或反之），只跑名单固定的那几个状态
（`arc90_*` / `icon_*` / `random_overlap_*`）。两条已经钉死的工具别再重复造：
* `tests/tl_dump.cpp`（`adocao_tl_dump`）：把三个数组 + `tileStartTimes` 全精度打出来，
  用于**二进制级**对照 —— 这才是真证据（拿自己写的第二个实现互比会一起错 ✗）；
* `tests/timeline_ab_dump.cpp`：把同一算式在另一个函数里重算再比 —— 只能证明"两条我写的路径
  一致"，**不是**独立证据（末层那个语义错就是两边一起错才漏掉的 ✗）。

**四个探针全部否掉了** ✗（2026-10，每个都带“值逐字节相同”的先验）：

**补两个否证（2026-10 继续）** ✗ —— 现在一共**六个方向全否**：

| # | 假设 | 做法 | 值 | 像素门槛 |
|---|---|---|---|---|
| 1 | 值变了 | 全精度 dump（数组 / Timeline 全量 / hitsound 时间戳 / 行星位置）| — | **逐字节相同** ✓ |
| 2 | 调用形状 | 读数组 → 调原样返回数组值的非内联函数 | 逐字节相同 ✓ | **51/51** ✓ |
| 3 | 串行/并行 | 并行阈值抬到不触发 | 逐字节相同 ✓ | **51/51** ✓ |
| 4 | 类布局 | 头里插个没人用的成员，偏移整体挪动 | 逐字节相同 ✓ | **51/51** ✓ |
| 5 | **FMA 收缩**（Clang 默认 `-ffp-contract=fast`）| 用 `-ffp-contract=off` 整编，另存独立基线后 self-contained 对照 | 逐字节相同 ✓ | **20/31** ✗ |
| 6 | 堆分配模式 | 把被删掉的三块 n×4 分配加回去（内容不用）| 逐字节相同 ✓ | **20/31** ✗ |

**结论（就写到这里，不再往下挖 ✗）**：#1 在**所有可测的量上逐位相同** ✓，却**稳定地**让 51 个像素基线
里的 31 个变色 ✗，原因未明；六个方向都没能解释它。要落地只有一条路：**接受这次变色、重存一次基线**
（项目已有先例：修图标顺序那次也是这么做的）。收益先看清楚：对现有谱仅 ~0.8 GB，只有到 2^31−1 层
才值 26 GB —— 而那边更大的两笔是 `Tile` 51 GB 与事件 34 GB，所以**优先级最低** ✗。

| 探针 | 值 | 像素门槛 |
|---|---|---|
| 只改调用形状（读数组 → 调原样返回数组值的非内联函数）| 逐字节相同 ✓ | **51/51 不变** ✓ |
| 只改串行/并行（并行阈值抬到不触发）| 逐字节相同 ✓ | **51/51 不变** ✓ |
| 只改类布局（头里插一个没人用的成员，偏移整体挪动）| 逐字节相同 ✓ | **51/51 不变** ✓ |
| **完整 #1** | 逐字节相同 ✓ | **20/31 变色** ✗ |

**全量 dump 也逐字节相同** ✓：`tileStartTimes` / `tileBPMs` / `tileIsCW` / `tileDisappearTimes` /
`tileAppearTimes` / `preRoll` / `audioStartOffset` / `totalDuration` / `getHitsoundTimestamps`
（11131 行，两边 md5 都是 `08c19d68…`）—— **渲染器能读到的 Timeline 量一个都没变** ✓。

**所以范围只剩一处** ✗：**渲染器那一侧的 codegen**（它编译时看到的 `Timeline.hpp` 内容变了：
删掉 3 个成员与 3 个访问器）。三个形状探针都**没能复现** ✗ —— 说明触发条件是“头文件内容的具体变化”，
而不是类大小/偏移/并行/调用形状中的任何一项。要定位它，下次得直接做“头文件改法”的对照
（例如用 pimpl 把那三个数组藏起来、让头文件表面不变）。
**在此之前不提交** ✗：数值安全，但会动 51 个基线里的 31 个，而原因未明。

**③ 因此实施顺序改为**：先做 (a) 的精度拍板（`m_tileStartTimes` 与这两个前缀扫描量），
再决定 (b) 走上面哪条；`tileDisappear/AppearTimes`（前缀扫描）情况同 (a）。

### 步骤 ② 路上撞到的一个真问题：导出模式的角度是 0

`m_tileStartAngles` 我原以为是死数据（顺序路径写 `0.0f`），**查完发现不是** ✗：

* `precalcTileRange`（并行路径，n ≥ 256）写的是真值 `outStartAngles[i] = startAngle`；
* `if (m_exportOnly)` 分支（`Timeline.cpp` 的 Phase 2）把 `m_tileStartAngles[i]` 与
  `m_tileTotalAngles[i]` **都写 0**；
* 而 `PositionSolver` 拿它算行星相对枢轴的轨道角：
  `angle = startAngles[i] + totalAngles[i]*progress; mv = pivot + (cos,sin)(angle)*dist`。

实测（一次性探针，angles360，65 组 (tile, progress)）：导出模式与普通模式同一 `(tile, t)` 上的
**最大位置偏差 = 2.000000 砖**；tile 1 的取值 导出 `(0, 0)`、游戏 `(3.159046, -3.124139)`。
也就是说 `adocao image` 的矢量路线（`MapExport.cpp:169/197/199` 用 `positionAtTile`，
Timeline 却是 `exportOnly=true` 建的）画出的是**层心折线整体平移 (1,0)** ——
因为画布按墨迹包围盒自动适配，看起来仍像正常轨道（所以一直没人发现），
但它**丢掉了层内弧线**，且**与游戏不是同一条曲线**。像素门槛只测 `--capture`，盖不到这条路。

**对内存方案的影响**：`m_tileStartAngles` 不能当死数据删 ✗。不过它在两种模式下都能 **O(1) 且逐位**
重算（普通模式 = `(i==0) ? (rotation+180)*pi/180 : fmod(tiles[i-1].direction+180,360)*pi/180`，
导出模式 = 0），所以 4 B/层仍然可以省；但**先要把"导出模式该不该用真实角度"这件事定了**：
若改成用真实角度，`adocao image` 的输出会变（多出层内弧线），这是行为变更，要用户拍板。

### 已修：导出模式的角度不再清零（`Timeline.cpp` Phase 2，2026-10）

`if (m_exportOnly)` 分支原来把 `m_tileStartAngles`/`m_tileTotalAngles` 都写 0，而
`PositionSolver::positionAtTile` 用它们算行星轨道位置（`mv = pivot + (cos,sin)(start+total*progress)*dist`），
`MapExport`（矢量 image 与 --1px 两条路）和 `core/map/LevelMap` 都在调它 —— 所以 `adocao image`
画的不是游戏那条曲线。现在导出模式与渲染路径**共用 `precalcTileRange`**，角度逐位相同。

实测（探针 + 图像对比）：
* angles360 1024²：导出 vs 游戏 墨点 69,551/55,522、位置不同 18,207（32.79%）、最大通道差 218
  → 修后 **55,522/55,522、差 0、最大通道差 0**；
* MYC 16384x6616：修前/修后只有 3,675 个像素不同（0.0034%，最大通道差 37）—— 巨谱上 1 世界单位
  远小于 1 像素，所以**几乎看不出**（敏感用例是小谱/低分辨率）。
* 回归：ctest 5/5、像素门槛 51/51（只动 m_exportOnly，--capture 路径不受影响）。

**遗留**：AGENTS 里那些"墨点 27,331 / AA 版 62,293""stitch 那条路均匀 2 px"的数字，
**是在这个有 bug 的输出上量的** ✗，需要重测重写（`--1px` 的线宽大概会从 2 px 收窄到 ~1 px）。

### 待查异常 ✗：MYC 无头加载的峰值 RSS 对不上

`adocao image --size 64x64`（MYC 611 MB 明文）实测峰值 **4,943 MB**，而 AGENTS 记的是 **1560 MB**。
本轮的改动全是省内存方向（tiles 容量 / setSpeedByFloor / hsChanges / 距离数组 / atStates），
没有一个会变大 —— 所以**原因未明，要单独二分**（`git worktree` 回到旧提交重建对比）。
这条与 2.1e9 的目标直接相关（加载峰值就是"能不能开"），优先级高于继续做检查点化。

### 已定位的异常：MYC 上快路径静默回退到旧路径（2026-10）

AGENTS 里"MYC 快路径 695 ms / 峰值 1560 MB"与实测对不上（`adocao image --size 64x64` = **4.60 s / 4,943 MB**）。
用 operator-new 大块记账按调用点归因（>=64 KB，峰值快照），峰值那一刻是：

| 持有 | 调用点 |
|---|---|
| 2003.6 MB | `std::string::__grow_by_and_replace`（字符串翻倍）|
| 611.2 MB | `loadFromFile`（文件缓冲，正常）|
| 611.2 MB | `loadFromBuffer`（第二份拷贝）|
| 580.9 MB | **`cleanJson`** |
| 500.9 MB | **`parseLegacy` 的 lambda** |
| 154.9 MB | `parseLegacy` |

→ 全部指向**旧路径**（cleanJson + DOM）。用现成钩子确认：`ADOCAO_FAST_REQUIRE=1` 在这张谱上
**直接加载失败** → 快路径确实放弃了。所以这张谱每次无头加载都在跑旧路径：**4.6 s / ~4.4 GB
而不是 ~0.7 s / ~1.6 GB**（结果仍然正确，旧路径本来就是逐位对拍的基准，但代价是 3 倍内存 + 6 倍时间）。

**不是本轮改动造成的**：把 `parseNumber` 的"整段消费"检查临时改回旧行为后重建，快路径**依旧放弃**
（退出码 1、4.61 s）→ 这个回退早于本次会话。而现有的测试**抓不到**它：`ctest` 的 fixture 太小，
真实谱的快路径成功与否没有任何断言 —— 与当年"窗口路径静默回退让测试全绿"是同一类坑。

**下一步（建议）**：先让回退**说话**——在 `tryFastParse` 的每个阶段前置一个 thread-local 阶段名，
回退时用 `LOG_W` 打出"在哪个阶段放弃"，并让 `ADOCAO_FAST_REQUIRE=1` 把它带出来（把静默回退
变成可诊断的失败，正如 `ADOCAO_WINDOW_REQUIRE` 当年做的那样）；然后按原因修，并把这张谱
（或一个等价的中等谱）加进回归。

### 更正：上面那条"不是本轮改动造成的"结论是错的 ✗（2026-10）

真凶就是本轮的 `parseNumber` hardening（`cfa7c3c`）：它把 `q >= e` 当成"token 被截断"而返回 false，
但**两个调用方传进来的 `e` 都是"这个值/这个区间的真实末尾"**（`angleData` 是区域括号、action 字段来自
`skipValue`），所以一个"正好填满跨度"的完整 token 被误判。整数走整数快路径、不经过这段代码，
于是**只有非整数的 action 字段**（`bpmMultiplier: 0.5` 这类）中招 → 整份文件退回旧路径。

我上一轮的 A/B **只回退了 `endp != dst + n` 那一行**，没回退这个 `q >= e` 判断，所以得出了
"不是本轮改动"的**错误结论**。教训：A/B 要把**整个改动**一起回退（或逐项各回退一次）。

修复：`if (q <= start) { p = start; return false; }`（去掉 `q >= e` 的误判）。

实测（修复后）：
* 最小复现 `{"bpmMultiplier": 0.5}`、`level.adofai`（300.9 MB）、`level_no three planets.adofai`（282.2 MB）、
  `…_MYC.adofai`（611.2 MB）、`tests/charts/angles360.adofai` —— **全部走快路径** ✓
  （修复前五张全放弃）；
* MYC 无头 `--size 64x64`：**4,943 MB / 4.60 s → 1,473 MB / 1.39 s**（含写 PNG）；
* ctest 5/5（三路逐位对拍这次真的在比快路径）、The Moon 60 万层对拍 1 通过 0 不一致、
  严格几何 2/2、三护栏、像素门槛 51/51 逐字节相同。

**待办（下一步）**：给这条加**永久**回归 —— 在 `tests/level_parse_test.cpp` 里加一个自检，
用 `setenv("ADOCAO_FAST_REQUIRE","1")` + 一段含非整数 action 字段的内存 JSON，
断言加载成功（即快路径必须吃下它）。否则这条静默回退还会回来。

### `.adocao` 二进制容器（P1 进行中，2026-10）

已落地：格式常量 + 列编码器（Dict/Const/DeltaVarint/BitPack/Raw + 编码自检 + 坏数据干净失败，
19 项自测含 5 个负向对照）+ **写入方** `adocao pack` + 规范文档 `docs/adocao-format.md`。

实测：
* **MYC 611.18 MB → 19.58 MB = 3.033 B/层（31×，0 换页）**；
  angleData Dict 0.7501 B/值（52 项/6bit）、floor DeltaVarint 1.0 B、type Dict 0.25 B、
  strId **Const 0 B**、flag BitPack 0.125 B、val1 Dict 0.75 B、val2 Dict 0.375 B。
* **primer 7.38 GB → 324.36 MB（对明文 22.8×），但对 xz -9e 的 357 MB 只有 1.10×**
  → **触发停手线（<1.5×），不再为音频谱加专用机制**。原因是 `actions.val1`（每层音量）
  6841 万个值几乎互不相同 → Raw 4 B/事件 = 272 MB = 全文件 84%；其余六列合计只有 68 MB。
* 顺带纠正一个早先的错误认知：primer 的 `angleData` 不是"开头有一长串 0"，而是**整列常量**
  （所以我那两次"中段采样"必然量出去重 1）。

下一步（P1-⑥⑦⑧）：读取方（`sniffLevelArchive` 新增 Adocao + `loadFromBuffer` 分派 + 逐位无损）、
`adocao_roundtrip`（13 节 hash + 可复现性）、零回归（ctest / 像素门槛 / 三护栏）。
可选：`DeltaF32` 列编码（对 float 做**数值**差分）榨音频谱 —— 先实测熵再决定。

### `.adocao` P1-⑥ 完成：读取方 + 端到端逐字节证明（2026-10）

core 侧只加了三样：`LevelArchiveKind::Adocao`、`kAdocaoMagic`（magic 唯一来源）、
`ArchiveBackend::decodeAdocao` 钩子；`loadFromBuffer` 按 magic 分派后**与两条 JSON 路径共用
同一个 `finishLoad()`**（这是逐位一致的结构性保证）。archive 侧 `AdocaoReader::unpackLevel`
校验 magic/version/fileSize/headerCrc（覆盖头+整张段表）/逐段 crc32c，并解析字符串池 →
settings 定长记录 → angleData 列 → actions 六列 → pathData；未知段 id 跳过（前向兼容）。

端到端验收（最硬的那种）：
* MYC（6,770,913 层）用 `.adocao` 与用明文 JSON 各出一张 64×64 PNG → **md5 逐字节相同**
  （`8fdbed25f383aaabea32d0677f89eae1`）；
* ctest 7/7、三护栏、`ADOCAO_TILE_EXACT=1` 几何 2/2、像素门槛 **51/51 逐字节相同**。

还剩 P1-⑦：把"pack → 走完整 loadFromBuffer → 13 节 hash 逐位相同 + 同一输入两次 pack 字节相同"
做成常驻用例（复用 `tests/level_parse_test.cpp` 现成的 13 节比对与 archiveRoundTrip 框架）。

### `.adocao` P1-⑦ 完成 → **P1 全部落地**（2026-10）

`tests/level_parse_test.cpp::archiveRoundTrip` 已接入 `.adocao`：每个 fixture（39 个 + 真实谱）
都会被 pack 成二进制 → 走**完整** `loadFromBuffer`（含 magic 分派与 `finishLoad`）→ 与明文加载
**13 节逐位比对**；并断言**可复现性**（同一输入两次 pack 逐字节相同）与两个负向对照
（截断 / 改坏 settings 载荷 → 段 crc32c 必须抓住）。负向对照自证：crc 失效则 `loadBuffer().ok`
为真、用例立刻报错。

最终成绩单（本机实测）：
| 谱 | 明文 / 原容器 | `.adocao` | 倍数 |
|---|---|---|---|
| MYC（677 万层 / 618 万事件）| 611.18 MB | **19.58 MB** | **31×** |
| angles360（5564 层）| — | 0.02 MB | — |
| primer（6841 万层）| 7.38 GB（xz 357 MB）| 324.36 MB | 对明文 22.8× / 对 xz 1.10×（**停手线，不再加专用机制**）|

回归：ctest 7/7、core purity、44 开关帮助、7 对 shader、严格几何 2/2、像素门槛 51/51 逐字节相同。

剩余（都不是 P1 必需项）：
* P3 零拷贝（段布局与内存布局一致时 mmap 直读）；
* P4 段级压缩（复用 xz/zstd backend）+ `preserved` 段（供 ADOCAO-E 无损回写）；
* `DeltaF32` 列编码（对 float 做**数值**差分）—— 只为榨音频谱，先实测熵再决定；
* `writerCommit` / `inputHash` 的构建期注入（字段已在头里占位）；
* `WindowSource` 对 `.adocao` 的"逐段交付"（目前整份读进内存；文件只有几十~几百 MB，不急）。

### Twirl 能压多少（实测，2026-10）—— 两个杠杆

用户问"Twirl 有能压的地方吗"，答案是**有，而且不小**：

1. **载荷列被所有事件共享**（设计缺口）：MYC 的 actions **99.6% 是 Twirl**（6,155,053 条），
   而 Twirl 的 val1/val2/strId 非零比例全是 **0%** —— 但它照样在 val1(6bit)/val2(3bit) 的
   下标流里占位。Twirl 现在占 13.94 MB（floor 5.87 + type 1.47 + val1 4.40 + val2 2.20）。
   **改成"按类型稀疏的载荷列"可省 6.60 MB / 19.58 MB = 33.7%**，且事件顺序与语义逐位不变
   （读取方按"类型→用哪些列"的固定表跳过不用的类型，值取默认 0）。
2. **段级压缩（P4）更大**：.adocao 再 xz -9e → MYC 19.58 MB 变 ~11 KB、The Moon 1.25 MB 变 <10 KB
   （这两张极度规则，**不可外推**）；angles360（1167 个角度）几乎压不动；
   **primer 324.36 MB → 195.43 MB**（对 xz-JSON 的 357 MB = **1.83×**）。
   → **修正上一轮的停手结论**：primer 的"只有 1.10×"只在"不压缩"时成立，P4 应当提上来做。
3. 音频谱还有第三个杠杆：`DeltaF32`（对 float 做**数值**差分）。依据是"位模式 Raw + 仅 xz 就能
   1.66×"，说明 float 里有真实结构被位模式表示丢掉了；**先实测数值差分后的熵再决定**。

顺带：Singularity / Tempest 在我试的两个路径下不存在（`~/Documents/Charts/Singularity/level.adofai`
等），所以上面的"一般谱面"结论目前只有 The Moon / angles360 两个样本，**样本偏少**。

### RLE 列编码（实测结论，2026-10）

四张赫兹谱实拍图（用户提供：锯齿 / 45° 直线 / 整圈 / L 形）引出的是**游程**结构。实测：
* `angleData`：游程均值 **1.0**（MYC，锯齿型）→ **RLE 反而更差**（5.08 MB → 11.58 MB）；
* `type` 117 / `flag` 11179 / `val1` 118 / `val2` 62444 → **RLE 极佳**
  （MYC 的这四列 9.29 MB → **~160 KB**，其中 val2 是 14,000×）；
* `floor` 是差分列 → 要用"**对差分流再取游程**"（6.18 MB → 十几字节）。

→ 加 `Rle` 列编码 + `DeltaRleVarint`；选中的判据必须是**实测字节数最小**
（不能按列名硬编码：直线的赫兹谱会在 angleData 上选中 RLE，锯齿型则不会）。

## ③ 切片 4：`tiles` 按块物化 provider（代码已落，默认关，差两关）

值级已逐位验收（`ctest floor_window_cache`：angles360 5,565 / diag_glide 1,025 / The Moon 600,539 /
**MYC 6,770,913 层（1654 块）全部 0 不一致**，含"改角度 → 必须变 / 复原 → 必须回来"的反面对照），
`LevelData::tilePositionAt/tileDirectionAt` 已全部走访问器。但**两关没过，所以默认关**
（`ADOCAO_FLOOR_WINDOW=1` 打开做 A/B）：

1. **像素门槛变**（51 个状态 50 个不一致）。差异图显示：**内圈整条弧消失 + 外圈 1~2 px 镶边** →
   问题在**网格装配/可见性**那一层，不在数值上（数值逐位相同）。方向：A/B `TileMesh` 的
   `m_posX/m_posY/m_tileShape` 与可见实例列表（`ADOCAO_CULL=scan` 对比），看是 chunk 基点、
   形状分组还是 cull 阈值不同。
2. **footprint 反涨**（MYC 493.6 MB → **1.1 GB**，而 live 528 → **150.5 MB**，−174 MB）——
   差值 ≈ 950 MB 是"已释放但仍占"的页 = **单块缓存抖动**：物化块反复分配/释放 130 KB
   （窗口 98 KB + 角度 32 KB + 事件）。方向：单块缓存 → **±1 预取窗**，并让 `TileMesh::build`
   与 `CullIndex::build` **顺序物化**（它们本来就是顺序的）。

**2026-10-15 进展（三个真 bug 已修，门槛 1/51 → 21/51）**：
1. `TileShape::keyForTile()` 直接读 `lv.tiles[i].direction` —— 丢数组后是**空数组越界**，
   崩溃地址 `0x4` 正好是 `direction` 在 `Tile` 里的偏移（改成走 `tileDirectionAt()`）；
2. `LevelData::tileCount()` 原来就是 `tiles.size()` —— 丢完数组变 0，而 mesh/capture/jump 都拿它当
   砖数（改成窗口化时由 provider 记着的权威值）；
3. `GameWindow::releaseMeshTemporaries()` 会清 `tileBPMs` —— provider 合成 SetSpeed 要用它
   （现象是"构建期正常、播放期崩"；窗口化时不清）。
修完后：网格数据**逐位相同**（`ADOCAO_MESH_DUMP=1` 转储 `tileShape/posX/posY/iconBits/chunkX/Y`
的 FNV，on/off 全等），`angles360` 整帧 **PNG 逐字节相同**；footprint 也回到 **404.6 MB**
（off 493.6 / live 268.2 vs 324.2）—— churn 由"重算输出复用缓冲"解决
（`computeFloorWindow` 不再 `out = FloorWindow{}`，容量保留；cache 持有 scratch）。

**仅剩 1 处**：MYC t=30s z25 差 **21 个像素**，落在**蓝行星拖尾开窗起点的 2 个采样**上
（`ADOCAO_TRAIL_DUMP=1`：红点哈希 on/off 相同；蓝点 `[0]/[1]` 不同、`[2]` 起完全一致；
off 那两个采样值相等=被钳住，on 是正常插值，差值正好是**一块砖的长度** ⇒ 窗口起点
`findTileIndex(startTime)` 的 tileIdx 差 1）。下一步：在 `sampleTrailRange` 起点处把
`findTileIndex` 的窗口实现与密集二分逐值对拍（含"起点恰好等于某砖 startTime"、以及
`startTime` 落在**相等时间串**里的情形）。

**2026-10-15 收尾：provider 已默认打开**（`ADOCAO_FLOOR_WINDOW=0` 可关，A/B 用）。
最后那处差异的根因不是 provider，而是**先在 bug**：`GameWindow::releaseMeshTemporaries()` 在**同步**
构建路径（`GameWindow.cpp:228`）也会跑，而无条件 `std::vector<double>().swap(m_level->angleData)`；
`Timeline::totalAngleAt()`/`angleDirBefore()` **每一帧**要读 `angleData` 算行星/拖尾的转动 ⇒ 空数组下
`rawAngleData` 全变 180°，行星沿**错误的弧**转（像素门槛只差 21 个像素，所以一直没被发现）。
修法：不再释放 `angleData`（+8 B/层 = MYC 54 MB，换来正确转动；容器路径将来会把它也窗口化）。
**基线已重存**（有意行为修正；重存前的差异统计：非行星/拖尾状态 **0 像素**，
`icon_ssup_under_trail` 1.26%、`icon_ssup_under_planet` 0.65% —— 全部落在行星/拖尾）。
修完后 provider 关/开两种模式**都 51/51 逐字节**。

常驻（MYC 原版 6,770,912 层，`--no-hitsound`）：③ 开工前 **675.5 MB / 528 MB live** →
切片 3a 后 **493.6 / 324.2** → 切片 4（默认开）**430.2 / 276.3**。

**2026-10-15 再进一步：`angleData` 也换掉了**（③ 切片 4 的另一半之一）。运行期读者一律走
`LevelData::angleAt()`，加载收尾把 `angleData`（8 B/层）编成**位精确紧凑存储**
（`core/level/AngleStore.hpp`：取值字典 + 位打包下标）：
* 实测真实谱面角度取值集极小 —— MYC **52** 个取值 → 6 bit/层 = **0.75 B/层（51.7 → 4.8 MB）**、
  The Moon 49 个取值（4.6 → 0.4 MB）、angles360 1167 个取值（3.05 B/层）；
* **护栏**：紧凑存储不比原数组小就不换（实测 `diag_glide` 1024 个取值 → 10 bit/层 + 字典 = 9.25 B/层，
  比 8 B/层还大 → 跳过、保留原数组）；
* 逐位验收 `ctest angle_store`：释放前后 `angleAt` / `Timeline::totalAngleAt` /
  `TileShape::keyForTile` **全层 0 不一致** + 反面对照（`setAngleForTest`；这次它抓到了我
  `putBits` **只置位不清位**的 bug —— 生产路径在全零缓冲上写所以没事，`setForTest` 会留残留位）。
* 注意这是**明文路径的过渡手段**：位打包仍是 O(n)（2³¹ 层 = 1.6 GB），最终要交给容器路径的
  blocked 角度列（按块取、与 checkpoint 同粒度）。

常驻（MYC 原版，`--no-hitsound`）：675.5/528（③ 开工前）→ 493.6/324.2（3a）→
430.2/276.3（provider）→ **404.0/206.3**（+ angle store）。

**2026-10-15 又一小块**：TrackVis 的逐砖可见位从 **1 B/砖** 改成 **1 bit/砖**
（`render/BitSpan.hpp` + `TileMesh::visSetRange`；MYC 6.8 → 0.85 MB）。
纯逻辑抽成不给存储的小工具，`ctest bit_span` 拿**朴素逐位实现当参照**对拍 37,812 组区间
（含跨字节/单点/整字节/边界/`a>b`/全区间不越界）。footprint 404.0 → **388.2 MB**、live 206.3 → **199.8 MB**。

**2026-10-15 地基已铺好**：`TileMesh::deriveRange(level, i0, i1, out)` —— 把"某层区间的每砖派生数据"
（相对块原点位置 / 形状号 / 图标位）抽成纯函数，**`build()` 现在也调用它**填那三支数组
（`m_shapeIndex` 用 64 位打包键：sa/ea 各 20 位 + mid 1 位，避免再引结构体哈希）。
于是"按块派生"与"今天的整段数组"的一致性**由构造保证**；像素门槛 51/51 逐字节正好证明派生口径没变
（纯重构的验收就是这条）。渲染侧真正省内存的那步还没做：

**2026-10-16 做成了**（同一思路，先二分定位）：整帧与基线 0 像素、门槛 51/51、ctest 24/24，
常驻 footprint **388.2 → 311.3 MB**、live **199.8 → 114.0 MB**。根因是**图标位**：
`tileHasTwirl/tileHasSetSpeed` 被 `releaseMeshTemporaries()` 在建网格后清掉，而窗口派生发生在
**剔除时**（更晚）→ icon 全 0（实测块 0 有 2148 处不一致、出图差 858 个边缘散点）。修法 = 窗口化时
保留这两张位图；另加"并行段主线程先 pin 窗口"（多段/超槽位退串行）。详见 `AGENTS.md` 与
`docs/rendering.md`。

**第一次尝试（2026-10-15）的记录（已不适用，留作教训）**：出图差 858 像素、scan 路非确定性崩溃。
已确认/排除的事实，下次直接从这些线索继续，别重走：

* **派生数据本身证明是逐位相同的**：`ADOCAO_MESH_DUMP=1` 打整段派生的 FNV，与"数组时代"记录的
  `tileShape=621cbe9f38f18544 posX=09b5fef3a8e9b736 posY=9d696ca1e1e161a9 iconBits=81ab7c351b347636`
  **四个全等**（MYC 677 万层）—— 所以差异不在 `deriveRange`。
* 但整帧差 **858 个像素**，全是**砖边缘的散点**（不是缺砖）⇒ 可见集在视锥边界上差一层。
* **已排除**：① 并行剔除碰共享窗口缓存的竞争（强制串行后 858 像素照旧）；② 剔除索引的取位置来源
  （改成与判定同一批 float 舍入值后照旧）。
* 另一条独立 bug：`ADOCAO_CULL=scan` 那条**多段**路会**非确定性地段错误**（lldb 下又干净退出
  ⇒ 内存不安全/竞争）。怀疑与"一个 range 跨多个 4096 窗口时，后续段在 worker 里取窗口"有关。

**下次的正确做法（二分 + 工具）**：
1. **切成两笔**：第一笔保留四支数组**同时**建块窗口，但让 `cullRange` **改从窗口读**（纯读路径 A/B）——
   若这一笔就复现 858 像素，问题在窗口取值/分段；若不复现，问题在我删掉的数组周边代码。
   第二笔再真正丢掉数组。
2. 多段路径要么**预先把每个 4096 段的窗口都 pin 好**再并行（worker 只读），要么该情形直接串行。
3. 用 **ASan** 跑 scan 路抓那个段错误的根因（`-fsanitize=address`）。
4. 注意 `cullRange` 的 SIMD 4 宽循环 + 尾巴两段结构**不能变**（顺序/舍入都靠它）。

**目标收益**：13 B/层（MYC 84 MB）→ footprint 388 → **~305 MB**。
两条硬约束：① **cull 内循环形状不能变**（今天直接下标取数组、约 0.47 ns/砖 → 改成"块窗口 + 每块派生
一次"要保持同样的内循环；且要小心"跨块边界"的取向）；② **形状分组的插入序 = 绘制序**
（`unordered_map` 的桶增长序列不能变，派生必须按层号顺序做）。
另外容器路径应把 `tiles`/`actions`/`angleData` 都换成 blocked 列 + 预取（`angleData` 的位打包仍是 O(n)：
2³¹ 层 = 1.6 GB ✗）。
难点是 **cull 内循环**现在直接下标取数组（~0.47 ns/砖），改成"块窗口 + 每块派生一次"要保持同样的
内循环形状，且**形状分组的插入序必须是绘制序**（`unordered_map` 的桶增长序列不能变 → 派生要按
层号顺序做）。另外容器路径应把 `tiles`/`actions`/`angleData` 都换成 blocked 列 + 预取。
