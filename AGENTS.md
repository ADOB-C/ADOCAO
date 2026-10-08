# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 导航（先读这里，省 token）

- 目标结构蓝图 + 已拍板决策 1–10：`docs/project-structure.md`
  ✅ P1–P5 全部完成：目录搬迁+CMake 拆库（P1+P2）→ PlaybackEngine 拆 core/timeline（P3）→
  app 拆分（P4：GameWindow → `app/CameraController`/`app/LevelScene`，LauncherWindow → `app/wizard/` 分页）
  → 资产并入 `assets/` + g_sc 清理 + 文档/CI 同步（P5）
- **入库文件里绝不出现机器绝对路径**（2026-10 真出过事：验收清单把 `/Users/<用户名>/Documents/Charts/...`
  写进了仓库，352 个提交里 50 个带着它，最后只能 `git filter-repo` 重写历史 + force-push + 重打 21 个标签）。
  本地资源一律走**环境变量**（如 `$ADOCAO_CHARTS`）或 `~/` 相对形式；`scripts/capture-gate.sh` 的 `abs()`
  会把 `~/` 展开成 `$HOME`。提交前自检（应为空）：
  `git grep -nE "/Users/[A-Za-z0-9._-]+/|/home/[A-Za-z0-9._-]+/" -- . ':!build'`
  （真做过一次：`git filter-repo` 会**移除 origin** 并让分支失去 upstream —— 重写后要
  `git remote add origin …` + `git branch --set-upstream-to=origin/master master`，否则
  `scripts/push-ci.sh` 里的无参 `git push` 会报 "no upstream branch"）。
- **长期保留的产物统一放 `~/.adocao/`**，别放 `/tmp`（系统会定期清，像素基线就这么丢过一次）：
  `gate/baseline`（像素门槛基线，`ADOFAO_GATE_HOME` 可改）、`cache/gh`（gh CLI 缓存，
  沙箱里要用 `XDG_CACHE_HOME=~/.adocao/cache/gh`）、`tools/`（如 git-filter-repo）、`scratch/`（一次性文件）。
  重写历史前的镜像**单独放** `~/Projects/.ADOCAO-Backup-1`（352 提交 / 21 标签，**里面仍含用户名**，
  这正是它作为回退手段的意义）：它的 origin 已被移除，**不要**把它 fetch/pull 回主仓库 ——
  那会把清理掉的旧历史原样带回来。不需要时 `rm -rf` 即可（同目录有 BACKUP-README.txt 说明）。
  **但临时抓帧文件必须留在工作区内**（门槛默认 `build/gate-scratch`）：受限沙箱里往工作区外**写**会被拒，
  于是 `/usr/bin/time` 的重定向失败、抓帧静默不发生，`check` 会报出一整片假的"不一致"（真踩过：
  把 `gate/check` 也放进 `~/.adocao` 后，51 个状态全报不一致，其实一张新图都没生成）。
  规则：**基线（只读）可以放 `~/.adocao`，临时/中间产物一律放工作区内**。
- **库侧公共 API 都在 `adofai::` 里**（2026-10 起，为抽出 ADOFAI.Lib 做准备）：core/audio/render 的
  头文件与实现都包在 `namespace adofai { … }` 里；app/tests 侧的文件用
  `using namespace adofai;`（消费方当然也直接写 `adofai::X`）。两条相关规矩：
  * 在 app 头里**别在全局前置声明库类型**（会和 `using` 打架变成 "ambiguous"）——写 `namespace adofai { class X; }`；
  * 系统/第三方类型（`ma_device`/`ma_decoder` 之类）的**前置声明必须留在 adofai 之外**，否则声明出来的是
    `adofai::ma_device`，跟真类型成两个东西。
