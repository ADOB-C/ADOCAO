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
| 10 | `15bfd0e` | `AdocaoWriter` 段级 zstd 决策/组装共用缓存（此前同一 blob 压两遍；输出逐字节不变） |
| 12 | `c293716` | `Progress::enabled`、`Profile::Entry::hits`、`Scheduler` 恒假分支、`AudioEngine` 的 `stop/resume/setVolume/hasEnvelope/m_initialized/m_volume`（×1.0 删除，逐位不变） |
| 16 | `b470888` | `Ilybin` 只写输出、`Scheduler::tokCopy`、`PngBand` 死初值、`PngStream` 零调用访问器 |
| 15 | `fd11ae6` | `AdocaoDiagnostics` 的 `sentinelRuns/allIntegers`（无读者）与 `inRun` |
| 14 | `2691110` | `AdocaoBlocked` 的无用 `ColKind` 形参、恒 true 的 compress、`blocks .first` 只写 |
| 13 | `5925b45` | `AdocaoReader` 的 `Container::data/length`、`Sec::elems`、零调用 `Container::find()` |
| 11 | `79cfd2e` | 不再夹取谱面 zoom（Re_ADOJAS 也不夹，仅保留除零兜底）；`ADOCAO_CULL_BLOCK` 覆盖用户设置改为 `LOG_W` |
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
- ~~`LevelMap` 死字段 / `PngStream` 1-bit 死路~~：已删（`6a72419`）；`--thickness` 超界现在会 `LOG_W`。
- `Camera` 的 `z<1`/`z>1e5` 夹取已删（`79cfd2e`）；`--cull-block` 被 env 覆盖现在 `LOG_W`。
- `core/map` 的 `timeColorAt` clamp 仍保留（公共 inline + 输入契约；两个调用点当前不会触发，
  注释已说明）；`AdocaoWindow` 的 `(void)settings` 是 core hook 的契约形参。
- 扫描结论：`-Wunused*` 归零；"头文件 accessor 零调用"扫描为 0；"成员字段出现次数 ≤2"仅剩
  真正被 memcpy 使用的数组（`TileMesh::m_fill/m_stroke`）与测试里的字符串护栏。
- 仍可按模块做更深的语义审计（例如 `AdocaoColumns` 的编码器分支、`AdocaoWindow` 的窗口重算
  分支是否有不可达组合），但机械可查的死代码已经清完。
- `docs/` 里还有若干"实测 X 种粒度逐字节相同"一类历史结论，需要按细块后的实现重测再改。

## 完成证据（2026-10-18）

| 检查 | 结果 |
|---|---|
| `ctest --test-dir build` | **31/31** |
| 像素门槛 `scripts/capture-gate.sh check` | **54/54 逐字节相同** |
| `-Wall -Wextra -Wunused*`（排除第三方/链接重复） | **0 告警** |
| 头文件 accessor 零调用扫描 | **0**（脚本：`X() const { return ...; }` 且全仓零调用） |
| 成员字段出现次数 ≤2 扫描 | 仅剩 `TileMesh::m_fill/m_stroke`（memcpy 使用）与测试里的字符串护栏 |
| `adocao pack angles360` 与审计前字节比 | **完全相同** |
| 剔除粒度 4096/8192/16384 同帧 PNG | **逐字节相同** |
| Windows 构建（MinGW） | 通过 |
| 工作区 | clean |

### 审计边界（诚实声明）

- 已覆盖：`app/ core/level core/timeline core/camera core/util core/map render archive audio tests`
  的机械可查项（未调用 API、恒真/恒假分支、只写字段、死文件、重复实现、静默 clamp），
  并由四个模块级 subagent 逐文件通读 + 全仓调用点核对。
- 仍需按需继续：个别编码器/窗口重算的**语义分支**（"在真实输入下是否可达"）无法靠
  grep/告警判定，需要构造输入实测；这些列在"已知仍待处理"里，不假装已证完。

## 语义分支审计（2026-10-18 续）

机械死代码清完后，按"真实输入下是否可达"核了几处：

| 位置 | 核查方式 | 结论 |
|---|---|---|
| `AdocaoColumns` 的 7 个列级 codec | 已有 `tests/adocao_columns_test` 逐项断言取舍：Const / Dict / DeltaVarint / DeltaRleVarint / BitPack / Rle / Raw 都有输入能选中，且逐位往返 | 全部可达 |
| `Codec::JsonPassthrough`(5) | 全仓无 encoder/decoder 分支 | **删枚举与 codecName case**，id 5 保留为空洞并写进格式文档 |
| `SectionXz`(17) | writer 只写 SectionZstd；reader 有兼容分支 | 标注"仅读侧兼容"；`codecName` 里保留 `SectionZstd`/`SectionXz` 两个名字（列级走不到，但枚举是同一个，列全才让 `-Wswitch` 守住"新增列编码必须取名字"） |
| `CkFlags::kCkRadius` / `radiusScale` / `kCkSentinelRun` | checkpoint 重算不写半径，`sentinelRun` 由字段直接读、flag 位无读者 | **格式预留位**：写盘 96 B 记录不变，保留并在注释/文档里注明预留（不删除，否则破坏格式字节） |

结论：列级编码器的每个分支都有真实输入；剩下的"看起来没人用"的只有格式预留位，
它们是 `.adocao` 记录的一部分，删了会改文件字节 —— 因此按"保留 + 注明"处理，而不是删。
