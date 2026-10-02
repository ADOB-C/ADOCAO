# ADOCAO 项目结构重构蓝图（定稿 v4）

> 状态：**定稿 v4**——§9 全部决策已拍板并回填正文。本版修订（决策 10）：Easing 与 Camera 同目录、
> TileGeometry CPU 版废弃改 GLSL、`core/geometry` 取消。
> 代码进度：**P1–P5 全部完成** —— P1+P2（目录搬迁 + CMake 拆库）、P3（PlaybackEngine → `core/timeline`）、
> P4（app 拆分：GameWindow → CameraController/LevelScene；LauncherWindow → `app/wizard/` 分页，见 §4.2/4.3）、
> P5（资产并入 `assets/` + `g_sc` 清理 + 文档/CI 同步，见 §7）。
> 目标读者：ADOCAO 维护者。
> 
> 背景需求（来自维护者）：
> 
> 1. 结构清晰、易懂；
> 2. 遵守 UNIX 哲学——每个文件不要太大，过大就要拆，一文件一职责；
> 3. 未来把「解析、角度计算、Timeline」等与画面/打拍音无关的部分抽成独立库，并可能对外导出（也可被 Vulkan 版 ADOCAV 等复用）；
> 4. 改动力度：**必须大改**（目录、CMake、构建脚本整体重排），但迁移过程分阶段、每阶段保持可编译可运行、零行为变化。

---

## 1. 现状盘点

### 1.1 顶层布局与职责

| 目录           | 内容                                                                       | 行数合计  | 与画面无关？          |
| ------------ | ------------------------------------------------------------------------ | ----- | --------------- |
| `app/`       | main、Application、LauncherWindow(向导)、LoadingWindow、LevelLoader、GameWindow | ~1900 | 否（含 ImGui/GLFW） |
| `audio/`     | AudioEngine(miniaudio 设备)、HitsoundManager(合成)、stb_vorbis                 | ~600  | 否（音频后端/打拍音）     |
| `camera/`    | Camera（纯数学，无 GL）                                                         | 89    | 部分              |
| `game/`      | PlaybackEngine(660)、Planet(**含 glad/GL**)                                | ~1000 | 否               |
| `glad/`      | OpenGL loader（生成代码）                                                      | —     | 否               |
| `hitsounds/` | 27 个 WAV 资产                                                              | —     | 资产              |
| `level/`     | LevelData(489)、JsonCleaner                                               | ~670  | **是**           |
| `render/`    | Shader、Shaders.hpp(回退 GLSL)、PlanetTrail、CullSIMD                         | ~500  | 否               |
| `shaders/`   | 运行时 GLSL 资产                                                              | —     | 资产              |
| `track/`     | TileGeometry(**纯几何**)、TileMesh(456, GL)                                  | ~740  | 部分              |
| `util/`      | Logger、ThreadPool、DataFile(miniz)、Easing                                 | ~500  | 是               |

CMake：单一根 `CMakeLists.txt`（352 行），一个 `add_executable(adocao)`，源文件按注释分组平铺。

### 1.2 发现的问题

1. **纯逻辑被 GL 传染（最严重）**：`game/PlaybackEngine.hpp` → `#include "Planet.hpp"` → `#include "glad/gl_core.hpp"`。
   即"计时/角度/Timeline"这一层只要被引用，就必须带上 OpenGL 头文件。
   未来导出核心库或给 ADOCAV 复用时，这条路直接堵死。
2. **没有库边界，目录分层只是"约定"**：单一 exe target 下，任何文件可以 include 任何文件，
   编译器不强制依赖方向；全项目共享一份编译单元列表，改动任何文件都牵动整条构建。
3. **目录与职责不对应**：
   - `game/Planet` 其实是 GL 绘制对象（VAO/VBO/EBO + draw），不是"游戏逻辑"；
   - `track/` 一半是纯几何生成（TileGeometry），一半是 GL 实例化网格（TileMesh）；
   - `camera/` 是纯数学但孤悬顶层，没人知道它属于哪一层。
4. **大文件多职责混杂**（违反 UNIX 哲学）：
   - `app/LauncherWindow.cpp` 772 行（6 页向导全在一文件）
   - `game/PlaybackEngine.cpp` 660 行（预计算 + 运行态 + 行星位置 + 轨迹采样混在一起）
   - `app/GameWindow.cpp` 607 行（输入、相机控制、音乐同步、渲染编排全在一文件）
   - `level/LevelData.cpp` 489 行、`track/TileMesh.cpp` 456 行
