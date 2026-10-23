# 环境变量（`ADOCAO_*`）总表

> 为什么单独一篇：这些钩子从 2026-10 一路长出来，散在 12 个文件里，没人能一眼说清"到底有哪些、
> 哪个是给用户的、哪个只是验收用的"。2026-10-18 清点：**40 个**（39 个 `ADOCAO_*` + 1 个历史拼写的 `ADOFAO_GATE_HOME`），布尔判断还有四种写法
> （`getenv() != nullptr`、`atoi != 0`、`atoi == 1`、首字符），同一句 `=0` 在不同变量上含义相反。
>
> 现在的规则：
> * **布尔**一律走 `adofai::env::flag()`（`core/util/Env.hpp`）：未设置 / 空 / `0` / `false` /
>   `no` / `off` = **关**，其它任何值 = **开**。
> * 数字、路径、枚举（`ADOCAO_CULL=scan`）仍按值解析（`atoi`/`atof`/`atoll`）。
> * **新增环境变量必须登记到本页**；`scripts/check-env-docs.sh` 会拿源码里的 `getenv("ADOCAO_…")`
>   逐条比对，漏了 CI 红。
> * "稳定性"一列：**用户** = 可以依赖（文档承诺）；**诊断** = 不承诺兼容，随时可能改名/删除；
>   **测试** = 只给 ctest / 验收脚本用。

## 1. 播放与性能（用户）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_VSYNC` | 布尔 | 关 | swap interval = 1；帧率上限变成显示器刷新率。录屏防撕裂时开（`docs/cli.md`） |
| `ADOCAO_UNCAPPED` | 布尔 | 关 | 去掉 320 fps 帧控，量机器真实上限（`docs/profiling.md`） |
| `ADOCAO_GL_VERSION` | `M.m` | `4.1` | 强制 GL 版本；`3.3` 会走 CPU 几何回退（验收降级链） |
| `ADOCAO_CPU_GEOMETRY` | 布尔 | 关 | 强制 CPU 几何回退（救急/验收） |
| `ADOCAO_XFORM_DEBUG` | 整数（砖号） | `-1` | 逐帧打印该砖的 MoveTrack/动画变换（move/rot/scale/alpha），用来对拍位置 |
| `ADOCAO_TRACKVIS_DUMP` | 布尔 | `0` | 逐层打印 AnimateTrack 的 appear/disappear 类型与时间窗（前 24 条），用来和 Re_ADOJAS 的 `buildAnimateTrackKeyframes` 逐条核对 |
| `ADOCAO_TILE_STYLE_SCALE` | 布尔 | `0` | 诊断开关：`1` = 恢复按 trackStyle 放大砖（Neon/NeonLight/Basic +0.025 宽/+0.02 长、Minimal −0.03）。它会**逐砖累加**、把轨道整体拉长（与录制版 t=100 s 差 26,801 像素），默认关 |
| `ADOCAO_DRAW_ORDER` | `ascending`/`asc` | 倒序 | 砖的提交顺序。**默认倒序** = Re_ADOJAS 的 `renderOrder=-tileIndex`（楼层大的先画、早的压在上面 → Neon 接缝描边连续，修「被狗啃」）；`ascending` 回到旧升序做 A/B |
| `ADOCAO_FLOOR_WINDOW` | `0`/`1` | **开** | `=0` 关闭按块物化（保留密集 `tiles`，A/B 用） |
| `ADOCAO_CULL_BLOCK` | 整数 | 设置里的粒度 | 覆盖剔除粗块粒度（层/块）；`0` = 关索引走全扫 |
| `ADOCAO_CULL` | `scan` | 空 | `=scan` 强制全扫（A/B） |
| `ADOCAO_CULL_LIMIT` | 整数 | 不设（预算开） | 只有 `=0` 关掉两道预算闸（"整张一帧"可能卡死，仅诊断） |
| `ADOCAO_LOD` | 整数 | 不设（LOD 开） | 只有 `=0` 强制关掉 LOD 抽稀 |
| `ADOCAO_CAPTURE_FRAMES` | 整数 | `1` | `--capture` 连渲 N 帧取稳态中位数（1..200） |

## 2. Profiler（用户）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_PROFILE` | 布尔 | 关 | 开分段计时（`core/util/Profile.hpp`） |
| `ADOCAO_PROFILE_FILE` | 路径 | `<ADOCAO.log 所在目录>/ADOCAO.prof` | `.prof`（CSV）路径；相对 = CWD |
| `ADOCAO_PROFILE_INTERVAL_MS` | 整数（ms） | `10000` | 每隔 N ms 落一行 `interval`（<100 忽略） |
| `ADOCAO_DEBUG_NO_FLASH` | 布尔 | 关 | 跳过 Flash 的 BG/FG 两趟，便于看清轨道（抓帧/调试用） |
| `ADOCAO_EXIT_AFTER_MS` | 浮点（ms） | `0` = 不自动退 | 跑满 N ms 正常退出，让 `end`/`segment` 汇总落盘 |
| `ADOCAO_LOG_FILE` | 路径 | exe 旁 → 系统日志目录 | 改 `ADOCAO.log` 路径（`.prof` 默认跟着走）；相对 = CWD，父目录自动建 |