- 资产查找走 `adofai::assetOptions()`（`core/util/AssetPaths.{hpp,cpp}`）：库默认只认"相对当前目录"、
  不找 zip、不找 data 目录；**产品名与布局集中在 `app/AssetSetup.cpp::configureAssetPaths()`**
  （zip = `ADOCAO-data.zip`、dataDir = `ADOCAO-data`、roots = CWD→exe 目录→macOS bundle 的 `../Resources`→上溯 3 级；
  非 Windows 优先 CWD，Windows 优先 exe 旁）。hitsounds 的相对目录同理由 `HitsoundManager::setDefaultHitsoundSubdir`
  设进来。**改这些名字/顺序时别退回 `core/util/DataFile.cpp` 里**——那里现在一个产品名都没有。
- **模块依赖：core 不依赖 archive**（2026-10 解耦，依赖倒置）。`core/level/ByteSource.{hpp,cpp}` 只有
  容器种类（`LevelArchiveKind`）、magic 判断（`sniffLevelArchive`）和 `WindowSource` 接口 + `ArchiveBackend`
  钩子；真正的解压（lzma/zstd、半窗 `ArchiveStream`）在独立的 **`archive/` 模块**（CMake target
  `adocao_archive`），由 `adofai::archive::install()` 注册 —— **`main()` 顶部调一次**（GUI / CLI /
  无头子命令都要，漏了就会出现"明文能读、.xz 读不了"）。只想解析明文谱的目标只链 `adocao_core` 即可，
  不背压缩库（`tile_expansion` / `geom_probe` / `tile_geometry` 就是这种目标）。
  `archive/Install.hpp` 刻意不带 `<lzma.h>`：app 只调 `install()`，不该为它配压缩库头路径。
- **音色目录必须带尾分隔符**：`HitsoundManager` 下游是 `m_assetsDir + type + ".wav"`，目录少了 `/` 会拼出
  `assets/hitsoundsKick.wav`（P1 真踩过，而且**像素门槛全程 `--no-hitsound` 看不见**）。现在拼接统一走
  `withTrailingSlash()`；门槛也补了守卫：清单里带 `+hitsounds` 的状态会真的加载 WAV，且对**所有**状态断言
  日志里没有 `Hitsound: Cannot open` / `Failed to read WAV`（负向对照验过：打断补斜杠那步立刻变红）。
- **`archive` 的公共头是 pimpl 的**（2026-10，ADOFAI.Lib 那边提的补丁）：`ArchiveStream` 的三方状态
  （`lzma_stream`/`ZSTD_DStream`/半窗缓冲）都在 `.cpp` 里的 `struct ArchiveStream::Impl`，头里只有
  `std::unique_ptr<Impl>` + 7 个访问器声明 → **`archive/LevelArchive.hpp` 不含任何 lzma/zstd 类型**，
  `adocao_archive` 的 `liblzma libzstd_static` 因此是**真正 PRIVATE**（消费者不必配压缩库头路径）。
  改 `ArchiveStream` 时新状态一律加进 `Impl`，别往头里塞成员。
  **跨版本兼容：别用 `LZMA_VERSION`/`ZSTD_VERSION` 去 gate 结构体字段** —— `lzma_mt::memlimit`
  在 **5.4.0** 就被拆成 `memlimit_threading` + `memlimit_stop`，按版本号判断时 5.4/5.5 的系统库
  会走进旧分支、然后报 `no member named 'memlimit'`（ADOFAI.Lib 的 `linux-system-deps` 那一格真红过，
  内嵌的 5.6.3 让本地永远看不出来）。现在走 `archive/LzmaMtCompat.hpp` 的**字段探测**
  （`if constexpr` + concept；discarded 分支必须留在**模板**里，非模板的 `if constexpr` 两边都会被检查），
  永久用例 `tests/lzma_mt_compat_test.cpp`（两种结构各喂一遍，`static_assert` 选择本身）。
  推论：**平台性风险不在"跑过没跑过"，而在"依赖的版本落在哪个区间"** —— macOS 上 system-deps
  验过 4/4 也挡不住这个（brew 的 lzma 5.8.4 ≥ 5.6）。注意 `tests/level_parse_test.cpp`
  **自己**在用 lzma/zstd API 造压缩样本（`lzma_easy_buffer_encode` / `ZSTD_compress`），所以那个
  测试目标的 include 与链接依赖不能跟着删。