5. **头文件膨胀 / include 卫生差**：
   - `app/GameWindow.hpp` include `LauncherWindow.hpp` + `LevelLoader.hpp`；
     `app/LevelLoader.hpp` 又 include `LoadingWindow.hpp` + `LauncherWindow.hpp`——应改为前置声明，把重 include 挪进 .cpp。
   - `game/PlaybackEngine.hpp` include 了 `audio/HitsoundManager.hpp`（触发打拍音只需时间戳数据，不需要管理器类）。
6. **全局可变 scratch**：`track/TileGeometry.hpp` 里 `extern Scratch g_sc`（全局复用缓冲），
   目前单线程内安全，但属于"隐式共享状态"，多线程化（并行 mesh 构建）时是隐患。

   > ✅ **P5 已清理**：删除全局 `g_sc`，调用方（`render/TileMesh.cpp`）改用函数局部 `Scratch`，
   > 为将来并行 mesh 构建扫清障碍（TileGeometry 本身仍是决策 10 的过渡文件，几何 GLSL 化后整文件删除）。
7. **资产路径分散在 4 处**（若将来移动 `hitsounds/`、`shaders/` 必须同步）：
   - `app/GameWindow.cpp:167-170` — 4 个 `"shaders/*.vert/frag"` 相对 CWD 的路径串；
   - `audio/HitsoundManager.cpp:61-73` — `findAssetsDir()`：Windows 取 exe 旁 `hitsounds/`，否则 CWD 相对；
   - 根 `CMakeLists.txt:317-325` — POST_BUILD 拷贝两目录到 build 根；
   - `.github/workflows/release.yml:46` — `Compress-Archive -Path hitsounds,shaders`。

   > ✅ **P5 已解决（2026-09）**：资产并入 `assets/` 后四处同步为：
   > `app/LevelScene.cpp` 的 `compileShaders()`（"assets/shaders/…"）、`audio/HitsoundManager.cpp` 的
   > `findAssetsDir()`（"assets/hitsounds/…"）、`app/CMakeLists.txt` POST_BUILD（拷贝到
   > `build/assets/{hitsounds,shaders}`）、`.github/workflows/release.yml`（`zip -r ADOCAO-data.zip ../assets`）。
8. **文档漂移**：`AGENTS.md`/`README.md` 曾残留 GPU compute culling 等已移除功能的描述（2.0.0 起已不需要，现已彻底清理）与不全的 CLI 标志表；重构完成后仍需统一核对目录命名等描述。

### 1.3 现状依赖图（真实 include 关系，已抽掉标准库）

```
app ──> LauncherWindow/LoadingWindow/LevelLoader/GameWindow（互相缠绕）
 ├─ glad / imgui / glfw / tinyfiledialogs（平台/UI）
 ├─ audio/AudioEngine, HitsoundManager
 ├─ camera/Camera
 ├─ track/TileMesh ──> track/TileGeometry, render/CullSIMD, util/ThreadPool, level/LevelData
 ├─ game/Planet ──> render/PlanetTrail, render/Shader, camera/Camera
 ├─ game/PlaybackEngine ──> level/LevelData, game/Planet(→glad), audio/HitsoundManager
 ├─ render/Shader ──> util/DataFile
 └─ level/LevelData ──> level/JsonCleaner, util/Logger（最干净的一条纯逻辑链）
```

---

## 2. 目标结构（目录蓝图）

原则：**目录即库**。每个库一个 `CMakeLists.txt` 生成 STATIC 目标；依赖方向由 CMake `target_link_libraries` 强制执行；
`core/` 整棵子树即未来可导出的"引擎核心"。

