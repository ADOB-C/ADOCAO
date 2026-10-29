# 渲染（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

## 砖色（ColorTrack / trackStyle，2026-10-17）

颜色分两层（与 Re_ADOJAS `src/lib/Player/TileColorManager.ts` 逐条对齐）：

1. **基色**：`trackColorType` 从 `trackColor` / `secondaryTrackColor` 算出一支本体色
   （Single / Stripes / Glow / Blink / Switch / Rainbow / Volume）。
2. **样式**：`trackStyle` 决定 fill / stroke 怎么从本体色派生
   （Standard 0.7×｜Neon 黑本体+本体描边｜NeonLight 半亮本体｜Basic 黑描边｜
   Gems 同 Standard｜Minimal 无描边）。`addBlack(0.7)` 是**逐通道 ×0.7**，不是向黑混合 70%。

实现：`core/level/TileColor.{hpp,cpp}`；settings 字段与 `ColorTrack` 富事件在 `LevelData`
（`colorEvents`，线性查找的 `colorEventAt` / `tileColorsAt` 只给测试与一次性查询用）。
`isTimeDrivenColorType()`（Glow/Blink/Rainbow/Volume）是"要不要逐帧"的判据。

**逐帧/静态的划分**（别每帧全谱重算；Re_ADOJAS 同款）：

| 东西 | 频率 |
|---|---|
| 每段 fill/stroke（非动画类型） | 加载时一次 |
| Glow/Blink/Rainbow/Volume、TweenColor | 每帧，仅**可见**砖 |
| ColorTrack 生效 / style 变更 | 事件时一次 |
| 几何/mask | 按 `(pred,dir,is999,style)` 缓存 |

**渲染接线（2026-10-17 已落地）**：

* 实例 VBO 由 5 float 扩到 **11 float/实例**（offset xyz + shape + iconBits + fill rgb + stroke rgb，44 B）。
  示例缓冲本来就只装**可见列表**（每帧几百砖），所以逐砖色不占全谱内存。
* `TileMesh::draw(..., InstanceColorFn, void*)`：色回调只对可见砖调用；`LevelScene` 传
  `LevelData::tileColorsAt(tile, timeInLevel)`。
* shader：`uUseInstColor=1` → `mix(aStrokeColor, aFillColor, layer)`；`=0` 回退 uniform
  （高亮砖、以及显式 `--fill/--stroke` 的"整条轨道一套颜色"）。
* **默认可视化 = 谱面颜色**（settings + ColorTrack），`--fill/--stroke` 仍可整体覆盖（CLI 显式给才生效）。
  像素门槛 54 个状态因此按新行为**重存**（原基线的描边是启动器默认 0.5×，现在是 chart 的 0.7×）。

**动态 A/B 跑参考实现（2026-10-18 起）**：`scripts/ref-ab.sh <chart> --time 44 [--no-camera]`
把 `~/Repos/Re_ADOJAS` 的 vite dev server 起起来，用 playwright 驱动它**真的播放到同一时刻**抓帧，
再用 `./build/adocao --capture-time 44 --capture-camera` 抓同一帧，最后并排拼成
`build/ref-ab/<name>_ab.png`（上=参考、下=我们）。用途：像素门槛只证明"和上一版一致"，
它证明"**和权威实现一致**"——两边都"有"的怪相说明那是谱面/样式本身的几何，不是我们的 bug。

三条踩过的坑：① DSH 自带的 node 带 hardened runtime，rolldown 的原生绑定会因 Team ID 不同被
dyld 拒绝（报 "Cannot find native binding"）→ 用 `/opt/homebrew/bin/node`（`REF_NODE` 可覆盖）；
② 参考仓库要在**自己目录**写 vite 缓存（`node_modules/.vite-temp`），沙箱里要先放开写权限；
③ 大谱（2025，3.4 MB / 1 万砖）解析约 25 s，之后才是"按空格开始播放"的起点，等帧时间要按这个基准算。

**RecolorTrack（2026-10-17 已落地）**：新版 ADOFAI 的换色事件（`RecolorTrack`，与旧的
`ColorTrack` 并存）。它**不是**"从 floor 起持续"的段，而是"**触发时改一段**"：

* 字段：`startTile`/`endTile`（`[offset, relativeTo]`，relativeTo = ThisTile/Start/End；
  也接受绝对数字/字符串）、`gapLength`、`duration`（**拍**）、`ease`、`angleOffset`；
* 触发时间 = `startTimeAt(floor) + angleOffset/180 × 一拍`（Re_ADOJAS `calcRecolorTriggerTime`）；
* 生效范围 = `[min(start,end) .. max(start,end)]`，步长 `gapLength+1`（隔层），**触发后覆盖**
  范围内的砖（包括玩家**前方**的砖 —— 这就是"波次变色"的来源）；
* 与 `ColorTrack` 的优先级：先算 ColorTrack/settings 的基础色，再叠加**触发时间 ≤ t 的最后一条**
  命中事件；同刻按文件顺序；
* 渐入：`duration > 0` 且 Single/Stripes 时，从"触发前该层显示色"（上一个命中事件的目标 /
  基础色）插值 `duration × 该事件层的一拍` 秒，Kill(complete) 语义同 TweenColor；
* 索引：每 **4096 层**一块，块内是该块可能命中的事件下标（按 trigger 排序）；查询
  O(块内事件数)。条目上限 **4M**（超了 `LOG_W` 截断）；`buildRecolorIndex()` 在 `LevelLoader`
  里、丢起始时间数组之前调用。

**逐实例颜色/变换的开销（2026-10-17 实测，Apple M5 / 1280×720 / uncapped / 8 s / p50）**：

| 谱面 | 可见砖 | `work` | `frame` | `anim` | 占 `work` |
|---|---|---|---|---|---|
| The Moon | 329 | 0.458 ms | **0.654 ms** | 0.079 ms | 17% |
| Hello (BPM) 2025 | 706 | 0.807 ms | **1.055 ms** | 0.265 ms | 33% |
| Won't You（MYC） | 126 | 0.405 ms | **0.633 ms** | 0.018 ms | 4% |

* 逐砖 ≈ **200 ns**（颜色 ~180 ns + 变换 ~5 ns）；60 fps 预算里占 **0.5–1.6%**。
* **26.2 万可见**（LOD 上限）外推 **~50 ms** —— 只在"整张一帧"的极端视距出现（那档本来就在 85.7 ms 剔除上限附近）。
* 没有 ColorTrack/RecolorTrack/MoveTrack 的谱 `setAnimNeeded(false)` → **0**；暂停（时间不变）→ **0**。
* 实例 11→19 float（44→76 B）：26.2 万上限 11.5→19.9 MB（+8.4 MB）；上传字节 +55%，p50 3–6 µs。
* 若要压"整张一帧"：下一步是**按段批量**算颜色（列表升序，每段一次；pulse/Volume 才逐砖），
  预期 ~5 ns/砖、26.2 万约 1–2 ms。profiler 段：`anim` / `animColor` / `animXform`。

**ScaleRadius（2026-10-17 已落地）**：轨道半径缩放（黑洞/吸入类特效）。

* 字段：`floor`、`scale`（%，缺省 100）。**继承**：从事件层起一直生效到下一个 ScaleRadius。
* 算法（对齐 Re_ADOJAS `buildTileRadiusScales` + `applyRadiusScaleOffsets`）：
  `r = scale/100`；从第 1 层起，若 `r ≠ 1`，沿**入口方向**（指向上一砖，`direction[i-1] + 180°`）
  累加 `(1-r)` 个单位：`ox += cos(θ)(1-r)`、`oy += sin(θ)(1-r)`，然后
  `tiles[i].position += (ox, oy)`。所以 `r>1` 把砖往**前进方向**推（拉长/绕圈），`r<1` 往回收。