- **`ADOCAO_ASSET_ZIP`**（根 CMake，默认 ON）：OFF 时 `DataFile.cpp` 不编 zip 那条路
  （`ADOCAO_HAVE_MINIZ`），core 不链 miniz。**这是 OFF 能成立的前提**：PNG 写出统一走 zlib
  （`core/map/PngDeflate.hpp` 把 zlib 名字映射成 `mz_*`，`PngBand`/`PngStream` 共用），
  于是 miniz 的唯一使用者只剩 `DataFile`。实测 OFF：core 里 0 个 `mz_` 符号、`adocao image`
  解码后像素与 ON 完全一致（连 PNG 文件字节数都相同）、门槛子集逐字节相同。
  注意 `core/map/**` 不进 ADOFAI.Lib（见 PLAN.md），所以库里 OFF 天然成立、也不需要 zlib。
- **`tiles` 不能先 `resize(n)` 再 `push_back` 末尾那块 extra 砖**：libc++ 会按 2× 增长 → 容量永久停在 2n
  → 每层从 24 B 变成 **48 B**（`calculateTilePositions`，`core/level/LevelData.cpp`）。正确写法是先
  `reserve(n + 1)` 再 `resize(n)`。实测：修复前 `cap = 2n`（三张谱一致）、修复后 `cap = n + 2`；
  **primer-final（6841 万层）的 GUI 播放换出量从 +296 MB 变成 0**（峰值 RSS 5.9 GB），
  像素门槛 51/51 逐字节不变。（背景与逐项开销见 `docs/scale-1e8-to-2e9.md` §7/§14。）
- **没有 SetSpeed 事件的谱面不该分配 `setSpeedByFloor`**：`processActions` 里那张 `vector<SS>`（12 B/层）
  曾经无条件 `resize(n)`，1e8 层就是 1.2 GB 白占（探针实测：25 万层的无害谱上有一块 250001 x 12
  的分配）。现在改成第一次遇到 SetSpeed 才 `resize`，读侧的逐层循环用 `empty()` 跳过 ——
  空 vector 与原来**逐位等价**（默认构造的 SS 全 false/0，循环体本来就什么都不做）。
  注意：这类**加载期瞬态**用 peak RSS 量不出来（会被后面 Timeline 的更大峰值盖住），
  要按 `docs/scale-1e8-to-2e9.md` §5 的办法用 operator new 记账 + 返回地址定位。
- **`angleData` 的预留别用固定常数**：每元素字节数实测差 4 倍（The Moon 3.53、angles360 5.76、
  MYC 13.39），旧的 `reserve(len / 3)` 于是预留 0.67x~4.46x —— MYC 白请求 171 MB、纯整数写法反而
  **低估**到 0.67x 触发一次倍增。现在按 `estimateAngleCount()` 采样估（8 个 32 KB 窗口、按密度外推到
  整段、取最大估计 + 12.5% 余量），实测五种谱面都落在 1.13~1.14x（出现 ~2x 就说明发生了重新分配）。
  **它只影响 capacity，不碰值**：`parseAngleDataRegion` 一行未动，逐位对拍（`a.bytes(&v, 8)` 的 FNV）
  与像素门槛都是绿的。
- **`parseNumber` 的 `e` 是"这个值/这个区间的真实末尾"，token 正好填满它是正常的**：两个调用方
  （`angleData` 区间、action 字段，后者来自 `skipValue`）都传真实末尾；流式窗口那条路有它自己的
  可续扫描器，不经过本函数。曾经这里把 `q >= e` 当"截断"返回 false，于是**所有非整数的 action 字段**
  （`bpmMultiplier: 0.5`、`beatsPerMinute: 1.5`、`angleOffset`…）让整份文件放弃快路径 ——
  整数走整数快路径、不经过那里，所以**只有小数中招**。实测 MYC：放弃时 4.60 s / 4,943 MB，
  修复后 **1.39 s / 1,473 MB**。三个 "Won't You" 版本与 `tests/charts/angles360.adofai` 全都受影响。
  **推论（血的教训）**：① 对拍测试比的是"两条路的输出"，而快路径一放弃，"快路径那次加载"**就是**
  旧路径 → 永远绿；② 所以 `ADOCAO_FAST_REQUIRE=1` 这类钩子**必须验它真的会触发**（拿一个"本该放弃"
  的输入做负向对照），否则它形同虚设 —— 本次它曾对所有真实谱都返回 declined；③ A/B 要把**整个改动**
  一起回退，只回退其中一行会得出错误结论（本次就因此错误地排除了自己的改动）。