## 3. 加载 / 解析（诊断·测试）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_PARSE_PIECES` | 整数 | `0` = 自动 | 强制把文件切成 N 段并行解析 |
| `ADOCAO_PARSE_DBG` | 布尔 | 关 | 分段计时的明细打到 stderr |
| `ADOCAO_WINDOW_KB` | 整数（KB） | `4096` | 流式窗口的半窗大小（≥4 KB 才生效） |
| `ADOCAO_WINDOW_REQUIRE` | 布尔 | 关 | 要求必须走通窗口路径，禁止静默回退（测试用） |
| `ADOCAO_WINDOW_DBG` | 布尔 | 关 | 窗口流水线/`.adocao` 窗口重算的调试输出 |
| `ADOCAO_WHOLE_DECOMPRESS` | 布尔 | 关 | 强制退回整份解压（每次读，测试可在同进程切换） |
| `ADOCAO_FORCE_DOM_PARSE` | 布尔 | 关 | 强制走旧 DOM 路径（与快路径对拍） |
| `ADOCAO_FAST_REQUIRE` | 布尔 | 关 | 快路径不许回退，否则直接失败（负向对照用） |
| `ADOCAO_ILYTX_DUMP` | 路径 | 空 | 把 `.ilytx` 翻译出的明文落盘（对照 RapidJSON 报错偏移） |
| `ADOCAO_FORCE_BLOCK_KB` | 整数（KB） | `0` = 不改 | 把 `.adocao` 分块阈值改成 N KB（在真实规模上造分块布局） |

## 4. 并行（诊断）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_JOBS` | 布尔 | 关 | 所有并行就地串行（A/B 对照；见 `docs/parallelism.md`） |
| `ADOCAO_SCHED_WORKERS` | 整数 | 硬件−1（≤8） | 固定 worker 数（`0` = 只有调用线程） |
| `ADOCAO_MIX_THREADS` | 整数 | 硬件数 | hitsound 混音线程数（`1` 让分块退化成粗粒度，供对拍） |

## 5. 诊断输出（诊断）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_CAM_DEBUG` | 布尔 | 关 | 相机 tween/rig 的状态日志（`docs/camera.md`） |
| `ADOCAO_COLOR_DEBUG` | 布尔 | 关 | 砖色/实例缓冲的调试日志 |
| `ADOCAO_ANIM_DEBUG` | 布尔 | 关 | MoveTrack/颜色动画的每帧日志 |
| `ADOCAO_TRAIL_DUMP` | 布尔 | 关 | 拖尾点哈希（A/B "按块物化 provider"） |
| `ADOCAO_TRAIL_INPUTS` | 布尔 | 关 | 前 3 个拖尾采样的输入（时间/位置/角度） |
| `ADOCAO_MESH_DUMP` | 布尔 | 关 | 网格派生数据的 FNV（整段比对） |
| `ADOCAO_CULL_DUMP` | 路径 | 空 | 把剔除结果（砖号列表）写到文件 |

## 6. 无头导出（用户）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_STITCH_Y0` | 整数 | `0` | `stitch` 从第几行开始（`--scale 1` 时有效） |
| `ADOCAO_STITCH_ROWS` | 整数 | 图高 | `stitch` 只拼多少行 |
| `ADOCAO_STITCH_SEQ` | 布尔 | 关 | 强制串行拼接（对拍/排障） |

## 7. 测试与脚本（测试）

| 变量 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `ADOCAO_TILE_EXACT` | 布尔 | 关 | 几何测试要求**逐位**相等（默认口径是 1e-5 砖） |
| `ADOCAO_GATE_PY` | 路径 | `python3` | 像素门槛比对用的 Python（需要 Pillow） |
| `ADOCAO_V7_NO_REORDER` | 诊断 | 关掉 v7 事件列的"按 kind 重排"，用来 A/B 压缩率（不改变解码结果） |
| `ADOCAO_V7_DEBUG` | 诊断 | pack 时打印 v7 事件索引构建信息（floors 数量/首尾 floor） |
| `ADOCAO_V7_BLOCK_EVENTS` | 性能/格式 | v7 事件列分块大小（默认 16384；越小 seek 越细、体积略涨） |
| `ADOCAO_V7_JSON_EVENTS` | 诊断 | v7 读侧退回"列 → JSON → 解析"的旧路（A/B 用；默认直接字段直灌） |
| `ADOCAO_DRAG_DEBUG` | 诊断 | 打印拖拽起始状态与每帧光标位移/镜头位移（排查拖动传送用） |
| `ADOCAO_ZOOM_DEBUG` | 诊断 | 打印每次滚轮缩放的锚点计算（光标点/帧缓冲像素/世界点前后值） |
| `ADOCAO_STATE_LOG` | 诊断 | 每秒一条 `state:`（相机/缩放/拖拽/播放/焦点），排查镜头乱跳用 |
| `ADOFAO_GATE_HOME` | 路径 | `~/Projects/.ADOCAO/gate` | 像素门槛基线目录（**注意拼写是 ADOFAO**，历史遗留，勿"顺手修正"） |

## 8. 相关但**不是**环境变量的东西

* `--cull-block` / `--no-exclusive` 这类开关在 `adocao --help`（CLI 唯一权威）。
* `ADOCAO_PORTABLE` / `ADOCAO_ZOOM_LEVEL` 是 **CMake 选项**（`-D…`），不是环境变量；
  读的是 `build.sh` / `build.bat` 里的 `PORTABLE` / `ZOOM_LEVEL` 变量。