* 位置是**烘进 `tiles[i].position`** 的（和 PositionTrack 同一个 pass 之后），渲染/剔除/行星/相机
  全都自动吃到；砖的**尺寸不变**（与原版一致）。
* **副作用**：偏移是"按入口方向的前缀和"，checkpoint 里没有它 → 有 ScaleRadius 的谱面
  **禁用起始时间窗口化**（保留密集数组，`LevelLoader` 里会打日志）。2³¹ 目标下这是待解项
  （要么把前缀和放进 checkpoint，要么按窗口从最近事件回放）。
* 验证：2025 有 335 条事件，t≈13.3–16.3 s 的"吸入"变形已出现；`.adocao` 动作列往返一致；
  ctest 31/31；像素门槛按新行为重存。

**MoveTrack（2026-10-17 已落地）**：轨道动画（位置/旋转/缩放/不透明度），对齐 Re_ADOJAS
`buildTileMoveTrack` 的"每砖属性关键帧"：

* 字段：`startTile`/`endTile`/`gapLength`（与 RecolorTrack 同一套范围解析）、
  `positionOffset`（**相对基础位置**，分量可为 null）、`rotationOffset`（**绝对角度**，覆盖砖自身
  旋转；我们的形状已经把方向烘进几何，所以实例旋转就是 rotationOffset 本身）、
  `scale`（数字或 [x,y]，%/100）、`opacity`（%/100）、`duration`（**拍**）、`ease`、`angleOffset`；
* 每条事件对"它写了"的属性做一次补间：`from = 同属性上一次的目标值`（Kill(complete) 语义）、
  `to = 绝对值`，时长 = `duration × 该事件层一拍 / max(0.1, pitch/100)`；末值保持；
* 索引：与 RecolorTrack 一样按 4096 层分块、块内按 trigger 排序；查询 O(块内事件数)。
  `movePad()` = max(|positionOffset| + (maxScale−1)×0.75)，`ensureDrawList` 的视锥外扩加上它，
  保证被移走/放大的砖不会漏剔除；
* 渲染：实例从 11 → **17 float**（+ `rot, scaleX, scaleY, alpha, moveX, moveY`）。顶点 shader 先
  **绕砖心**缩放/旋转（Re_ADOJAS 的几何以砖心为原点：`CaculatePoints` 的 `len` 是半长、
  点跨 ±(len+outline)；我们是"砖起点 + pivot"，pivot = 0.5×(cos dir, sin dir)），
  再 `+ aMove + aInstOffset`；`vOpacity *= alpha`。纯位移（rot=0, scale=1）走单独分支，
  逐位不变。
  identity（rot=0/scale=1/alpha=1/move=0）逐位不变（`geom_probe` 197 万坐标 0 失败，走 GEOM_PROBE
  分支时不做变换）。
* **`RepeatEvents` 展开（2026-10-17 已落地）**：`eventTag`（空格分隔的多个 tag）与同 floor 的
  `RepeatEvents.tag` 匹配 → 按 Re_ADOJAS 规则展开：Beat 模式（`repeatType != Floor`）保持 epFloor、
  角度偏移 `+= interval*rep*180`；Floor 模式 `executeOnCurrentFloor` 把 epFloor 移到目标层，
  否则用几何拍差 `(entryBeat(target)-entryBeat(base))*180`；范围/触发/pitch 都按 **epFloor** 算。
  **同 floor 的事件必须按文件顺序**（`order`）：2025 的 floor 2309 有 `scale/opacity 0` 与
  `opacity 250` 两条，顺序反了砖就消失——这就是“MoveTrack 抄不对”的真因。
* 未做：行星仍沿原路径（不跟随 MoveTrack 位移，与原版一致）。

**Flash（2026-10-17 已落地）**：全屏闪屏，按 `plane` 分两层：

* **Background**：在砖之前画（砖会盖住它，视觉上"只染背景"）；
* **Foreground**：在所有内容之后画（屏幕空间最上层）；
* 事件字段：`duration`（**拍**，`buildFlashIndex` 里换算成秒 = `duration × 该层一拍 / pitch`；
  2025 floor 995 的 `dur 32` 在 bpm 12150 下 ≈ 0.16 s，官方实测“纯黑只有几帧”）、`plane`、
  `startColor`/`endColor`、`startOpacity`/`endOpacity`
  （0..100 → 0..1）、`ease`、`angleOffset`、`flashStyle`（Flash/Kill/StayBlack）；兼容旧格式
  `color`/`colorTo`/`opacity`/`flashStyle`。
* 语义（对齐 Re_ADOJAS `FlashEffect`）：触发时把该 plane 设成 start 再按 ease 补到 end；
  **duration<=0 或补完后停在 end 值**（`endOpacity>0` 就一直保持），直到下一次 Flash/Kill；
  `Kill` 立即清空，`StayBlack` 立即全黑 100%；每个 plane 只有一条活动过渡（后触发的覆盖前一条）。
* 触发时间 = `startTimeAt(floor) + angleOffset/180 × 一拍`，在 `LevelLoader` 里
  `buildFlashIndex()` 预计算并排序；每帧 `flashStateAt(plane, t)` 二分取最后一条 ≤t 的事件。
* 全屏是一个 `gl_VertexID` 三角形（无 VBO）+ `flash.vert/flash.frag`；混合
  `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`，`glDepthMask(false)` 不写深度。
* 未做：`legacyFlash`（v13 及更早谱面 FG/BG 共用一块材质）的兼容分支；`Reverse` 旧样式。
* **Alpha（2026-10-17 已落地）**：逐砖 alpha = **颜色 hex 的 AA/255**（如 `ffffffb3` → 0.70）**×**
  MoveTrack 的 `opacity`；写进实例 `aXform.w`，`vOpacity = uOpacity * aXform.w`；砖块 pass 打开
  `SRC_ALPHA` 混合（alpha=1 时逐字节不变）。alpha<0.005 的砖在 FS 里 discard。Flash 的 alpha 是
  独立的叠层，不经过这里。

**style 几何（2026-10-17 已落地）**：`buildShape()` 收 `styleWidth/styleLength`（Basic/Neon/NeonLight
半宽 +0.025、半长 +0.02；Minimal 半长 −0.03），形状键 `GeoKey`/`packShapeKey` 带上 style（3 bit），
所以同一角度在不同 style 下是不同形状；`deriveRange` 的查表也带 style。The Moon 的 1296 条 Neon /
64 条 Minimal 事件因此真的变了形状（门槛 3 个状态重存）。

**Volume（2026-10-17 已落地）**：`AudioEngine` 在 `loadMusic` 后用**另一个单声道解码器**扫一遍，
存 **50 Hz 的 RMS 包络**（`amplitudeAt(seconds)`；10 分钟 ≈ 120 KB）。`GameWindow::render` 把
`amplitudeAt(audioPos)` 传进 `LevelScene::render`，`tileColorsAt(floor, t, amp)` 用它做 Volume 插值。
离线包络 → **抓帧确定**（不依赖当时是否在放音），与 Re_ADOJAS 的 WebAudio RMS（2048 样本窗口）同义。

**TweenColor（2026-10-17）**：`tileColorsAt(floor, t, amp, timeline)` 在事件 `duration > 0` 且
`trackColorType ∈ {Single, Stripes}` 时插值（Re_ADOJAS `tickColorFade`）：

* `duration` 的单位是**拍** → 秒 = `duration × timeline.durationAt(e->floor)`（该层一拍）。
* 起点 = `timeline.startTimeAt(e->floor)`（玩家"跨过"事件层的那一刻）。
* `t < start` → 上一段目标色；`t ≥ start+dur` → 目标色；中间按 `easeApply(e->ease, p)` 插值。
* 打断语义 = `Kill(complete:true)`：新事件的 "from" 是**上一段的目标色**（不是当时的混合值）。
* `justThisTile` 只影响本层，它的 "from" 是"若没有这条事件时该层生效的持久段"。
* alpha 不插值（Re_ADOJAS 一直用新事件的 alpha）。