```
ADOCAO/
├── CMakeLists.txt              # 瘦身：project/选项/FetchContent/add_subdirectory/资产拷贝（不再列源文件）
├── cmake/                      # （可选）自定义 CMake 函数/工具链片段
│
├── core/                       # ★ 纯逻辑库 adocao_core —— 无 GL / 无窗口 / 无音频设备
│   ├── CMakeLists.txt
│   ├── level/                  # ← level/：.adofai 解析与数据模型
│   │   ├── LevelData.hpp/.cpp
│   │   └── JsonCleaner.hpp/.cpp
│   ├── timeline/               # ★ 新目录 ← game/PlaybackEngine 拆出（解析之外的一切"时间"逻辑）
│   │   ├── Timeline.hpp/.cpp          # 预计算 + 查询（原 precalculateTiming、tileStartTimes、findTileIndex…）
│   │   ├── PositionSolver.hpp/.cpp    # 任意时刻行星位置/角度（纯 double 数学，原 computePositionsAtTime*）
│   │   └── PlaybackClock.hpp/.cpp     # 运行态：start/startAt/syncToAudio/stop/currentTile/elapsed
│   └── util/                   # ← util/：跨层基础设施（允许被任何库依赖）
│       ├── Logger.hpp/.cpp
│       ├── ThreadPool.hpp/.cpp
│       └── DataFile.hpp/.cpp   # .adofai 读取（zip/目录/回退）——核心库导出时随库走
│
├── render/                     # ★ GL 渲染库 adocao_render（依赖 core）
│   ├── CMakeLists.txt
│   ├── Camera.hpp/.cpp                 # ← camera/（纯数学，无 GL）
│   ├── Easing.hpp                      # ← util/Easing.hpp（唯一使用方：Camera 的缓动动画，见决策 10）
│   ├── TileGeometry.hpp/.cpp           # ← track/TileGeometry（**过渡**：几何 GLSL 化前 TileMesh 仍需它；之后删除）
│   ├── Shader.hpp/.cpp                 # 现状不变
│   ├── Shaders.hpp                     # 内嵌回退 GLSL，不变
│   ├── TileMesh.hpp/.cpp               # ← track/TileMesh（GL 实例化/剔除）
│   ├── Planet.hpp/.cpp                 # ← game/Planet（GL 绘制 + 轨迹；不再被 core 引用）
│   ├── PlanetTrail.hpp/.cpp            # 现状不变
│   └── CullSIMD.hpp                    # 现状不变
│
├── audio/                      # ★ 音频库 adocao_audio（依赖 core；miniaudio 仅本库私有）
│   ├── CMakeLists.txt
│   ├── AudioEngine.hpp/.cpp
│   ├── HitsoundManager.hpp/.cpp
│   └── stb_vorbis_impl.cpp
│
├── glad/                       # OpenGL loader，保留顶层，独立静态目标 adocao_glad（生成代码不入 render）
│   ├── gl_core.hpp / gl_core.cpp
│   └── CMakeLists.txt
│
├── app/                        # ★ 可执行 adocao（唯一 exe 目标；依赖所有库 + glfw/imgui/tinyfiledialogs）
│   ├── CMakeLists.txt
│   ├── main.cpp / Application.hpp/.cpp
│   ├── LauncherWindow.hpp/.cpp        # 向导壳：窗口/GL/ImGui 生命周期 + 分页调度（95 行，P4 收窄）
│   ├── LoadingWindow.hpp/.cpp
│   ├── LevelLoader.hpp/.cpp           # 解析+音频胶水；头文件已前置声明瘦身（P4）
│   ├── GameWindow.hpp/.cpp            # 主循环骨架：窗口/输入/音乐↔时钟同步/渲染编排（505 行，P4 收窄）
│   ├── CameraController.hpp/.cpp      # ★ P4：拖拽平移/滚轮缩放/点击/屏幕↔世界换算状态机
│   ├── LevelScene.hpp/.cpp            # ★ P4：shader/TileMesh/Planet/轨迹/高亮/图标 + 可见性动画编排
│   └── wizard/                        # ★ P4：LauncherWindow 向导分页（一页一文件）
│       ├── WizardState.hpp            # 页面枚举 + 跨页共享编辑状态 + 选项表
│       ├── WizardChrome.hpp/.cpp      # 向导窗口/ImGui 引导 + 配置页公共 chrome（进度条/Next/Start）
│       ├── FileDialogs.hpp/.cpp       # 文件/文件夹对话框（Win32 COM / tinyfd）+ 音乐自动检测
│       ├── PageWelcome.cpp            # Welcome 页 + 预加载线程启动 + Export
│       ├── PagePreload.cpp            # Preload 过渡页（后台 parse+timeline 进度）
│       ├── PageHitsounds.cpp / PageGraphics.cpp / PageVisuals.cpp
│       └── PageMusic.cpp              # Music 页 + Start（提交全部配置）
│
├── assets/                     # ★ 运行时资产（P5 已执行：shaders/、hitsounds/ 并入）
│   ├── shaders/                # ← shaders/（.vert/.frag，运行时相对 CWD 加载）
│   └── hitsounds/              # ← hitsounds/（27 个 WAV）
├── docs/                       # 架构/规划文档（本文件）
├── scripts/                    # 开发流水线：push-ci.sh（推送+跟踪 CI）run.sh（运行/调试）release.sh（发版）
│                               #   make-app.sh（macOS 自包含 .app 打包：二进制 + assets + ad-hoc 签名）
├── build.sh  build.bat  build.ps1     # 不动（仍 cmake -B build）
├── .github/  README.md  TODO.md  AGENTS.md  LICENSE
```

### 2.1 依赖方向（强制规则）

