# 代码审计：死代码 / 无理由护栏 / 静默限制用户输入

> 2026-10-18 起的分批审计。规矩只有三条：
> 1. **每一行代码都要有作用**（没人调用的 API、恒真/恒假分支、只写不读的状态，删）；
> 2. **每个护栏都要说得出真实失败场景**（说不出、或前置条件已经保证的，删或改 assert）；
> 3. **不许静默限制用户输入**（clamp/上限/截断要么有日志，要么有可陈述的硬约束）。
>
> 每批验收：`ctest` 31/31 + 像素门槛 54/54（行为变化会写明并重存基线）。
> 审计方法：`-Wall -Wextra -Wunused*` 告警 + 逐文件通读 + 全仓 `grep` 核对调用点。

## 批次与提交

| 批次 | 提交 | 内容 |
|---|---|---|
| 1 | `678a315` | `-Wall -Wextra` 告警归零：死函数/死字段/UB 占位（`b/0*0`）/switch 漏枚举/初始化顺序 |
| 2 | `7e4a790` | `PositionSolver` 恒真分支、重复 clamp、`LevelData` 死排序/死清空、`HitsoundManager::setVolume`、`CameraDirector::setHoldOffset`、`FloorWindow::why()`（含悬垂 `c_str()`）、`ByteSource::halfSize()`；修 `Ilytx` 成员名下溢与 `AdocaoCheckpoints` 先乘后查 |
| 3 | `83d936e` | `TileMesh` 计数/死访问器/死字段/错误移动构造、恒零图标 z、`TileShape` 死常量、`Camera::proj/view`、`Shader::id`、`Planet/PlanetTrail` 死 API 与 `rebuildGeometry`、删 `highlight.vert`/`render/Easing.hpp` |
| 4 | `0acef34` | `--no-auto-stroke` 真正生效、export 的 music 守卫删除（修复 export 100% 失败）、capture PNG `else` 绑错、死访问器/守卫、帮助文本、`--` 处理 |
| 5 | `b83e5f6` | Windows `AssetSetup` 的 exe 目录、`LoadingProgress::stage`、向导 `musicAutoTried` 死块、`Tile::angle`（无读者） |
| 6 | `bb5b1fe` | `Logger::setDebugConsole`、`Easing` 恒真判断、`Scheduler::configure/Config/m_mainParticipates`、`CameraTimeline::clear`、`CameraDirector::cursor`、`HitsoundManager::setHitsoundType/reset/isEnabled` |
| 7 | `97e735d` | `--width/--height` 校验、image 帮助去重、`m_firstStartTime` 注释与实现对齐、槽位/三角形/texel 注释修正 |
| 10 | 本轮 | `AdocaoWriter` 段级 zstd 决策/组装共用缓存（此前同一 blob 压两遍）、`LevelMap`/`PngStream` 死选项、清理后告警归零 |
| 8 | 本轮 | `PlatformPaths` 统一 `executableDirectory()`（Windows 版补 `GetModuleFileNameA`）、`Application` 两段逐行重复的导出/加载/进游戏合并为 `runLevelToGame()` |

## 修掉的真 bug（不是风格问题）

| 位置 | 症状 | 处理 |
|---|---|---|
| `assets/shaders/tile.vert` | 410 生产路径**没读 `aPivot`**：MoveTrack 旋转/缩放绕砖起点，CPU 路径绕砖心 | 两条路径统一；`mt_test` 抓帧 GPU vs CPU `diff 0`；门槛 8 个含 MoveTrack 状态重存 |
| `render/CullIndex.hpp` | 细块数写死 8：粒度 8192/16384 时每块后半**静默漏画**（20k 砖只覆盖 11808/7712） | `m_subPerBlock = m_floors/kSubFloors`；ctest 覆盖 512/4096/8192/16384 |
| `archive/Ilytx.cpp` | 成员名 3–12 字符时 `size_t` 下溢 → `compare` 抛 `out_of_range` | `leaf.size() > wl` 前置 |
| `archive/AdocaoCheckpoints.cpp` | `count*96` 先算后查（先溢出） | 先查 `count > 2^40` |
| `app/Application.cpp` | `export` 的 `musicPath.empty()` 守卫让 `adocao export` 与向导 Export 100% 失败 | 删除守卫（导出只用 hitsound 合成） |
| `app/main.cpp` / `LevelScene` | `--no-auto-stroke` 零读；`--fill` 从不推导描边 | `LevelScene::init` 按 `autoStroke` 推导（0.5×fill），默认值与 `6F5D3D` 一致 → 门槛不变 |
| `app/GameWindow.cpp` | `capture` 写 PNG 的 `else` 绑到 `profile::enabled()`，PROFILE 模式下失败不报错 | 加花括号，错误日志归位 |
| `app/AssetSetup.cpp` | 缺 `_WIN32` 分支 → Windows 资产根只剩 CWD | 统一到 `PlatformPaths.cpp`（含 `GetModuleFileNameA`） |
| `assets/shaders/tile_cpu.vert` | （更早）`layer` 未定义 → 老 GPU 上 shader 编译失败、整帧黑 | 改 `type`；CPU/GPU 抓帧逐字节相同 |

## 明确保留的护栏与理由

| 护栏 | 真实失败场景 |
|---|---|
| `fitsInt32` / `toInt32Checked` 边界检查 | 2³¹−1 层目标下，静默截断就是错误数据；收窄前必须显式判 |
| `maxPixels`（256 MPix）、PNG 帧大小上限 | 恶意/损坏谱造 16K 以上画布会 OOM |
| `kMaxCullCandidates` / `kMaxInstances` | 实测 1,055 万砖"整张一帧" 36.2 s、macOS 沙滩球；压到 2¹⁶ 门槛立刻 50/51 |
| `ADOCAO_CULL_LIMIT=0` / `ADOCAO_CULL=scan` | 排障与 A/B 回全扫的真实钩子 |
| `Camera::setAdoZoom` 的 `!(z>0)` 兜底 | 谱面 `zoom: 0` 会除零/投影 inf |
| 滚轮 zoom 的 `[MIN, 100000]` | 0/负数会导致投影退化 |
| `--capture` 钉死 trailAdaptive + 同帧重渲染自检 | 2026-10 真踩过：只快几毫秒就让 31/51 个像素状态变色 |
| `--capture-tile` 的 `strtoll` + int32 范围校验 | `atoi` 溢出是 UB，且项目要求 2³¹ 边界显式处理 |
| `tile_cpu.vert` 的 identity 快路 / `GEOM_PROBE` 分支 | 前者逐位保持旧路径，后者是几何逐位对拍入口 |
| 线性拖尾的 `maxPoints` 环容量 | 点数硬上限；现在截断会 `LOG_W`（不再静默） |

## 已知仍待处理

- ~~`AdocaoWriter` 的 plain 候选被 zstd-19 压两遍~~：已用 thread_local 决策缓存让同一 blob 只压一次；
  `angles360` 打包输出与改动前**逐字节相同**。
- `core/map/LevelMap` 死字段（`timeFrom/timeTo/drawBlue/markers`）与 `PngStream` 整条 1-bit 索引色死路；
  `--thickness` 的 `halfW<=64` 是唯一仍静默限制用户输入的点（要么报错要么写进文档）。
- `docs/` 里还有若干"实测 X 种粒度逐字节相同"一类历史结论，需要按细块后的实现重测再改。