**还没做**：
* Neon/NeonLight/Basic/Minimal 的 **2×128 渐变条**（需要给每个顶点补一个"距外缘归一化"的 v，
  在 `TileShape::expand` 侧算；现在是硬边 ring，颜色对、过渡不对）。
* ~~Gems 的宝石网格~~ —— 2026-10-17 已落地：新增砖 part `P_GEM`（槽 0=圆心、1..8=八边形顶点，
  配方常量是相位 π/8 的单位八边形），`buildShape(..., gems=true)` 用**两圈**记录
  （描边半径 `width+OUTLINE`、填充半径 `width−OUTLINE`）——Re_ADOJAS 的 `createGemsMesh`
  中间那圈黑八边形与两侧同色、视觉不可分，所以不需要第三圈；端盖顶点它也**不引用**。
  `P_GEM` 让 canonical 槽位从 175 → **193**（砖 142 + 图标 51），索引表 426 → **474**，
  （2026-10-18 起图标改在片元里画，槽位/索引只剩砖：**142 / 330** —— 见 §"图标"）
  形状记录 25 → 29 texel；`tile.vert` 的 `fetchRec` 里 `layer * 6` 同步改成 `layer * 7`，
  `tile_cpu.vert` 的槽位/层边界常量也同步。几何逐位对拍（`geom_probe`，197 万坐标）**0 失败**。
* 逐砖 alpha（8 位 hex 的 AA）。
* ~~TweenColor~~ —— 2026-10-17 已落地（见下）。
* 向导 Colors 页现在只影响高亮/`--fill` 覆盖（谱面颜色优先），UI 文案要跟着改。

## 半透明砖的自重叠（2026-10-18）

一块砖的 fill 是**多个 part 拼出来的**（`P_CIRCLE` 圆盘 + `P_WEDGE` 扇区 + `P_CAPS` 端帽），
这些 part 在同一 tile 内**互相重叠**；alpha=1 时同色看不出来，`#RRGGBBAA` / MoveTrack 把 alpha 降到 0.7 后，
重叠处被混合两次（0.7×0.7≈0.49）→ 砖内部出现**更暗的阶梯块**。

做法（零几何改写、零额外采样）：

* 砖 pass 显式 `glEnable(GL_DEPTH_TEST)` + `glDepthFunc(GL_LEQUAL)`：同深度时**后画的赢**；
* 顶点着色器把**描边层 z 拉近 2e-4**（`pos.z -= (1-layer)*2e-4`，`layer=0` 描边 / `1` 填充）；
* **图标不是几何**：它画在砖的**片元着色器**里（`tile.frag`，与 Re_ADOJAS `instanced.frag` 同一条路），
  所以既没有独立 part、也没有 z、也不受绘制序影响；`glDepthFunc(GL_LESS)` 保持原样
  （同一 tile 内同层 part 自重叠只在 LESS 下不会双叠 —— 换成 LEQUAL 会让半透明砖出现暗阶梯，
  2026-10-18 用户实测）；
* 几何探针（`GEOM_PROBE`）路径**不加**这两个偏移，逐位基准不变。

代价：不透明砖的描边/填充在**共面边界**上会有 1px 级重排（描边压过填充），54 个像素基线按新行为重存；
`angles360` 这类纯不透明状态字节差为 **0**（实测 arc119 diff 0）。

## 拖尾采样率：固定率 vs 自适应上限（2026-10-17，2026-10-18 去掉静默下限）

用户显式固定率（`adaptive=false`）现在**完全按用户值**：不再被 `rateMin` 抬（以前设 10/s 会变 60/s），
只在 `maxPoints` 环容量真的装不下时截断，且**每次截断都 `LOG_W`**（去重 2s），不再静默。
`rateMin/rateMax` 只对自适应 governor 生效。

## MoveTrack 的旋转/缩放枢轴（2026-10-18 修 410 路径）

`aPivot = 0.5×(cos dir, sin dir)`（砖心）；`tile.vert`（4.10 生产路径）此前**完全没读 aPivot**，
旋转/缩放仍绕砖起点，和 `tile_cpu.vert`（绕砖心）出图不一致。现两条路径同一算式：
`q=(pos-aPivot)*scale → 旋转 → +aPivot + aMove`；identity（rot=0/scale=1）仍走 `pos.xy + aMove` 逐位不变。

## 剔除粒度与细块数（2026-10-18）

`CullIndex::kSubPerBlock` 原为常量 8（=4096/512），但 wizard 允许 8192/16384 粒度：每块应分别有
16/32 个细块。旧实现只建前 8 个 → 粒度 8k/16k 时每块后半的砖**永远不交出**（静默漏画）。
现在 `m_subPerBlock = m_floors / kSubFloors`，ctest `cull_index` 对 512/4096/8192/16384 四种粒度
逐一断言"索引覆盖全部砖"。

## 拖尾采样率：固定率 vs 自适应上限（2026-10-17）

拖尾采样率有两个来源，**语义不同**：

* **用户显式固定率**（Launcher Trail 页的 Sample rate / `--trail-sample-rate`，此时
  `trailAdaptive=false`）：只受 **`maxPoints = 8192`** 的硬上限约束。
* **自适应 governor**（默认）：把帧预算换算成采样率，夹在 `[trailRateMin, trailRateMax]`
  （默认 60..4000）。

以前两条路都走 `clamp(rate, rateMin, rateMax)`，于是用户在 Launcher 里把 Sample rate 调到
64000 会被**静默夹回 4000** —— 设置形同虚设。The Moon（BPM 14817，一砖 ~4ms）在 4000/s 下
只有 **0.75 采样/砖**，拖尾是折线、看着一顿一顿的，正是这个原因。现在
`TrailSamplingConfig::adaptive` 区分两条路：显式固定率只保留 `rateMin` 与 `maxPoints`
（64000 → 实际 20480/s、8194 点；实测每帧采样 ~0.9ms，仍远低于 3.1ms 的 320fps 预算）。

抓帧路径不受影响：`--capture` 强制 `trailAdaptive=false` 且门槛状态的率都是默认 200/s。

## Coordinate System

OpenGL world space: X right, Y up. View matrix always at origin —— 相机相对偏移在 **double** 里算完，
**只窄化一次**再上传 float。

**绝对世界坐标绝不进 float**（2026-10-08 修的一个真 bug，血的教训）。`Tile::position` 是 double，
但 `TileMesh` 曾经把它直接窄化成 float 存下来；primer-final（一条 `y=0`、x 从 0 直走到 6.84e7 的
直线谱，68,411,077 砖）实测：

```
posX=[0.0, 68411076.0]  posY=[0.0, 0.0]   float ulp @ 末端 = 8 砖
相邻砖对在 float32 里是同一个点：34,693,868 / 68,411,077 = 50.7%
```

表现就是"**轨道不渲染**"：zoom 260 下可见 49 块砖，量化后塌成约 6 个位置（用户截图里只剩几块大板）。
行星是同一个病的第二处：`LevelScene` 把 double 的 `frame.redPosition` 窄化成 float 存进
`Planet::position` 再减相机 → 砖与行星各自量化到**不同**格点（实测红色行星整颗丢失、蓝色行星偏一格；
`tile=65000000` 的抓帧 **22,344 px / 9.70%** 不同，最大通道差 255）。

现在的规矩：