- **`.adocao`（自研二进制容器，v2）**：列式 + **每列按其取值基数选编码**
  （`Dict`/`Const`/`DeltaVarint`/`BitPack`/`Rle`/`DeltaRleVarint`/`Raw` + 段级 zstd），**逐位无损**、
  坏数据明确失败、编码器自检（编完解回来逐位比，最坏退回 `Raw`）。CLI：`adocao pack <谱> <out.adocao> [--codec-report]`。
  接入方式：core 只按 magic `ADO1` 分派（`ArchiveBackend::decodeAdocao` 钩子，**core 依旧不依赖 archive**），
  解开后与两条 JSON 路径**共用同一个 `finishLoad()`**；`.adocao` 与 JSON 出的图逐字节相同（MYC 677 万层验过）。
  实测：MYC 611.18 MB → **6,120 B**（10.5 万倍）、v300 300.95 MB → 6,128 B、The Moon 15.60 MB → 3,544 B、
  angles360（人工谱）→ 9,344 B（字典主导）。规范 + 逐项实测分析见 `docs/adocao-format.md`（§5.1–§5.11）。
- **2^31−1 层目标的 32 位边界（#0，2026-10）**：计数/长度一律 **int64**，只在必须交给 32 位 API
  （`GLsizei`、`int` 参数）时收窄，而且**必须**走 `core/util/CountLimits.hpp` 的 `fitsInt32()` /
  `toInt32Checked()` —— **绝不静默截断**。纯函数 + `tests/limits_test.cpp`（ctest `limits`）是这条
  边界的唯一验证手段：本地最大谱（primer 6.84e7 层）只有目标的 **3%**，真造 21 亿层的谱不可能。
  已完成阶段 1–2（"int64 算、收窄前显式判、装不下就喊"）；**阶段 3**（循环索引、`floor` 本身的类型、
  GL 分批）未做，见 `docs/scale-1e8-to-2e9.md` §6.1。
- **像素/渲染类问题：先出差异图，再谈实验**（2026-10 血的教训）。一个“值逐位相同却 31/51 个抓帧状态
  变色”的谜，我做了**七个**受控实验（值 / 调用形状 / 串行并行 / 类布局 / FMA 收缩 / 堆分配 / 线程历史）
  全部否掉，最后**做了一张三联差异图（基线 | 新版 | 差异×N）**，一眼就看到“**只有拖尾不同**”——
  真因是 `--capture` **没有关掉拖尾的自适应采样率**（它由测得的**帧时间**驱动），于是“快了几毫秒”
  就画出**不同长度**的拖尾。规则：① 差异先出图；② 抓帧必须确定性（`--capture` 已强制
  `trailAdaptive=false`），任何“由帧时间反馈决定的量”都不许进抓帧路径。
  ③ 本沙箱里 `~/.adocao` **不可写** → 重存基线要用工作区内目录（`ADOFAO_GATE_HOME=build/gate-base`）。
  ④ **两层兜底 + 日志前缀**（各自的边界要说清 ✗）：
  * **钉死 + 守卫**（今天这个 bug 的解法）：`--capture` 时 `trailAdaptive=false`，并在抓帧块里
    自检 `trailAdaptive`，为 true 就 `LOG_E`。**负向对照验过**：故意打开 → 日志确实出现
    `capture-selfcheck: trailAdaptive=true …` ✓。
  * **自检**（防将来的未知量）：`--capture` 写完 PNG 后再渲染同一帧一次并逐字节比，不一致
    `LOG_E` + `exit(3)`。**它的边界**：只抓得住**每次渲染都不同**的不确定性（随机 / 未初始化 / 竞态），
    **抓不住**"跨帧才漂移"的那类（拖尾 governor 就是 —— 负向对照里两次连续渲染它还没反应过来，
    所以自检**没响** ✗）。所以新增量时仍要人问一句"它会不会因为机器快几毫秒而变"，再按上一条钉死。
  * **日志前缀**：抓帧块里新增的日志**不许**以 `capture:` 开头 ✗（`capture-gate.sh` 用"日志里最后一条
    `capture:` 行"核对本次的 `tile=`/`req=`；自检日志一开始也叫 `capture:` → 51 个状态全判 FAIL，
    门槛**自己**抓住了这个疏忽 ✓）。自检统一用 `capture-selfcheck:` 前缀。
