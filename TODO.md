# TODO

> 未完成清单已定稿（移除：~~GPU compute culling~~——2.0.0 起已不需要，彻底删除；~~Decoration~~）。
> MoveTrack 保留，归入「遥远的未来」，仅记录不排期。已完成项保留在文末作历史。

## 未完成

### 打拍音
- [ ] ColorTrack（COLOR_FUNCS / FLOOR_FUNCS / RecolorTrack）
  - 蓝图决策 8/9：只做静态；颜色逻辑归 render；几十万事件走 GPU/GLSL。

### 渲染
- [x] **TileMesh 改 GLSL 几何 + 图标并入**（2026-10 完成，蓝图决策 10）：
      1 实例 = 1 砖、几何在 VS 按形状表展开、图标并入实例流；draw 次数 4838/8192/294 → **1**。
      顺手修掉一个 bug：旧顺序"砖 → 拖尾 → 行星 → 图标"让不透明的图标把半透明拖尾擦掉一块
      （图标现在跟砖一起画，在拖尾之前；`*_under_trail` 8 个状态覆盖，老顺序会红 13700~29241 px）。
      三层验收：`tests/tile_expansion_test.cpp`（CPU 逐位 3.3e7 坐标）、
      `tests/geom_probe_test.cpp`（GPU 逐位 4.7e7 坐标）、`scripts/capture-gate.sh`（34 状态逐字节）
- [ ] 备选：形状表改成"每槽位直接存顶点"（fetch 模式）—— 只有"不想把 GL 上下文提到 4.1（`fma()` 需要
      GLSL 4.00+）"或"换编译器后 CPU/GPU 逐位对拍对不上"时才需要；架构（实例流/canonical 表/图标并入/
      剔除/测试）都不变，只换 `packShapeTable` 的内容与 VS 主体
- [ ] 备选：按 mode 分 4 次 draw（每次只带该模式的 part）—— 现在所有模式共用一份 175 槽的 canonical 表，
      直身（BIG）占多数时会白跑不活动的 part。**先量帧时间**再决定是不是值得（代价是绘制顺序要重新对拍）

### 功能
- [x] 无头出图（地图全景）：`--map out.png [--map-size WxH] [--map-bg HEX|transparent]`
      真无头（不碰 GL/窗口），整条路径等比铺满 —— 6.77M 层的 MYC 出 4096² 只要 6.3 s
- [ ] 无头视频（构想）：那个 fork `rech114/ADOCAO` 用离屏 FBO + ffmpeg 管道做过（Apache-2.0，可参考）
  - core 侧已有先例：headless 探针直接跑 PositionSolver/sampleTrail（见拖尾调研用的 trailprobe）；缺口在离屏 GL 上下文（headless GLFW / EGL）+ 渲染循环的时间源——无音频设备时不能依赖音频回调推进 m_readCursor，需固定步长驱动
- [ ] MoveCamera（5 种 relativeTo 模式）——Easing 唯一规划使用方（Camera 缓动）
- [ ] PositionTrack：relativeTo、rotation、scale、opacity、stickToFloors
- [ ] Bloom / Flash 特效

### 架构
- [ ] **加载期常驻内存**（"十亿层"的真正瓶颈，2026-10 实测）：MYC 走真无头 `adocao image`
      （只加载 + 解算、不建 mesh）峰值就是 **1560 MB**，而 mesh 只占 ~430 MB —— 下一仗在
      `Tile`/`tileBPMs`/`FastAction`/timeline 的窗口化与 checkpoint
- [ ] 帧率上限从编译期常量改为运行时配置（LauncherConfig 选项）
- [ ] 缩放范围可配置（min/max zoom）
- [ ] 剔除边距可配置

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

### 架构
- [x] GameWindow 重构为类（init/update/render 分离）
- [x] P1-P5 结构重构：P1+P2 目录搬迁+CMake 拆库；P3 PlaybackEngine → core/timeline；P4 app 拆分（CameraController/LevelScene + wizard 分页）；P5 资产并入 assets/ + g_sc 清理（见 docs/project-structure.md）
- [x] dirty check 跳过静止帧 GPU 上传（已回退：导致静止帧黑屏）
- [x] Spatial grid 加速大关卡剔除（已回退：queryGrid 从未接入 draw，复杂度和 O(n) 遍历无差距）

### 性能
- [x] Visibility cache fix
- [x] Frame profiler (per-section timing)
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