* **砖位置按块存相对 float**：`TileMesh` 每 `kPosChunkShift=16`（65536）砖记一个 double 原点
  （`m_chunkX/m_chunkY`），块内偏移用 float（块内 ulp ≤ 0.008 砖），一切读取走 `tileX()/tileY()`，
  上传时才 `(float)(tileX(t) - camX)`。代价 16 B/块 = **0.00024 B/砖**。
* **行星位置存 double**（`Planet::position` 是 `glm::dvec3`），相机相对化在 `Planet::draw` 里做完再窄化。
* 拖尾本来就是对的：`PlanetTrail::setPoints(const double*)`，内部按自身中心重定心。
* **视锥边界也是 world → double**：`Camera::frustumBounds` 与 `TileMesh::draw/ensureDrawList`
  都收 `double`，`frustumChanged` 的 0.5 阈值也在 double 里比。改成 double 后 primer 抓帧逐字节不变
  （20 砖的边距本来兜住了 8 砖的量化），但它挡的是 2^31 档：float 的 ulp 会涨到 256，**把边距整个吞掉**。

**别退回去**：`(float)pos - (float)cam`（各自窄化再相减）会叠加两次舍入；`glm::vec3((float)x, …)`
存绝对坐标同理。判据很简单 —— 只要某个值可能是 1e7 量级的世界坐标，它就不能是 float。

## Rendering

### Shader files
GLSL source in `assets/shaders/` — loaded from files at runtime via `Shader::compileFile()` (asset lookup: CWD → exe dir → macOS `.app` `Contents/Resources` → up to 3 parents above the exe, see `app/LevelScene.cpp`). Hitsound WAVs live in `assets/hitsounds/` (`audio/HitsoundManager.cpp::findAssetsDir`, same order). On macOS the exe path is `realpath`-resolved first, so launching through a symlinked `.app` (e.g. `/Applications/ADOCAO.app`) still finds the bundle's own assets. `scripts/make-app.sh` copies `assets/` into the bundle. Embedded fallback strings remain in `render/Shaders.hpp`.

### Z-depth render order
Tiles and icons use depth test ON with per-instance Z values encoding far-to-near order. Ortho far plane reduced to 200 for depth precision (~755K steps, supports 7M-tile levels). Z allocation:
- 砖（描边与填充都）: 局部 z = 0，实例偏移的 z = `tileZ(i, n)`（0.0 far → 9.0 near）——
  同砖的两层**共面**，靠 `glDepthFunc(GL_LEQUAL)` + "描边先、填充后"的索引序让填充盖住描边
  （这也是绘制顺序敏感的原因，见上）
- 图标: **没有独立几何**。画在砖的片元里（`tile.frag` 用砖心局部坐标 + `aIconBits`），
  与砖同一个片元/同一次 draw，天然跟着砖的 z 与 alpha —— 不需要 z 偏移或深度技巧
- Planets: Z = 9.5 (always in front)
- Trails: depth test OFF, always visible
- Highlight: depth test OFF, always visible

### 每实例属性（2026-10 起：**1 实例 = 1 砖**）
- **静态配方表**（VBO，175 项）：每个 canonical 槽位 `(part, layer*64+slot, k0, k1)` ——
  砖 6 个 part × 2 层（描边/填充）共 142 槽；索引表 110 个三角形（330 索引），**所有形状共用一份**。
  图标（twirl / ssUp / ssDn）不是 part：`aIconBits` 进 FS，由 `tile.frag` 在砖心画圆。
- **形状表**（RGBA32F 纹理 + `texelFetch`）：每个形状 14 条 8-float 记录（7 part × 2 层）+ 1 条活动位掩码 = **29 texel**。
  记录内容 = `render/TileShape.cpp::buildShape()` 存下来的参数（cos/sin/pow 的结果都在这里）。
- **每实例（每帧上传、只含可见）**：`[offX, offY, offZ, shape, iconBits]` — 5 floats。
- 颜色/不透明度走 **uniform**（改造前是逐实例 `[fill*3, stroke*3, opacity]` 7 floats，28 B/实例）。
- `aType` 仍然在拼色：VS 里 `mix(uStroke, uFill, layer)`；图标的三种颜色在 FS 里按 `bits` 选。

### 可见性 / 剔除缓存

**⑦ 容量边界（2026-10-16 起有第一道闸）**：极端视距下"整张一帧"会让主线程卡死几十秒 —— 实测用
`~/Documents/Charts/15. Singularity…` 抽出的"一分钟大圆"（**10,555,392 层**、螺旋每圈只错开 1.5 砖）
在低视距下**一帧要提交 10,555,392 个实例**（20 B/个 ≈ 211 MB），`work max = 36.2 s`，macOS 直接转
沙滩球。现在 `TileMesh` 有两道闸：

* `kMaxCullCandidates = 524,288`：命中块太多时降级为**就近细块**
  （`collectHitSubNearest`：在 **512 层细块**粒度上按到相机的距离排序，取到 `kMaxCullCandidates`
  为止，再按砖号升序重排后逐段剔除）。**三个错误做法都真踩过**：
  ① "超预算就不画" → 整帧黑屏（只剩行星/拖尾）✗；
  ② "按**粗块** AABB 到相机的距离挑最近块" → 粗块 AABB 是一整圈的包围盒（~1300 单位），小视锥会被
  几百个块"包住"（距离全为 0），挑出来的块里可能一块砖都不在视锥内 → `drawn=0`、轨道整个消失 ✗；
  ③ 纯"按层号顺序边剔边收" → 相机在几百万层处时只会收谱开头那一段，屏幕同样空白 ✗。
  现在的小视距实测：满预算 0 块，关预算 5,284 块（同一张谱同一个 t）；细块距离与全扫
  在可见数小时逐元素相同（3,903 / 5,130 / 5,284 三个时刻两种路径一模一样）。
* `kMaxInstances = 262,144`：可见实例**确定性截断**（按砖号取前 N）。
* 两个值**必须高于门槛里任何合法状态**：压到 `1<<16` 时门槛立刻 50/51 ✗（有个状态本来就抽 >6.5 万），
  回到 `1<<18` 后 51/51 ✓。
* 效果：最坏帧 **36.2 s → 85.7 ms**（同一张谱、同一 zoom 1），轨道照常画出（drawn = 262,144）；
  门槛 51 个状态逐字节不变（它们最宽只抽 5,407 个实例）。
* 还没做：真正的"整张一帧"策略（例如按屏幕覆盖率挑块、或多帧渐进补全）——当前是"就近降级 + 截断"。


**每砖派生数据不再常驻**（2026-10-16，③ 切片 4 渲染侧）。原来 `TileMesh` 常驻四支每砖数组
（`m_tileShape` 4 B + `m_posX`/`m_posY` 8 B + `m_iconBits` 1 B = **13 B/砖**，MYC 84 MB）；
现在只保留**形状表 + 形状索引 + 每块 double 原点 + 剔除索引 + 可见位**，剔除时按 **4096 层窗口**
用 `TileMesh::deriveRange()` 从 provider 现派生（每窗 4096 × 13 B = **53 KB**，128 槽环形 ≈ 6.8 MB 上限）。

* 口径唯一：`deriveRange()` 同时被 `build()` 与剔除用，所以"整段"和"按块"一致**由构造保证**；
  `ADOCAO_MESH_DUMP=1` 打整段派生的 FNV，可与"数组时代"的记录值直接对比（实测四个全等）。
* 三条踩过的坑：① `TileShape::keyForTile` 原来直接读 `lv.tiles[i].direction`（丢数组后越界崩）；
  ② 窗口化时**不能清 `tileHasTwirl/tileHasSetSpeed`**（图标位是剔除时才派生的，清了得到 icon=0）；
  ③ 并行剔除段**必须主线程先 pin 窗口**（worker 碰共享缓存会撕数据），命中块数 > 槽位数或剔除块
  粒度 > 4096 时退回串行。