- 纯逻辑层护栏：`scripts/check-core-purity.sh`（core/ 禁 glad/GLFW/imgui/miniaudio/tinyfiledialogs/平台头，CI 已接入）
- 解析对拍测试：`tests/level_parse_test.cpp`（快路径 vs cleanJson+DOM 逐位比对，用例在
  `tests/level_fixtures/`，生成脚本 `tests/gen_level_fixtures.py`）；`ctest --test-dir build`
- 几何测试（三层，2026-10 起）：`tests/tile_geometry_test.cpp`（中旋五边形不变量 + 调用点护栏）→
  `tests/tile_expansion_test.cpp`（**CPU 逐位**：形状表展开 vs 参考实现，168491 形状 / 3.3e7 坐标）→
  `tests/geom_probe_test.cpp`（**GPU 逐位**：真跑 `assets/shaders/tile.vert`，4.7e7 坐标；无显示时 SKIP）
  → `scripts/capture-gate.sh store|check`（**像素逐字节**，51 个状态，基线默认 `~/.adocao/gate/baseline`、不入库；带 `+hitsounds` 的状态会真的加载音色）；
  默认口径是几何尺度容差 1e-5 砖，`ADOCAO_TILE_EXACT=1` 才要求逐位（见 `docs/rendering.md` 的"砖块几何"段）
- shader 一致性：`scripts/check-shader-fallback.sh`（`render/Shaders.hpp` 的内嵌回退 GLSL 必须与
  `assets/shaders/*` 逐字相同）
- 待办（已筛选）：`TODO.md`（未完成清单 + 遥远的未来：MoveTrack）
- 已彻底删除：GPU compute culling（2.0.0 起不需要，勿再引入）
- 脚本：`scripts/push-ci.sh`（push → gh run watch；`--watch` 默认输出平台耗时/产物）
  `scripts/run.sh --debugger` | `scripts/release.sh`（支持 x.y.z-AlphaN/-BetaN/-RcN）
  `scripts/make-app.sh`（macOS：把 `build/adocao` + `assets/` 打成自包含的 `build/adocao.app`，含 ad-hoc 签名；CI 的 macOS job 也用同一个脚本）
  `build.sh`（已有 build/ 缓存且不带参数 = 增量构建，不提问）


## 细节文档（AGENTS 只留铁律与索引 —— 按需读，别全文读）

2026-10 把长节**整段搬了出去**（原文一字未改，只换文件）：

| 文档 | 什么时候读它 |
|---|---|
| `docs/level-loading.md` | 动解析 / 流式窗口 / 快路径回退 / `Timeline::build` / 加载时间与内存 |
| `docs/rendering.md` | 动渲染、几何、实例流、剔除、图标、坐标约定、**像素门槛怎么用** |
| `docs/audio.md` | 动音频、hitsound 混音、导出 WAV、audio-as-chart |
| `docs/image-export.md` | 动 `adocao image`（真无头出图）、瓦片与拼接 |
| `docs/cli.md` | 加/改命令行开关、拖尾采样参数 |
| `docs/adocao-format.md` | `.adocao` 容器规范与逐项实测 |
| `docs/scale-1e8-to-2e9.md` | 十亿层目标的三笔账、实测口径（§5 的记账法、§6.1 的 32 位边界） |
| `docs/project-structure.md` | 目录 / CMake 结构、历史决策 |