```
        ┌────────────────────────────────────────────┐
        │  app（exe）  glfw / imgui / tinyfiledialogs │  平台+UI 只允许出现在 app
        └───────┬───────────┬─────────────┬──────────┘
                │           │             │
        ┌───────▼───┐ ┌─────▼─────┐ ┌─────▼─────┐
        │ adocao_   │ │ adocao_   │ │ adocao_   │
        │ render    │ │ audio     │ │ glad      │
        │ (GL)      │ │(miniaudio)│ │(loader)   │
        └───────┬───┘ └─────┬─────┘ └───────────┘
                │           │
        ┌───────▼───────────▼─────┐
        │ adocao_core             │  ← 唯一的纯逻辑层，禁止 include：
        │ level / timeline /      │    glad、GLFW、imgui、miniaudio、
        │ util                    │    任何平台头；只许 std/glm/rapidjson/miniz
        └─────────────────────────┘
```

- `render`、`audio` 只能依赖 `core`（以及自己的后端库 glad / miniaudio，后者设为 PRIVATE include）。
- `core` 不得反向依赖 `render`/`audio`/`app`。
- `app` 是唯一允许碰 GLFW/ImGui/窗口的地方。
- 编译期用 CMake `target_link_libraries` + `target_include_directories` 落实：缺链即编译失败，而非口头约定。

---

## 3. 文件映射表（现状 → 目标，全量）

| 现状                          | 目标                                                                      | 动作                            |
| --------------------------- | ----------------------------------------------------------------------- | ----------------------------- |
| `app/main.cpp`              | `app/main.cpp`                                                          | 保留（CLI 解析属入口职责）               |
| `app/Application.*`         | `app/Application.*`                                                     | 保留                            |
| `app/GameWindow.*`          | `app/GameWindow.*`（拆出 `CameraController.*`、`LevelScene.*`，P4 已执行）               | 保留 + 内部分拆                  |
| `app/LauncherWindow.*`      | `app/LauncherWindow.*`（页面拆入 `app/wizard/`，P4 已执行）                              | 保留 + 向导分页拆分                  |
| `app/LoadingWindow.*`       | `app/LoadingWindow.*`                                                   | 保留                            |
| `app/LevelLoader.*`         | `app/LevelLoader.*`                                                     | 保留（胶水层，依赖 core+audio+render）  |
| `glad/gl_core.*`            | `glad/gl_core.*`                                                        | 位置不变，独立 STATIC 目标             |
| `level/LevelData.*`         | `core/level/LevelData.*`                                                | git mv + include 加 `core/` 前缀 |
| `level/JsonCleaner.*`       | `core/level/JsonCleaner.*`                                              | 同上                            |
| `game/PlaybackEngine.*`     | **拆** → `core/timeline/Timeline.*`、`PositionSolver.*`、`PlaybackClock.*` | 行为保持，见 §4                     |
| `game/Planet.*`             | `render/Planet.*`                                                       | git mv（GL 对象归 render）         |
| `camera/Camera.*`           | `render/Camera.*`                                                       | git mv                        |
| `track/TileGeometry.*`      | `render/TileGeometry.*`（过渡）                                             | git mv；几何 GLSL 化后删除（见决策 10）   |
| `track/TileMesh.*`          | `render/TileMesh.*`                                                     | git mv（GL 网格入 render）         |
| `render/Shader.*`           | `render/Shader.*`                                                       | 保留                            |
| `render/Shaders.hpp`        | `render/Shaders.hpp`                                                    | 保留                            |
| `render/PlanetTrail.*`      | `render/PlanetTrail.*`                                                  | 保留                            |
| `render/CullSIMD.hpp`       | `render/CullSIMD.hpp`                                                   | 保留                            |
| `audio/AudioEngine.*`       | `audio/AudioEngine.*`                                                   | 保留                            |
| `audio/HitsoundManager.*`   | `audio/HitsoundManager.*`                                               | 保留（合成/混音留在 audio，不进 core）     |
| `audio/stb_vorbis_impl.cpp` | `audio/stb_vorbis_impl.cpp`                                             | 保留                            |
| `util/Logger.*`             | `core/util/Logger.*`                                                    | git mv                        |
| `util/ThreadPool.*`         | `core/util/ThreadPool.*`                                                | git mv                        |
| `util/DataFile.*`           | `core/util/DataFile.*`                                                  | git mv（含 miniz 依赖，随库走）        |
| `util/Easing.hpp`           | `render/Easing.hpp`（与 `Camera.*` 同目录）                                   | git mv（Camera 是唯一使用方）         |
| `hitsounds/` `shaders/`     | `assets/hitsounds/`、`assets/shaders/`                                   | ✅ P5 已执行并入（四处路径已同步，见 §1.2-7）       |

include 约定：以仓库根为 include 根（与现状一致），新路径为
`#include "core/level/LevelData.hpp"`、`#include "render/TileMesh.hpp"`、`#include "core/timeline/Timeline.hpp"`。

---

## 4. 拆分方案（大文件 / 一文件一职责）

目标粒度：单文件 **≤ ~250-300 行**；一个类一个文件；一个文件只做一件事。