* 代价与验收：`--capture` 下 `cull` 段 0.009 → **0.059 ms**（MYC z25，整帧 36 ms 的 0.16%）；
  像素门槛 51/51 逐字节；常驻 footprint 388.2 → **311.3 MB**、live 199.8 → **114.0 MB**。


`draw()` 只为**可见**实例建一份压实列表（一个全局缓存，容差 0.5 与改造前同）：视锥变了才重新
SIMD 剔除（按绘制序切块并行，`render/TileMesh.cpp` 的 `getPool()`，Meyers 静态、活到进程退出），
否则只按 (dx,dy) 平移相机相对偏移。世界 AABB = 形状局部包围盒（`Shape::localMin/Max`，double）+
砖位置 —— 与改造前 `cullAndOffsetGroups` 同一条表达式（逐位相同）。

**砖之间的提交顺序：2026-10-15 起 = 按层号升序**（原来 = `unordered_map<GeoKey>` 迭代序 +
组内下标降序）。历史：旧注释记过"反转组内顺序 → MYC t=30s 那帧 102 个像素不同（maxdelta 111）"，
那是**旧的全局 Z 归一化**下发生的 —— 相邻砖的深度被量化到同一个值，重叠处谁赢由先后决定。
改成**按可见窗口归一化 Z**（见下）之后窗口内步长 ≈0.018 ≫ 深度分辨率 1.19e-5，并列基本消失，
所以顺序不再影响出图：**像素门槛 51/51 逐字节不变**，另在三种 Won't You + TNR + primer-final
上做了 16 帧 A/B（含"整张一帧"6,841 万块）**全部逐字节相同**。
**（2026-10-15 后续：该 A/B 的钩子随 `m_drawOrder` 一起删除了** —— 绘制顺序既然对出图不可观测，
那支 4 B/砖 的排列数组就是纯冗余（MYC 实测常驻 **−26.3 MB**、2³¹−1 外推 8.6 GB），
**绘制顺序（2026-10-21 定案）**：**默认倒序**（`renderOrder = -tileIndex`，与 Re_ADOJAS 一致）：
楼层大的先画、早的后画压在邻居上面，Neon 的「黑填充 + 亮描边」在接缝处才连续（修「被狗啃」）。
与 2026-10-18 的录制版（旧升序）在重叠接缝处差一圈描边（t=118 s 逐像素 2,207），但**裁决权在
Re_ADOJAS**，不是旧版行为。A/B：`ADOCAO_DRAW_ORDER=ascending`。历史：`ADOCAO_DRAW_ORDER=grouped` 与 `scripts/order-ab.sh` 已移除。**要重做这个 A/B，就临时改
`TileMesh` 的提交顺序**（把下标换成"按形状分组 + 组内降序"的排列）再抓帧比对 —— 别留一个
"设了环境变量但没人读"的钩子，那会变成永远绿的假对照。**）**
残留的理论情形只有"极端广角 + 屏幕上真的重合但颜色/图标不同的砖"（那 16 帧没造出来），
且那时胜负也是**确定**的（低层号在上），不再是哈希序。
**⇒ 于是那支排列数组也删了**：`m_drawOrder`（4 B/砖）在 2026-10-15 被移除，剔除与上传**直接用下标**
（下标即砖号）。实测（同一份二进制、MYC 6,770,912 砖、`app/ProcMem.hpp` 的**常驻**口径）：
建 `TileMesh` 前 831.3/831.6 MB → 后 **919.5 MB**（无数组）vs **945.8 MB**（临时加回 4 B/砖的数组）
= **省 26.3 MB**（预测 6.77M×4 B = 25.8 MB ✓；2³¹−1 外推 **8.6 GB**）；出图与删前**逐字节相同**、
像素门槛 **51/51**。注意这笔用**峰值** RSS 量不出来（1429.5 → 1428.9 MB）：大谱的峰值在**加载期**，
渲染侧数组是加载之后才建的 —— 所以要量它必须用常驻口径（见 `app/ProcMem.hpp`）。

⇒ 因此**不要再依赖"必须复现旧绘制序"**，但如果你把顺序改成"非层号序"的任意排列，
仍要跑门槛 + 巨谱 A/B 再判。）

### ④ 剔除：粗块 AABB 索引（2026-10-15）
原来是"遍历全部 n 块砖逐个判 AABB-视锥"，每次视锥变化 O(n)（MYC 6.77M 砖实测 3.1 ms、
primer 6841 万砖 **~114 ms**）。现在 `render/CullIndex.hpp`：每 **4096 层**一个保守 AABB
（含砖半径外扩；`m_cullHalf` = 所有形状 localBBox 的最大外扩），查询先在块表上筛（命中块按
层号升序），再在命中块内调用**原来那个** `cullRange` —— 谓词完全没动，所以输出与全扫
**逐元素相同**（`tests/cull_index_test.cpp` 对拍：直线/圆/随机游走/螺旋/真实谱 + 反面对照；
ctest `cull_index`）。块按层号升序输出 ⇒ 天然就是上面那个提交顺序，**不需要排序**。

* 索引体积 **0.0098 B/砖**（40 B/块；MYC 66 KB、primer 652 KB；2³¹−1 外推 21 MB），
  建表一次性 ~1.45 ns/砖（MYC 10 ms、primer ~100 ms），`n < 4096` 时不建（走原全扫）。
* 粒度依据：`tests/cull_cost_probe.cpp --blocks 64…32768` 实测扫表 ≈0.42 ns/块、块内 ≈0.47 ns/砖，
  MYC 的最优就是 4096；折叠谱（Singularity）最优是 256，但 4096 也只是 0.117 ms（帧预算 0.7%），
  而内存差 16 倍 → 取 4096。折叠谱要更快就按需给"命中且候选大"的块建细表（两级），别全局铺。
* 候选少时**不开线程**（`kCullSerialMax = 262144`）：唤醒 + 每任务两个 vector 比串行还贵
  （实测 Singularity zoom 25 并行比串行慢 ~1 ms）。
* 端到端（`--capture` 的 `render_ms`，每格 3 次取最小；`ADOCAO_CULL=scan` 回全扫，
  两路 **PNG 逐字节相同、drawn 相同**）：

  | 帧 | 索引路 | 全扫路 | 省 |
  |---|---|---|---|
  | MYC zoom 25（画 178）| 36.14 ms | 38.21 ms | 2.07 ms |
  | MYC zoom 1（画 3,025）| 36.76 ms | 40.71 ms | 3.95 ms |
  | Singularity zoom 25（画 3,765）| 36.99 ms | 38.24 ms | 1.25 ms |
  | **primer zoom 25（画 127）** | **42.95 ms** | **156.62 ms** | **113.67 ms** |
  | primer 整张一帧（画 68,411,077）| 3,381.97 ms | 3,492.85 ms | 110.88 ms |

  （抓帧帧时含 ~36 ms 固定开销，所以噪声底 ~1.5 ms —— MYC/Singularity 那三格的差值贴在本底上，
  primer 那格才是这个改动真正要解决的东西。）
* **粒度是用户可选的**（CLI `--cull-block N`、wizard 的 Graphics 页"剔除块粒度"下拉；
  0 = 关闭索引走全扫；`ADOCAO_CULL_BLOCK=<n>` 是 env 覆盖，测试/A-B 用）。合法范围
  64…1048576，超范围**钳制并 LOG_W**（这是性能旋钮，不是正确性开关）。
  **关键性质：粒度只影响成本与内存，不改出图**。细块数必须随粒度变
  （`m_subPerBlock = m_floors / kSubFloors`；旧实现写死 8，8192/16384 会**静默漏画**每块后半，
  2026-10-18 修复并加 ctest 覆盖）。复测：2025 t=6 的 4096/8192/16384 三档 PNG **逐字节相同**；
  历史上 Singularity zoom 25 上 8 种粒度（0/256/…/65536）也是逐字节相同（块内仍走同一个
  `cullRange`，块序 + 砖号序 ⇒ 输出逐元素相同）。日志每次加载都会打印
  块数 / 字节数 / **2³¹−1 外推** / 建表耗时，方便按内存预算挑值。