**为什么拆**：本文件曾 70 KB，超过会话的指令预算（65536 B）后**尾部被静默截断** —— 被切掉的正好是
「像素门槛怎么用」和「几何位精确」那两段（真踩过：会话里看不见，只能重新摸一遍）。
**规矩**：新增内容写进对应主题的文档，本文件只加一行指针；本文件必须始终明显小于 65536 B
（改完用 `wc -c AGENTS.md` 看一眼）。

## Project

A native C++ / OpenGL "A Dance of Fire and Ice" (冰与火之舞) level viewer. Plays `.adofai` custom levels on Windows, Linux (Wayland) and macOS.

A Vulkan port exists at `../ADOCAV/` (same game logic, Vulkan 1.2 backend).

Reference implementations:
- `../ADOFAI-JS/` — Core angle parsing
- `../Re_ADOJAS/` — Three.js web player (hitsound, camera, decorations)
- `../ADOFAN_PIXI/` — PixiJS web player
- `../ADOFAI/A Dance of Fire and Ice/` — Original Unity game. Decompile `Assembly-CSharp.dll` with:
  `../ADOFAI/dnSpy/dnSpy.Console.exe -o ../ADOFAI/decomp_game "../ADOFAI/A Dance of Fire and Ice/A Dance of Fire and Ice_Data/Managed/Assembly-CSharp.dll"`
- `../ADOFAI_HitSound/` — Maicy0609's C++ hitsound generator (RapidJSON, Nyquist filter, equal-power pre-scaling, double mixing). Reference for loading performance and hitsound synthesis.

A WinUI 3 C# launcher lived in a separate repo at `../ADOCAO_WinUI3_Launcher/` —— **已废弃，
不要再为它保留 CLI 兼容性**（历史原因：早期命令行语法是它的调用约定）。

## Branches

- `master` — Current development branch (OpenGL 4.3)

## Floating-Point Precision Rules

**ALL timing and position values must use `double` (float64).** Float32 precision loss at extreme values causes artifacts.
- `angleData`: `std::vector<double>` (16 decimal places)
- `tileStartTimes[]`, `m_elapsedTime`, camera target: all `double`
- GPU uploads: `float` only at last step (camera-relative offset), values stay small

## Build & Run

**Windows:** `build.bat` (`build.bat portable` for static, `build.bat exzoom` for min zoom 0.5)
**Linux / macOS:** `chmod +x build.sh && ./build.sh` — 首次运行提问；已有 `build/` 缓存后不带参数运行即增量构建（不提问、不重新 configure；改选项需带参数如 `-U -Portable`）
**Manual:** `mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --parallel`

Minimum CMake 3.20. C++20. OpenGL 4.3+ required. Dependencies via FetchContent (GLFW, glm, RapidJSON, Dear ImGui, miniaudio, stb, miniz, tinyfiledialogs).

`--debug` flag enables debug console, disables hitsounds by default.

**Logs** (`ADOCAO.log`, written by `app/Application.cpp::logPath()`): **next to the executable** — one predictable location no matter which directory you launched from (it used to be CWD-relative, which littered whatever directory you happened to be in, `$HOME` included). The one exception is a macOS `.app`, where the executable directory is `Contents/MacOS` and writing there would break the code signature; that case uses `~/Library/Logs/ADOCAO/`. Fallbacks when the executable directory is not writable: `~/Library/Logs/ADOCAO/` (macOS), `%LOCALAPPDATA%\ADOCAO\logs` (Windows), `$XDG_STATE_HOME/ADOCAO` or `~/.local/state/ADOCAO` (Linux), then the temp directory. The first `Log file:` line in the log records the path actually used.


## File extensions

All project headers use `.hpp`. Third-party includes (miniaudio, imgui, tinyfiledialogs) retain their original extensions.