### 4.1 `game/PlaybackEngine.cpp`（660 行）→ `core/timeline/` 三个文件

| 新文件                       | 职责（对应现状内容）                                                                                                                                                                                                                     |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Timeline.hpp/.cpp`       | **数据 + 预计算**：`precalculateTiming()` 四阶段、`tileStartTimes/tileDurations/tileBPM/tileIsCW/tileAppear/tileDisappear`、`totalDuration()`、`findTileIndex()`、`getHitsoundTimestamps/Groups`。构建入参只依赖 `LevelData`。                       |
| `PositionSolver.hpp/.cpp` | **纯函数**：`computePositionsAtTime(t, red&, blue&)` / 按 tile 版 / `computePlanetTrails` 的采样逻辑。输入 `(Timeline, t)`，输出 `dvec2`/轨迹点数组——**不再写进 Planet 对象**。                                                                             |
| `PlaybackClock.hpp/.cpp`  | **运行态状态机**：start/startAt/stop/update/updateWallClock/syncToAudio、elapsedTime、currentTileIndex、preRoll、audioStartOffset、force-hitsound 配置。持有 `Timeline*`，每帧产出一个纯数据帧（时间、tile、红/蓝位置 dvec2），由调用方（GameWindow）灌给 render 层 Planet/轨迹。 |

关键解耦点：删掉 `PlaybackEngine.hpp` 对 `Planet.hpp` 与 `HitsoundManager.hpp` 的 include——
timeline 层只输出 `double`/`dvec2` 与时间戳结构，行星的 GL 外观与打拍音的播放全部由上层消费。

### 4.2 `app/GameWindow.cpp`（607 行 → P4 已执行）

| 新文件                         | 职责                                                         |
| --------------------------- | ---------------------------------------------------------- |
| `GameWindow.cpp`（收窄，505 行） | 主循环骨架：窗口/GL 上下文、init/run/update/render 编排、音乐与 PlaybackClock 同步 |
| `CameraController.hpp/.cpp`（~100 行） | 拖拽平移、滚轮缩放、点击检测、屏幕↔世界换算（原 `GameWindow::Input` 与 handleInput 中相机部分；纯状态机，可单测） |
| `LevelScene.hpp/.cpp`（~316 行）   | TileMesh/Planet/轨迹/高亮/图标的创建与逐帧绘制 + 轨道出现/消失动画状态，GameWindow 只调用它 |

（行数为当前 master 实测：GameWindow 505 / CameraController 101 / LevelScene 316。P4 落地时的
初值是 464 / ~100 / ~290；GameWindow 之后的增长来自书签与键盘导航、越歌曲结尾的音频同步、
以及为自适应拖尾速率做的帧计时，不是拆分失效。§1.1/§1.2 与 §4.4 的行数是重构**前**的盘点
快照，保留原值未随代码更新。）

GameWindow 内剩下的成员职责：LevelScene（GL 对象，异步 build 轮询）、CameraController、
窗口/全屏切换、键盘播放控制（Space/书签/方向键）、点击选中 tile（命中测试由 GameWindow 做，
坐标换算由 CameraController 提供）。

### 4.3 `app/LauncherWindow.cpp`（772 行 → P4 已执行）

按向导实际页面拆成每页一个文件（页面枚举 + 跨页状态集中在 WizardState）：

```
app/wizard/
├── WizardState.hpp          # 页面枚举（Welcome/Preload/Hitsounds/Graphics/Visuals/Music）+ State + 选项表
├── WizardChrome.hpp/.cpp    # 窗口/GL/ImGui 引导（原 LauncherWindow init）+ 配置页公共 chrome
├── FileDialogs.hpp/.cpp     # openFileDialog/selectFolderDialog（Win32 COM/tinyfd）+ detectMusicFile
├── PageWelcome.cpp          # Welcome → (Preload 后台线程) → Hitsounds；Export 出口
├── PagePreload.cpp          # Preload 过渡页（进度条，线程完成后由调度循环推进）
├── PageHitsounds.cpp / PageGraphics.cpp / PageVisuals.cpp
└── PageMusic.cpp            # Music 页 + Start 按钮（提交全部配置 → done）
```

（LauncherWindow.cpp 保留窗口生命周期与分页调度，~95 行。与蓝图差异：真实向导没有独立
「Start 页」——Start 是 Music 页底部的提交按钮；「Preload」是 Welcome 与 Hitsounds 之间
的过渡页，单独成文件。原 §1.2-4 的大文件混杂问题已消除。）

### 4.4 其余大文件（可选 / 后续）

| 文件                          | 行数  | 建议                                                                                                   |
| --------------------------- | --- | ---------------------------------------------------------------------------------------------------- |
| `level/LevelData.cpp`       | 489 | 暂不拆。若继续膨胀，按 `LevelParser`(DOM→模型) / `LevelActions`(processActions+逐 tile 事件) / `LevelData`(容器+API) 拆 |
| `track/TileMesh.cpp`        | 456 | 暂不拆（类内职责已分区）。可选按 build/draw/cull 拆多个 .cpp 共享类                                                        |
| `audio/HitsoundManager.cpp` | 291 | 暂不拆。可选把"纯合成（16-bit 混音）"与"资产加载/游标管理"分开                                                                |

---

## 5. 未来库边界与导出（本次只定界，不做）

### 5.1 core 允许 / 禁止

- 允许：C++20 std、`glm`（仅数学）、`rapidjson`、`miniz`（DataFile 读 zip）、自己目录内头文件。
- 禁止：`glad`、`GLFW`、`imgui`、`miniaudio`、`tinyfiledialogs`、任何平台 API。
  现状中唯一违反点就是 `PlaybackEngine → Planet → glad`，阶段 3 必须拔除。

### 5.2 导出后的用法示例（愿景，非本次实现）

```cpp
// 任何想复用 ADOCAO 核心的程序（CLI 工具 / ADOCAV / 测试）：
#include <adofai/level/LevelData.hpp>     // 或 core/level/LevelData.hpp
#include <adofai/timeline/Timeline.hpp>
#include <adofai/timeline/PositionSolver.hpp>