* 未做：块表**在 pack 期算好写进 `.adocao`**（现在加载时建）、折叠谱的两级细表、
  以及"编辑/预览要回答任意位置的砖"时的空间索引。

### 剔除边距 / TrackVis
`draw()` 的视锥外扩 20 单位（与改造前相同）。TrackVis 的可见位是**每砖 1 字节**（首次
`updateVisibleRange()` 时才分配；判据与消费方 `app/LevelScene.cpp` 的 `m_tileVisEnabled` 一致），
砖隐藏时它的图标一起消失（= 改造前行为）。

### Memory management
`LevelData::releaseMemory()` frees angleData, actions/decorations JSON, tilePositionOffsets after loading. `tileBPMs` kept — needed by `buildIcons()` for SetSpeed icon coloring.

### Frame pacing
Sleep-based: `sleep_for(remaining - 1ms)` + spin last 1ms for precision. 320 FPS soft cap. DPI awareness + CPU pin to performance cores on Windows.

### Event icons（2026-10 起**并入砖的实例流**）
Twirl（紫）、SetSpeed up（红）、SetSpeed down（蓝）。每砖 1 字节 `iconBits`（bit0 twirl /
bit1 ssUp / bit2 ssDown，判据 = 改造前 `buildIcons()` 的 `tileBPMs[i]/tileBPMs[i-1]` 1.05/0.95
阈值），Z 由 VS 按 part 算：twirl `砖Z+0.002`、SetSpeed `砖Z+0.002+(有twirl?0.003:0.001)`。
- **独立图标实例集已删除**（改造前 MYC 有 616 万个图标实例，外加 CSR 索引 / 独立可见位 /
  独立颜色 VBO）。现在图标 part 在不亮的砖上塌成一个点 → 零面积 → 不产生片元。
- **和砖在同一次 draw 里**，整条轨道 **1 次 draw**；图标不进索引表，而是在 `tile.frag` 里
  按砖心局部坐标画（所以它天然在砖的片元上，不会擦掉拖尾、也不吃深度顺序）。
  这里有一个**故意的行为修复**：改造前是"砖 → 拖尾 → 行星 → 图标"，图标画在拖尾**之后** ——
  图标不透明、拖尾是半透明混合，于是图标会把拖尾擦掉一块（The Moon t=1s 实测 97 个像素的 alpha
  从 255 变 154~194）。那是 bug（用户 2026-10 指出），所以图标必须**在拖尾之前**画：
  这个修复让"图标与拖尾重叠"的像素与旧版不同，是**预期内**的差异，基线已按新行为重存。
  永久覆盖：`tests/capture_states.txt` 里每个图标用例都有 `*_under_trail`（锚在图标砖之后 2 砖
  + `--trail-tiles 8`，否则默认 0.4 s 的拖尾在 BPM 60 的谱面上扫不到一砖）与 `*_under_planet`；
  把顺序改回老行为，这 8 个状态会红 13700~29241 个像素（maxdelta ~220）。

### 砖块几何 / 中旋砖（angleData=999）
**几何在 GPU 上按"形状表"展开**（2026-10 改造，决策 10 落地）：`render/TileShape.{hpp,cpp}` 把每个
形状键 `GeoKey(round(sa*100), round(ea*100), mid)` 解析成 canonical 布局里的 part 记录，
`assets/shaders/tile.vert` 按这份表算出顶点。**没有 CPU 顶点汤**：改造前的
`render/TileGeometry.cpp`（每形状一份 mesh + 每形状一次 draw）改名成
`render/TileGeometryReference.cpp`，只在测试里当 A/B 基准（和 `parseLegacy` 一个路子）。
角度约定：`sa = 上一砖 direction − 180`（= Re_ADOJAS 的 `pred`），`ea = 本砖 direction`；中旋砖的几何**只依赖 sa**。

**canonical 布局**（`TileShape.hpp`）：175 槽 = 砖 6 个 part × 2 层 + 图标 3 个 part × 17；
索引表 142 个三角形（砖 94 + 图标 48），**所有形状共用**。part 与模式：
`ARC(0<ang<120°) = CIRCLE+WEDGE+CAPS`、`BIG(ang≥120°) = BIGQ+CAPS`、`ZERO(ang==0，U 型) = CIRCLE+EXT4`、
`MIDSPIN = PENT`；不活动的 part 整块**塌成一个点** → 零面积 → 不产生片元（所以一次 draw 能画所有模式）。
每形状只存 12 条 8-float 记录（+1 条活动位掩码 = 25 个 texel），实例只带形状号。

中旋砖 = **五边形**：以 `a1 = sa` 为轴，`0..0.275` 是宽 0.55 的方块（半长 = 半宽 = `TILE_WIDTH`，比普通砖的 0.5 短），
再向来路伸出 depth = 0.275 的**尖角**，整体沿 −a1 平移 0.04；7 顶点 / 3 三角形；描边层 = 各 +OUTLINE。
- **尖角是正确的**（2026-10 确认），别去找"圆角五边形 / 原版 curvaturePoints=3"版本：全站 `gh search code "curvaturePoints"` 0 命中，
  AdoCpp 的 `m_interpolationLevel` 传进 `createMidSpinMesh` 后根本没用。
- 参考实现三家一致但**同源**（不是三方独立验证）：`StArray/A4GDX` Java → `adofaiex/AdoCpp` C++（注释 "Thanks for StArray's code"）→ `adofaiex/Re_ADOJAS` TS。
- 旧实现是 `mid ? createTileMesh(eA,eA,sc)`（`ang == 0` 分支 = 圆 r=0.30 + 方块，看着像"大圆+菱形/发卡弯"），已删除。
- 谱面里中旋是 `angleData` 里的**数字 999**（不是 `"!"`；`"!"` 只是编辑器的显示/`LevelData` 字符映射）。
  本地实测：The Moon - Coal 1307 个、Singularity 442、Won't You Make a Song with Me 11、Fledgling 9。

### 形状表的容量闸（2026-10-28，Issue #2）

形状表是**一张** `1024 × rows` 的 RGBA32F 纹理（`TileShape::kTexW` × rows，**29 texel/形状** =
7 part × 2 层 × 2 texel + 1 条标志），行号 = 实例里的形状号。它有两个上限，**任一越界就整条轨道消失**：

* 驱动的 `GL_MAX_TEXTURE_SIZE`：Intel UHD、macOS、2011 年的 GTX 590 都是 **16384** 行；
* 显存预算：`TileMesh::kMaxShapeRows = 4096` 行（1024×4096×16 B = **67 MB**）。

**超过上限时的失败是静默的**，这是 Issue #2 的全部内容：谱面 `hertz.adofai` 有 3,583,664 砖、
**3,560,627 个唯一形状**（BPM 65536 的谱面几乎每块砖的角度都不同）→ 行数 100,839 ≫ 16384 →
`glTexImage2D` 被驱动拒绝 → 纹理"不完整" → `texelFetch` 一律返回 0 → 每块砖的活动位掩码 = 0 →
所有 part 塌成零面积 → **画面纯黑，程序不卡死、日志一条错都没有**（Intel 驱动只在调试输出里嘀咕
一句 `texture unloadable`）。

现在两道防线（`render/ShapeKey.hpp` + `render/TileMesh.cpp`）：

