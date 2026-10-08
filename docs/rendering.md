# 渲染（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

## Coordinate System

OpenGL world space: X right, Y up. View matrix always at origin — camera-relative offsets computed in double, converted to float for GPU.

## Rendering

### Shader files
GLSL source in `assets/shaders/` — loaded from files at runtime via `Shader::compileFile()` (asset lookup: CWD → exe dir → macOS `.app` `Contents/Resources` → up to 3 parents above the exe, see `app/LevelScene.cpp`). Hitsound WAVs live in `assets/hitsounds/` (`audio/HitsoundManager.cpp::findAssetsDir`, same order). On macOS the exe path is `realpath`-resolved first, so launching through a symlinked `.app` (e.g. `/Applications/ADOCAO.app`) still finds the bundle's own assets. `scripts/make-app.sh` copies `assets/` into the bundle. Embedded fallback strings remain in `render/Shaders.hpp`.

### Z-depth render order
Tiles and icons use depth test ON with per-instance Z values encoding far-to-near order. Ortho far plane reduced to 200 for depth precision (~755K steps, supports 7M-tile levels). Z allocation:
- 砖（描边与填充都）: 局部 z = 0，实例偏移的 z = `tileZ(i, n)`（0.0 far → 9.0 near）——
  同砖的两层**共面**，靠 `glDepthFunc(GL_LEQUAL)` + "描边先、填充后"的索引序让填充盖住描边
  （这也是绘制顺序敏感的原因，见上）
- 图标: 局部 z = 砖 Z + 0.002（twirl）/ +0.002+0.003 或 +0.002+0.001（SetSpeed，看同砖有没有 twirl）
- Planets: Z = 9.5 (always in front)
- Trails: depth test OFF, always visible
- Highlight: depth test OFF, always visible

### 每实例属性（2026-10 起：**1 实例 = 1 砖**）
- **静态配方表**（VBO，175 项）：每个 canonical 槽位 `(part, layer*64+slot, k0, k1)` ——
  砖 6 个 part × 2 层（描边/填充）+ 图标 3 个 part × 17 槽；索引表 142 个三角形，**所有形状共用一份**。
- **形状表**（RGBA32F 纹理 + `texelFetch`）：每个形状 12 条 8-float 记录 + 1 条活动位掩码。
  记录内容 = `render/TileShape.cpp::buildShape()` 存下来的参数（cos/sin/pow 的结果都在这里）。
- **每实例（每帧上传、只含可见）**：`[offX, offY, offZ, shape, iconBits]` — 5 floats。
- 颜色/不透明度走 **uniform**（改造前是逐实例 `[fill*3, stroke*3, opacity]` 7 floats，28 B/实例）。
- `aType` 仍然在拼色：VS 里 `mix(uStroke, uFill, layer)`（图标 part 直接用各自的常量色）。

### 可见性 / 剔除缓存
`draw()` 只为**可见**实例建一份压实列表（一个全局缓存，容差 0.5 与改造前同）：视锥变了才重新
SIMD 剔除（按绘制序切块并行，`render/TileMesh.cpp` 的 `getPool()`，Meyers 静态、活到进程退出），
否则只按 (dx,dy) 平移相机相对偏移。世界 AABB = 形状局部包围盒（`Shape::localMin/Max`，double）+
砖位置 —— 与改造前 `cullAndOffsetGroups` 同一条表达式（逐位相同）。

**砖之间的绘制顺序必须与改造前逐字相同**（`unordered_map<GeoKey>` 迭代序 + 组内下标降序），
因为深度 24 bit 下相邻砖会量化到同一个深度，重叠处谁赢由先后决定：实测反转组内顺序，
MYC t=30s 那一帧有 102 个像素不同（maxdelta 111）；而给形状表 `reserve()` 改了桶增长序列、
形状顺序变了，会让 278 个像素从 stroke 变 fill。所以这里**不要**为了"看起来更整洁"去改容器、
加 reserve、或换排序 —— 门槛会红，而且红的理由不直观。
（**例外**：图标与拖尾的先后是**故意**改过的 —— 旧行为是 bug，见下面 Event icons 段。）

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
- **和砖在同一次 draw 里**（索引表顺序：砖描边 → 砖填充 → 三个图标 part），整条轨道 **1 次 draw**。
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
  实测 3.3 ↔ 4.1 **像素中性**（当时的 34 个验收状态逐字节相同）。不想要 4.1 的退路见 TODO.md。
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