adofai::Level level;
level.loadFromFile("level.adofai");          // 解析（与画面无关）
adofai::Timeline tl;
tl.build(level);                             // 角度/BPM/出现消失时间（与画面无关）
auto groups = tl.hitsoundTimestamps();       // 交给任意打拍音合成器
double rx, ry, bx, by;
adofai::PositionSolver::positionAt(tl, 12.34, rx, ry, bx, by);  // 任意时刻行星位置
```

- **不导出**：`render/` 全部（Planet/TileMesh/Shader/Camera 的 GL 部分）、`audio/` 的引擎与合成、`app/` 全部。

- 打拍音不在 core（维护者明确"和打拍音无关"），但时间戳是纯数据，天然在 core——合成器想复用时只需链接 `adocao_audio`。

- 未来新功能落点（对照 TODO.md，保证结构可扩展）：
  
  | 功能                                          | 落点                                                                                                                                    |
  | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
  | PositionTrack / AnimateTrack 时间线            | `core/timeline` 扩展 + `render` 消费                                                                                                      |
  | MoveTrack（**遥远的未来**，仅记录）                    | 届时 `core/timeline` 扩展 + `render` 消费；现不排期                                                                                              |
  | ColorTrack / RecolorTrack（**只做静态**，见决策 8/9） | `core/level` 只解析事件数据；事件→每-tile 颜色由 render/TrackColor 承担（默认 CPU 静态；几十万事件走 GPU/GLSL 求值）                                                 |
  | 中旋渲染（curvaturePoints）                       | render：GLSL 程序化形状 + TileMesh 实例化（CPU TileGeometry 已废弃）                                                                                |
  | 轨道几何生成（替代 CPU TileGeometry）                 | render/GLSL：每实例带角度属性，VS 程序化生成弧/圆/五边形（无 CPU 顶点汤，见决策 10）；事件图标并入 tile 实例属性/本地子几何，随 tile 变换（AnimateTrack/PositionTrack 自动带上，省独立 icon 实例集） |

---

## 6. CMake 组织（骨架）

顶层 `CMakeLists.txt` 职责收窄为：project / 选项（PORTABLE、ZOOM_LEVEL、Release 优化）/ FetchContent 依赖 / `add_subdirectory` / 资产拷贝与 install。每个库目录自带：

```cmake
# core/CMakeLists.txt（示意）
add_library(adocao_core STATIC
    level/LevelData.cpp level/JsonCleaner.cpp
    timeline/Timeline.cpp timeline/PositionSolver.cpp timeline/PlaybackClock.cpp
    util/Logger.cpp util/ThreadPool.cpp util/DataFile.cpp)
target_include_directories(adocao_core PUBLIC ${CMAKE_SOURCE_DIR})  # include 根 = 仓库根
target_link_libraries(adocao_core PUBLIC glm::glm)
# rapidjson / miniz include 目录以 PUBLIC 形式透出
```

```cmake
# render/CMakeLists.txt（示意）
add_library(adocao_render STATIC
    Camera.cpp Shader.cpp TileGeometry.cpp TileMesh.cpp Planet.cpp PlanetTrail.cpp)