1. **容量闸 + 放粗阶梯**：`build()` 先按 `shapeTableRows()`（= min(4096, GL 上限)，下限 64）算出能
   装多少个形状，然后**先用 0.01° 这档试**，装不下才逐档放粗
   `0.01° → 0.02° → 0.04° → 0.1° → 0.2° → 0.5° → 1°`（每档最多 2.5 倍，落点离容量上限不会差太多），
   每放粗一档打一条 `LOG_W`。**1° 封底，更粗的档位已删除** —— 1° 网格下 (sa, ea) 最多 360×360，
   实测那个随机角度谱 1° 就是 130,321 个形状 = 3,691 行，离 16384 行还差四倍多。
   放粗只改砖的**角度**（几何），**位置与时长完全不受影响**。
2. **纹理守卫** `checkShapeTexture()`：装完直接问驱动这张纹理在不在
   （`glGetTexLevelParameteriv` 的宽高 + `glGetError`），没建成就是一条 `LOG_E`（含请求尺寸、
   驱动实际给的尺寸、GL 错误码、容量闸的数字）。**负向对照验过**：临时把 GL 上限那一步去掉 →
   `请求 1024x19819，驱动实际给 0x0，GL 错误 0x0501` + 驱动侧 `texture unloadable` 同时出现。

**基准档 0.01° 不动**（铁律，2026-10-28 用户："**没碰壁根本不用动啊**"）。这个"壁"指的是容量：
没有一张谱是"装不下"才需要更细的精度，所以精度**只在装不下时放粗，永远不主动改几何**。
走过一次弯路要记着：曾把基准提到 float32 量级（1/16384° = 6.1e-5°）并 ×2 放粗 ——
5 张门槛谱全部落在最细档（说明没有谱需要它），但 Singularity 有 18 块砖的角度不是 0.01° 的
整数倍，几何因此变了 → 像素门槛 152 px 变化、`singularity_mid` 基线得重存。
"更准"不是理由：门槛是回归网，重存基线等于把参照弄丢一次。已撤销。

各档实测花名册（本机）：

| 谱面 | 形状数 | 行数 | 落在哪档 |
|---|---|---|---|
| MyC（677 万砖） | 294 | 9 | 0.01°（基准） |
| Singularity | 9,349 | 265 | 0.01° |
| The Moon | 215 | 7 | 0.01° |
| angles360 | 4,838 | 138 | 0.01° |
| 随机角度 358 万砖（Issue #2 同尺寸） | 130,321 | 3,691 | **1°**（前 6 档都装不下，总耗时 1.36 s） |

**颜色不在这个键里**（别把 16.7M 种颜色算进容量）：`styleOfTile()` 只把 `trackStyle` 映射成
**6 个几何档位**（Standard/Neon/NeonLight/Basic/Gems/Minimal，其它一律 Standard —— 与 Re_ADOJAS
`FLOOR_FUNCS[trackStyle] || Standard` 同口径），而 `trackColor` 的 `0x000000..0xFFFFFF` 走
**逐实例** fill/stroke（只对可见砖上传，6 float）。实测：200 砖 / 200 种不同 `trackColor` →
**2 个形状**（同一角度下的 stroke + fill 两种），抓帧里 101 种不同颜色。所以阶梯与颜色数无关。
反过来说：**绝不要为了"按颜色缓存几何"把颜色编进 style** —— 键里的 style 只有 **3 位**，
16.7M 个值会静默碰撞成"画错砖"（`tests/shape_key_test.cpp` 把这条字段宽度钉住了）。

两个容易踩的地方：① `build()` 与 `deriveRange()` 的键必须用**同一个** `m_shapeSteps`
（档位不同 → 键全对不上 → 每块砖都退回形状 0，画面全错，日志会喊"形状键在表里找不到"）；
② GLSL 3.3 的 CPU 回退路（`ADOCAO_CPU_GEOMETRY=1`）贴的是**槽位表**，142 texel/形状 →
同样的 4096 行只装得下 **29,537** 个形状（比 GPU 路少 4.8 倍），所以那条路遇到稠密角度的谱面
会一路放粗到 1° 并在守卫那里报错 —— 这是**已知边界**：那种机器（GL < 4.0）本来就画不动
三百多万砖的谱面。

验收：`ctest -R shape_key`（0.01° 基准与 7.2.0 逐位相同 / 各档位键无碰撞且在 20 位内 / 阶梯到 1° /
容量口径）+ 像素门槛 **54/54 逐字节相同**（GPU 路与 `ADOCAO_CPU_GEOMETRY=1` 两条都跑过）。

复现配方（不要往仓库里塞大文件）：`angleData` 每砖取 `round(uniform(0,360),4)` 的谱面，
700,000 砖 → 699,811 形状 → 修之前 `1024×19819` 也被拒（本机 `GL_MAX_TEXTURE_SIZE` = 16384），
修之后放粗到 1°（129,628 形状 / 3,672 行）；同帧非黑像素 0.30% → 14.41%。
粗档位到底会不会"走样"：1° 与 5°（旧阶梯的 CPU 回退路落点）两帧逐像素只差 **1.17%**、全在边缘 ——
放粗是"边缘移动"，不是"画面走样"（5° 这档已随"1° 以上删掉"一并移除，这条只是当时的量测记录）。

**逐位对拍的两种口径**：两个对拍测试的默认口径是**几何尺度上的绝对容差 1e-5 砖**
（打印 `逐位相同 / 非逐位但在容差内 / 最大偏差`），`ADOCAO_TILE_EXACT=1` 才要求**逐位**：
```
ADOCAO_TILE_EXACT=1 ctest --test-dir build -R 'tile_expansion|geom_probe'   # 本地验收/换驱动
ADOCAO_TILE_EXACT=1 ./build/tests/adocao_tile_expansion_test tests/charts/shapes_exhaustive.adofai
```
为什么默认不用"输出值的 ULP"当度量：**中旋 PENT 的 slot0/1 有相消**，中间量只差 1 ULP，输出上会
放大成几十 ULP —— CI 上 GCC 实测 13 个坐标差 4~64 ULP，但绝对偏差始终 ~6e-9 砖（y 只有 5.9e-4）。
拿 ULP 当门槛会误判，拿"砖"当门槛不会：真正的转写错误（符号/参数/顺序）是 1e-3 往上，而 zoom 1000
时 1 px 才 1e-3 砖。
本机（钉住的那台）两种口径都验过：**CPU 3298 万坐标、GPU 4708 万坐标全部逐位相同、最大偏差 0**
（GPU 侧只有 ±0 的符号差 19366 个，单独计数）。CI 的 Linux job 跑默认口径。
推论也要记着：换编译器/换驱动时"和改造前逐位一致"只在钉住的那台机器上成立，别的平台差 1 ULP 级 ——
8-bit 输出上看不出来（实测：把一处融合拆成 1 ULP 级扰动，7/7 验收状态完全相同）。

**位精确（改任何几何前先读这段）**：参考实现是 `-O3 -march=native` 编出来的，`a*b+c` 会变成
`fmadd/fmsub/fnmul`（实测 `TileGeometry.o` 里 45 fmul + 45 fmadd + 16 fnmsub）——**差 1 ULP 也算改坏**。
所以：
- 超越函数（`cos/sin/pow/fmod`）**只在 CPU**：结果进形状表；VS 里没有超越函数。
- VS 每条公式与 `TileShape.cpp::expand()` **逐字同结构**：融合步显式 `fma()`，要单独舍入的乘积用
  `rp(a,b)=fma(a,b,0)`（裸的 `a*b` 会被编译器合法地融合进旁边那次加法）；负系数一律
  `-(rp(w,m))`（`rp(-w,m)` 里那个 +0 加数会吃掉 -0.0 的符号）。