# TileGeometry.cpp 为过渡（几何 GLSL 化后删除）；Easing.hpp 仅头文件不参与编译，随 Camera 使用
target_include_directories(adocao_render PUBLIC ${CMAKE_SOURCE_DIR} PRIVATE ${glad_SOURCE_DIR})
target_link_libraries(adocao_render PUBLIC adocao_core PRIVATE adocao_glad OpenGL::GL)
```

```cmake
# app/CMakeLists.txt（示意）
add_library(adocao_imgui STATIC ${IMGUI_SOURCES})          # imgui 核心 + GLFW/GL3 后端
add_executable(adocao main.cpp Application.cpp ... )
target_link_libraries(adocao PRIVATE adocao_core adocao_render adocao_audio adocao_glad
                                   adocao_imgui glfw tinyfiledialogs)
```

要点：

- FetchContent 集中在顶层（保持现有版本/GIT_TAG/选项不动，CI 缓存 `build/_deps` 不受影响）。
- `glad/` 作为 `adocao_glad` STATIC：GL 类型只经 render/app 各自链接透出，core 永远接触不到。
- 顶层 POST_BUILD 资产拷贝、MinGW DLL 拷贝、install 原样保留。

---

## 7. 迁移阶段（每阶段必须能编译、CI 绿、行为不变）

| 阶段              | 内容                                                                                                               | 验收                                                          |
| --------------- | ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| **P0（本次）**      | 基线确认 + 本蓝图文档入库                                                                                                   | `docs/project-structure.md` 评审通过                            |
| **P1 目录搬迁**     | 按 §3 映射逐层 `git mv`；批量修正 include 前缀与根 CMakeLists 的 SOURCES 路径；**先不拆文件、不拆库**                                       | `build.sh`/CI 三平台绿；CLI 与向导冒烟                                |
| **P2 CMake 拆库** | core/render/audio/glad/app 各自 STATIC 目标 + `target_link_libraries`；根 CMakeLists 瘦身                                | 同上；故意去掉一条链接应编译失败（验证边界生效）                                    |
| **P3 core 化**   | `PlaybackEngine` 按 §4.1 拆为 `core/timeline/*`；Planet 移 `render/`；GameWindow 改为消费纯数据帧；**消灭 core→GL 的一切引用**         | `grep -r "glad\|GLFW\|imgui" core/` 为空；播放/跳转/书签/轨迹/导出行为逐项一致 |
| **P4 app 拆分**   | GameWindow 拆 CameraController 等（§4.2）；LauncherWindow 向导分页（§4.3）；头文件 include 卫生（前置声明，重 include 移 .cpp）            | ✅ 已完成（commit `d9e447c`）；编译 + CLI/向导冒烟通过，逐页可用                 |
| **P5 资产与收尾**    | 资产并入 `assets/`（已决策；§1.2-7 四处同步改）；`g_sc` 清理项评估；TODO.md/AGENTS.md/README 与结构同步；可加一条 CI job 或脚本检查"core 无违规 include" | ✅ 已完成；资产四处已同步；`g_sc` 全局 scratch 已改局部实例（见 §4.4 备注）；`scripts/check-core-purity.sh` 已加并在 build.yml linux job 运行；文档与代码一致 |

提交节奏（决策 5）：**P1 + P2 合并为一个 commit**（同为机械重构，互相验证），P3、P4、P5 各自独立 commit，便于 bisect / 回滚。

---

## 8. 不变量与风险清单

1. **精度规则不可破坏**：时间/位置全 `double`，GPU 仅最后一步转 float（CLAUDE.md 规则）。P3 拆 PositionSolver 时逐行对照，不得顺手改精度。
2. **CLI 参数全集不变**（README 中列出的所有 flag），`--export`、`--auto-play`、`--legacy-culling` 等路径行为不变。
3. **资产路径**：已决策并入 `assets/`。P5 之前 `shaders/`、`hitsounds/` 保持相对 CWD/exe 的现状；P5 移动时必须同步 §1.2-7 的 4 处（GameWindow shader 路径串、`findAssetsDir`、CMake POST_BUILD、release.yml）。
4. **回退字符串**：`render/Shaders.hpp` 内嵌 GLSL 是运行时找不到 `shaders/` 文件时的兜底，搬迁不得删。
5. **构建脚本/CI**：`build.sh/.bat/.ps1`、`.github/workflows/*.yml` 的 `cmake -B build` 用法不变；`build/_deps` 缓存路径不变。
6. **P3 是行为风险最高的阶段**（PlaybackEngine 拆分）——用 `--export` 导出的 hitsound WAV、逐帧位置日志等方式做差分对照后再合入。

---

## 9. 决策记录与待定项

| #   | 决策                                                        | 结论                                                                                                                               |
| --- | --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 1   | include 风格                                                | ✅ 仓库根 + `core/` 前缀（推荐方案）                                                                                                         |
| 2   | 核心目录命名                                                    | ✅ `core/`                                                                                                                        |
| 3   | 资产目录                                                      | ✅ 并入 `assets/`（`assets/shaders/`、`assets/hitsounds/`），P5 执行                                                                      |
| 4   | glad 位置                                                   | ✅ 保留顶层，独立 STATIC 目标                                                                                                              |
| 5   | 拆分节奏（P1/P2 与 P3/P4 是否分开）                                  | ✅ P1+P2 合并一 commit，P3、P4、P5 各一（理由见 §9.1）                                                                                         |
| 6   | GameWindow / LauncherWindow 拆分粒度                          | ✅ 按 §4：GameWindow 拆 `CameraController` + `LevelScene`；LauncherWindow 按向导 6 页拆文件                                                  |
| 7   | 文档语言                                                      | ✅ 中文为主，无需英文版                                                                                                                     |
| 8   | ColorTrack 动态动画（pulse / animDuration / RecolorTrack 渐变扫动） | ✅ 不做动态：后续自定策略转为静态每-tile 颜色（策略 TBD，收在 render 内，core 无感）                                                                           |
| 9   | 颜色逻辑归属与大批量事件处理                                            | ✅ 解析归 core（只产出事件数据）；事件→颜色归 render；几十万 RecolorTrack 事件 + 大关卡用 GPU/GLSL 求值（compute bake 或可见实例 VS 求值），仍不做动态动画                       |
| 10  | 几何生成与 Easing 归属                                           | ✅ TileGeometry CPU 版废弃 → 几何改 GLSL（render，实例角度属性 VS 程序化）；`core/geometry` 取消；Easing 与 Camera 同目录（`render/Easing.hpp`，Camera 唯一使用方） |

### 9.1 决策 5 详细说明：为什么 P1/P2 先走、P3/P4 单独提交

（这是"详细讲一下"的展开，看完再拍板。）

**P1/P2 是机械重构，P3/P4 是行为重构，风险来源完全不同，应分开：**

- **P1（git mv + include 前缀 + CMake SOURCES 路径）**：不改变任何类、函数、数据布局。唯一可能出错的是"路径写错 / include 漏改"，而这类错误**编译器与 CI 立刻能抓**（缺文件、未声明符号）——提交后几分钟即可验证，回滚 = revert 一个 commit，代价趋近于零。
- **P2（拆库目标 + target_link_libraries）**：同样机械。但它的价值是**给 P3 装上护栏**：库边界一旦由 CMake 强制，P3 里 core 只要出现 glad/GLFW/imgui 的 include，编译直接失败，不必靠人肉 review 把关。
- **P3（PlaybackEngine → core/timeline 拆分）**：改的是**类的划分和接口**（Planet 不再被 core 持有、GameWindow 改为消费纯数据帧）。若混进 P1/P2，一个 commit 里同时有"几百处路径改动"和"逻辑重写"——出问题时（如行为漂移）无法判断是搬错还是拆错，排查成本翻倍。
- **P4（GameWindow / LauncherWindow 拆分）**：只动 app/，风险低但 UI 密集、人工冒烟点多，值得单独一个 commit 单独评审。

**额外理由：**

- git 历史质量：搬迁 commit 里文件会被识别为 rename，之后的逻辑改动拥有干净的 blame 起点；先改逻辑再搬，历史就乱了。
- 可并行性：P1/P2 合入后，任何新功能分支（如 TODO 的 ColorTrack / PositionTrack）都基于新结构开发。拆分拖得越久，新代码越会继续长在旧 PlaybackEngine 上，届时再拆更痛。
- 折中选项：P1 与 P2 可**合并为一个 commit**（同属机械重构且互相验证：先立库边界再搬文件），但 P3、P4 各自独立是不变的原则。

**结论（已拍板）**：P1 + P2 合并为一个 commit（目录搬迁 + CMake 拆库同时落地，互相验证），P3、P4、P5 各一 commit；每步 CI 绿 + 冒烟。

### 9.2 决策 6 说明：GameWindow / LauncherWindow 拆分粒度（建议按 §4）

- **GameWindow（607 行）**：拆出 `CameraController`（输入→相机，纯状态机、可单测）与 `LevelScene`（渲染对象编排），主循环骨架收窄到 ~250 行内。不建议更细（逐方法拆会撕裂本来就内聚的帧循环），也不建议不拆（当前一文件里输入/同步/渲染三层混合）。
- **LauncherWindow（772 行）**：按 5.0.0 向导的 6 页各拆一个文件（§4.3）。想更保守可先在单文件内按页分成 `drawXxxPage()` 静态函数（每段 <150 行），仍超阈值再物理拆文件——两者都符合"≤250–300 行"原则，只是节奏不同。
- **结论（已拍板）**：按 §4 建议执行——GameWindow 拆 `CameraController` + `LevelScene`，LauncherWindow 按向导 6 页拆文件。