- **同一个算式在不同 part 里可能被编成不同结构**（PENT 与 EXT4 就是这样），所以每个 part 的融合结构
  都是**实测钉死**的，不是推出来的。改几何必须三个测试全绿：
  `tests/tile_expansion_test.cpp`（CPU 逐位）、`tests/geom_probe_test.cpp`（**GPU 逐位**，读的就是
  生产 `tile.vert`，画点进 RGBA32F FBO 再读回来比）、`scripts/capture-gate.sh`（像素）。
- `fma()` 要 GLSL 4.00+ → 上下文请求是 **4.1**（`app/GameWindow.cpp`；macOS 上限也是 4.1）。
  实测 3.3 ↔ 4.1 **像素中性**（当时的 34 个验收状态逐字节相同）。
- **`tileBPMs` / `tileHasTwirl` / `tileHasSetSpeed` 是"绘制期依赖"，不是加载期临时数据**：
  `TileMesh` 在**每帧**派生可见窗口的每砖数据时会读这三张表（`deriveRange` 读图标位、
  provider 合成 SetSpeed 事件读 `tileBPMs`）。所以：
  * `LevelData::releaseMemory()` **必须留着它们**（4 B/层 + 2 bit/层）；
  * 窗口化之后**绝对不能清** `tileHasTwirl/tileHasSetSpeed`（清了图标全变 0：实测块 0 有
    2148 处不一致、出图 858 个边缘散点）；
  * 编辑器改完事件用 `LevelData::refreshDerivedCaches()` 重建，别手搓这几张表。
- **拿不到 4.x 的退路（2026-10-17 落地）**：砖块 VS 有两支 —— `tile.vert`（410，fma 展开）与
  `tile_cpu.vert`（330，**查表**）。后者不在 GPU 上算几何：`TileShape::expand()` 在 CPU 上把每个
  canonical 槽位展开成 RG32F 纹理（每形状 175 texel，texel i = 槽位 i 的 (x,y)），VS 里只有
  `texelFetch` + 实例偏移的相加 —— **一次乘加都没有**，所以顶点值就是 CPU 参考值本身。
  * 选择靠**试编**：410 编不过才切（`app/LevelScene.cpp` 的能力探测），不按版本号猜
    （同 `archive/LzmaMtCompat` 的教训：版本区间 ≠ 实际能力）；`ADOCAO_CPU_GEOMETRY=1` 强制切。
  * 上下文侧：`app/GameWindow.cpp` 有 4.1 → 3.3 的重试链（`ADOCAO_GL_VERSION` 可指定首试版本）。
  * 两支**共用**同一个 VAO / 实例流 / 索引表 / 绘制序 / 剔除 / LOD，只有坐标来源不同 ⇒ 出图必须
    逐字节相同。验收：`ADOCAO_CPU_GEOMETRY=1 scripts/capture-gate.sh check` → **51/51 逐字节**
    （同一份基线）。单帧 A/B 也对过（angles360、4,838 形状，逐字节相同）。
  * 代价：一次性 `expand()` + 形状数 × 175 × 8 B 的纹理（MYC 45 形状 63 KB、门槛最宽的
    8192 形状 11 MB）。
  * **平台事实**：macOS 核心 profile 的地板是 4.1，本机造不出真 3.3 上下文
    （`ADOCAO_GL_VERSION=3.3` 请求到的仍是 4.1）—— 端到端只能靠强制开关验，
    别以为在这台机器上"切 3.3"就算验过。
- 跨平台备注：这套镜像结构是**对同一台机器/同一编译器**钉的；换编译器时 1 ULP 级差异在 8-bit
  输出上看不出来（见下），但 CPU/GPU 逐位对拍会红。

**机械护栏**：`tests/tile_geometry_test.cpp`（五边形三条不变量；注意顶点表顺序**不是**边界序，
边界序是 `0,1,2,4,3`，直接对 0..4 求 shoelace 会得到 `3w²/2`）+ 负向对照（旧几何 `createTileMesh(a,a)`
必须过不了）+ **调用点护栏**（源码级：`TileMesh.cpp` 必须出现 `TileShape::buildShape`，不许出现
`createTileMesh(`/`createMidSpinMesh(`、不许出现独立图标实例集，不许出现单独的 `drawIcons()`）。
看图工具在 `tools/tile-geometry-lab/`（注意它是针对旧的 CPU `TileGeometry` 的，别指望它画新管线）。

**像素门槛怎么用**（改渲染前先存基线，改完 check）：
```
scripts/capture-gate.sh store                                 # 改动前（默认 ~/Projects/.ADOCAO/gate/baseline）
# …改代码、重新构建…
scripts/capture-gate.sh check
```
清单在 `tests/capture_states.txt`（由 `tests/gen_render_fixtures.py` 生成，别手改）：
`<name>|<chart>|<tile|time>|<值>|<zoom>|<WxH>|[额外开关]`。按**砖号**抓帧（`--capture-tile`）而不是按秒 ——
6 千砖的谱面上按秒给的时刻会随砖时长累积漂移。脚本会断言日志里的 `tile=` 与清单一致（防"抓错时刻"
这种静默失败），并记录 RSS / `unique shapes` / draw 次数。差异像素统计要 Pillow：
`ADOCAO_GATE_PY=<带 Pillow 的 python3>`。**基线不入库**（驱动相关），`store` 一次、`check` 多次；默认落在 `~/Projects/.ADOCAO/gate/baseline`（`ADOFAO_GATE_HOME` 可改）—— 别写 `/tmp`，会被系统清掉；也别写进仓库，`git clean -xdf` 会连它一起删。
三条已标定过的灵敏度：砖的绘制顺序反转 → MYC 那帧 278 个像素变；**图标放回拖尾之后**
（旧 bug 行为）→ 8 个 `*_under_trail` 状态红 13700~29241 个像素（maxdelta ~220）；
而把一处融合拆成 1 ULP 级扰动 → **7/7 状态完全相同**（8-bit 输出看不出来）—— 所以 ULP 级的
正确性由上面那两个逐位测试负责，别指望 PNG。清单支持第 7 个可选字段传额外开关
（例如 `--trail-tiles 8`，用来让拖尾真的扫到图标）。

**怎么"实际看"**（本轮就是这么验的，别再靠脑补）：
1. 几何层：`tools/tile-geometry-lab/dump.sh`（编的就是 `render/TileGeometry.cpp` 本人）→ 顶点 JSON →
   `json2svg.mjs` → `svgshot.cjs`（Playwright + **系统无头 Chromium**）→ 看图。三条路子和坑表都在
   `tools/tile-geometry-lab/README.md`；
2. 渲染层：`./build/adocao <谱> --auto-play --fullscreen` + `screencapture -x`；最省事的验证谱是几层里放一个中旋，
   下面这段可以**直接存成 .adofai**（第 2 层中旋，合法 JSON，别再加省略号）：
   ```json
   {"angleData":[0,999,0,0,0,0],"settings":{"version":15,"bpm":60,"offset":0,"trackColor":"debb7b","backgroundColor":"000000"}}
   ```
   （文档里的示例谱也是代码：改完必须过 `json.loads` + `./build/tests/adocao_level_parse_test <file>`。
   本轮真踩过——先写成带 `…` 的假 JSON，谁复制谁中招。）
3. 对拍：同一张谱用 Playwright 喂给 Re_ADOJAS 编辑器首页的 `input[type=file]`（`.adofai`），两边画出来应当一致。
   注意 Re_ADOJAS 前端是 **HashRouter**：`http://127.0.0.1:3144/#/editor`，`/editor` 只会给你首页。

### Highlight
选中砖用反转色画（`highlight.frag` 里 `1.0 - vColor`）。**共用** `tile.vert`（几何展开只有一份实现），
颜色走与砖相同的 uniform；`drawHighlightedTile()` 往实例缓冲里写 1 条记录再画 1 个实例。

