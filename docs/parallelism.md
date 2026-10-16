# 并行模型（2026-10-16）

> 目的：全库现在有**五套各管各的并行机制**，互不知情 → 过订阅、每帧新建线程池、共享缓存被迫主线程
> pin（单帧 36% 的开销就这么被串行化掉）。这份文档是**统一模型**：一个调度器、两条车道、四条不变式，
> 以及每个站点的归类与迁移顺序。**并行不许改变出图**（像素门槛 51/51 逐字节是最终守门员）。

## 1. 现状盘点（改之前的事实）

| # | 位置 | 机制 | 用途 | 问题 |
|---|---|---|---|---|
| 1 | `core/util/ThreadPool.{hpp,cpp}` | 真池 + `parallelFor` | 通用 | 用 `std::function`（类型擦除、要求可拷贝、每任务一次间接调用） |
| 2 | `render/TileMesh.cpp:45` | 上面那个池的**静态单例** | 每帧剔除 | 好；但窗口物化必须**主线程 pin** → 并行段借不到并行度 |
| 3 | `core/level/LevelData.cpp:670` | **每次调用新建 `ThreadPool pool(n-1)`** | action 分块解析 | 每次加载都 spawn 一批线程 ✗ |
| 4 | `app/MapExport.cpp:287`、`core/map/PngBand.cpp:60` | 各自 `std::vector<std::thread>` | 出图瓦片/PNG 分带 | 又一套；与 #2/#3 同时跑就**过订阅** ✗ |
| 5 | `audio/HitsoundManager.cpp:363` | 自己的 `std::thread` 块调度 | hitsound 混音 | **故意独立**（注释写明：`adocao_audio` 不该依赖 `core/util/ThreadPool`，否则单独编这个文件的 harness 会缺源码）→ 模型必须尊重这条 |
| 6 | `app/LevelLoader.cpp:66`、`app/LevelScene.cpp:151` | `std::async` | 音频解码；网格构建 | #6 的网格构建跑在**共享 GL 上下文**的线程上（GL 并非"只能主线程"） |
| 7 | `app/LoadingWindow.cpp:27,117`、`app/wizard/PageWelcome.cpp:109` | 裸 `std::thread` | 加载窗/预载 | 生命周期散落（`WizardState::preloadThread` 成员） |

## 2. 问题清单（按危害排序）

1. **过订阅**：出图（#4）与加载期解析（#3）各自开满 `hardware_concurrency`，而网格构建（#6）可能同时在跑
   → 8 核上能有 24+ 个 worker ✗。
2. **共享缓存的写竞争**：`FloorWindowCache`（每 4096 层一个窗口，128~512 槽，带淘汰）被剔除的 worker
   读、主线程写 → 只能"主线程先 pin 好再并行" ✗。**这就是 `sample` 量到的最大头**：
   `materialize` 824/2284（36%）+ `computeFloorWindow`（内层逐砖 `__sincos_stret`）都在主线程 ✗。
3. **嵌套并行无护栏**：并行加载里调并行剔除没有拦截 ✗。
4. **无取消、无优先级**：用户换谱后旧的网格构建/预载没法取消 ✗；后台预载会与**帧关键**的剔除抢核 ✗。
5. **确定性靠约定而非 API**：现在靠"每任务写自己那段、最后按序拼接"（`TileMesh` 的 `jobs` +
   `m_listTiles` ✓、`LevelData` 的 `slots` ✓）——但 API 不强制，新站点容易写成 `push_back` 竞争 ✗。
6. **异常模型缺失**：裸线程里抛异常 = terminate ✗；`std::async` 的异常只在 `.get()` 时才浮现 ✗。
7. **热循环里分配**：worker 内 `std::vector` 分配（每任务两个 vector ✗）→ 分配器竞争 + 抖动。

## 3. 模型

### 3.1 一个调度器（`core/util/Scheduler`，纯 `std::thread`，不碰平台头 → 不违反 core-purity）

```cpp
namespace adofai {

enum class Lane { Frame, Background };      // 只有两条：帧关键 / 后台

class Scheduler {
public:
    static Scheduler& instance();
    // 确定性分块：任务按 [lo,hi) 连续切片、按 index 升序派发；调用线程也参与
    template <class F> void parallelFor(size_t n, size_t minGrain, Lane, F&& fn);
    // 一次性后台任务（返回可等待、可取消的句柄）
    Handle submit(Lane, std::function<void(CancelToken)> fn);
    unsigned workers() const;               // = clamp(hw-1, 1, 8)，主线程另算
};
}  // namespace adofai
```

* **worker 数**走**策略**而不是硬编码：`Scheduler::Config { unsigned workers; bool mainParticipates; }`，
  默认 `clamp(hardware_concurrency() - 1, 1, 8)`（沿用 `LevelData` 里"超过 8 边际收益很小"的实测口径，
  但**上限是配置项**：将来按核数放开 / 让用户选 / 按电池状态降档都只是改 Config），
  **调用线程永远参与**（和现在 `parallelFor` 一样），所以 8 核默认 = 1 调用 + 7 worker。
* **车道用"优先级 + 名字"表达，不用二元开关**：`Lane` 是一个小枚举 + 每车道一个优先级整数
  （`Frame` 最高，`Background` 最低）。worker 按优先级取任务；低优先可被饿着（后台本来就是"空闲时做"），
  但**每车道要写明"会不会饿死"**。当前帧的剔除/物化走 `Frame`；解析、出图、预载、`.adocao` pack 走
  `Background`。加车道 = 加一个常量 + 定优先级 + 写饿死政策（见 §3.4）。
* **嵌套护栏**：线程本地 `inParallel` 标志 → 并行区内再调 `parallelFor` **就地串行执行**（防过订阅，且确定）。
* **取消**：`Handle::cancel()` + `CancelToken`（`bool cancelled()`）；加载类任务在块边界检查。
* **模板 `parallelFor`**：不再用 `std::function`（去类型擦除、支持 move-only 捕获）。
* **全局串行开关**：`ADOCAO_JOBS=1` → `parallelFor` 全部串行、`submit` 同步执行。
  **这是"每个并行路径都必须有串行 A/B"的项目纪律的统一实现**（替代现在每个站点自己写
  `ADOCAO_CULL=scan` / `ADOCAO_MIX_THREADS=1` 的做法 —— 那些钩子保留，但新站点只需要这一个）。
* **步长策略集中一处**：`minGrain` 由调用方给，调度器统一套用"总量小于阈值就串行"的规则
  （把 `TileMesh` 的 `kCullSerialMax` 这类经验值收进一个常量表，避免每个站点各写一套）。

### 3.2 四条不变式（比 API 更重要）

1. **并行区内不写共享缓存**。要么 (a) **先在并行区里按互斥分片预派生**成只读表（分片之间不重叠 →
   无需锁），要么 (b) **每 worker 一份私有缓存**（纯函数 → 确定性）。**绝不允许**"多线程写同一个带淘汰的
   共享缓存"（现在正是这个把 `materialize` 逼到主线程 ✗）。
2. **输出按 index 写**。任务拿到 `[lo,hi)`，写进**预分配**的容器对应位置；禁止在并行区内 `push_back` 到共享
   容器。需要变长输出时：先串行数一遍（或上界分配）+ 每任务写自己那段 + 按序拼接（现有做法 ✓）。
3. **GL 归上下文所有者**。上传/绘制/ImGui 只在持上下文的那条线程；**构建类 GL 工作**可以在共享上下文的
   线程上（`LevelScene::beginAsyncBuild` 已经这么做 ✓）—— 模型把它显式化：`GpuContext` 有一个 owner。
4. **确定性优先于速度**。任何"结果依赖线程数/调度顺序"的写法一票否决；新增并行路径必须过
   `ADOCAO_JOBS=1` 的逐字节对拍（+ 现有的位对拍用例）。

### 3.3 例外：音频保持独立

`adocao_audio` **不依赖** `Scheduler`（模块规则：单独编译 `HitsoundManager.cpp` 的 harness 不能缺源码 ✗）。
它继续用自己的 `std::thread` 块调度，但：(a) 线程数收进一个参数（已有 `ADOCAO_MIX_THREADS` 钩子 ✓），
(b) 由 app 侧把"可用核数"传进去，避免与调度器过订阅。

### 3.4 可扩展性（哪些是缝、哪些是承重墙）

**承重墙（不许动）**：§3.2 的四条不变式 —— 它们保证"并行不改出图"，任何扩展都必须在它们之内。
**缝（预留，改起来是加东西而不是改结构）**：

| 扩展点 | 现在长什么样 | 将来怎么加 | 别破坏什么 |
|---|---|---|---|
| **worker 数 / 是否参与** | `Scheduler::Config` 默认 `clamp(hw−1,1,8)` | 按核数放开、CLI/wizard 选项、省电档 | 调用线程必须仍能参与（否则主线程空等） |
| **车道** | 枚举 + 优先级整数（`Frame`/`Background`） | 加 `Stream`（容器预取）/`Gpu`（上传）/`Audio` 等：加常量 + 优先级 + 饿死政策 | 低优先车道不许无限饿死**帧关键**的依赖（例如上传必须让帧车道先跑） |
| **任务种类** | `parallelFor`（定长分片）/ `submit`（一次性） | `parallelReduce`（有序合并）、`parallelForEach`、DAG（`after(handle)` 依赖）—— **现在不建**，但 `Handle` 已经是引用计数的，加 `after` 不动调用方 | 归约必须**有序**（否则不确定 ✗） |
| **取消** | `CancelToken::cancelled()`，块边界检查 | 超时、父任务级联取消、取消传播到 IO | 被取消的任务不许留下"半写"的输出（要么全写要么不写） |
| **暂存 / 分配** | 热点内复用 per-worker 缓冲（§5 第 6 步） | per-worker arena、`ThreadLocalBuffer<T>`、分配器按车道切 | 暂存不许跨任务泄漏状态（否则不确定 ✗） |
| **观测** | `ADOCAO_JOBS=1` 串行钩子；沿用 `ADOCAO_PROFILE=1` | 每车道/每任务的耗时直方图、队列深度、核占用（`docs/profiling.md` 的口径） | 埋点不许进抓帧路径的确定性输出（老规矩 ✗） |
| **平台策略** | core 只认 `std::thread`（core-purity 铁律） | app 侧注册回调：macOS QoS、Windows 线程优先级、CPU 亲和 | 平台头/API **不许进 core**（`scripts/check-core-purity.sh` 会红） |
| **音频** | 明文例外（自造线程） | 给 `HitsoundManager` 一个"可用核数"入口（已有 `ADOCAO_MIX_THREADS` 钩子的方向），由 app 把调度器的预算传进去 | 不许让 `adocao_audio` 依赖 `Scheduler`（单独编 harness 会缺源码 ✗） |

**明确不建**（避免过度设计，但留了上面的缝）：fiber/协程、工作窃取、全局 DAG 调度器、自研无锁队列。
真需要时的入口：`Handle` 的依赖链接（DAG）、车道优先级（窃取）、`Config`（线程数策略）。

## 4. 逐站点归类（改造后的样子）

| 站点 | 车道 | 并行粒度 | 备注 |
|---|---|---|---|
| 剔除（`TileMesh::ensureDrawList`） | Frame | 命中块 → 按 ~32K 砖切任务 | 已是并行 ✓；**窗口物化要一起并行**（见 §5 ①） |
| 每砖派生（`deriveRange` / 窗口） | Frame | 每任务一个**互斥块集** | 关键改动：不再 pin 到主线程，改成并行预派生只读表 |
| 实例属性构建（`m_listAttr`） | Frame | 与剔除同任务 | 现在就是每任务写自己的段 ✓ |
| 拖尾/行星更新 | （串行） | — | O(拖尾长度)，量级小，不该并行 |
| 上传 + 绘制 + ImGui | 主线程 | — | GL 上下文所有者（不变式 3） |
| 网格构建 `TileMesh::build` | Background（共享 GL 上下文） | 按块 | 已有 `std::async` ✓ → 迁到调度器，可取消 |
| 谱面解析（`angleData`/actions） | Background | 分块（现有 `findActionSplits` ✓） | 去掉**每次新建池**；换 `Scheduler` |
| 位置/时间/图标计算 | Background | 按块 | 加载期 O(n) 的大头 |
| 剔除索引建表（粗+细） | Background | 按块 | O(n)，天然并行 |
| 角度紧凑存储 | Background | 按块 + 有序归并 | 字典需归并（确定性：按块序取首个出现） |
| 出图/瓦片/PNG 分带 | Background | 瓦片/带 | 去掉各自造线程 |
| 容器 pack / decode（zstd 分块） | Background | 按块 | CLI 也能吃到多核 |
| 音频解码 + hitsound 混音 | 音频自有 | 块 | 例外（§3.3） |
| 加载窗/向导预载 | Background | — | 收编裸线程；支持取消（换谱时掐掉旧的） |

## 5. 迁移顺序（每步都可独立验收，门槛 51/51 + 位对拍）

1. **调度器骨架 + 窗口物化并行**（最大一刀）。`Scheduler::parallelFor`（模板、车道、嵌套护栏、
   `ADOCAO_JOBS`），先把 `TileMesh` 的剔除迁过去；把"主线程 pin 窗口"改成**并行预派生**：
   按命中块集切成互斥分片，每个任务在自己分片内调 `deriveRange` 写进 per-frame 只读表。
   预期：`sample` 里那 36% + 34% 的 `materialize`/`computeFloorWindow` 从主线程上消失 →
   宽视野重建帧 **160~240 ms → ~20~30 ms**。验收：门槛 51/51 + `ADOCAO_JOBS=1` 逐字节相同。
2. **收编 #3/#4**：`LevelData` 的每次新建池、`MapExport`/`PngBand` 的自造线程 → `Scheduler`（消过订阅 ✗）。
3. **细锚点**（第 2 条打法）：位置提供者加 512 层锚点 → 每窗只走 512 层 trig，再 ~8×。
4. **后台车道 + 取消**：网格构建/预载/出图进 `Background`，换谱时 `cancel()` 旧任务。
5. **加载期并行**（位置/时间/图标/索引/角度存储）→ 2³¹ 的加载时间也 ∝ 核数。
6. **per-worker 暂存**（热点内不再分配）——放最后，属于边际优化。

## 6. 验收与风险

* **验收**：每一步都要过 ① `ctest` 全绿；② 像素门槛 **51/51 逐字节**；③ **`ADOCAO_JOBS=1` 逐字节对拍**
  （新的统一串行钩子）；④ 涉及数值的另加位对拍（`ADOCAO_MESH_DUMP=1` 的 FNV、`cull_index` 逐元素）。
* **新增建议用例**：`tests/scheduler_determinism_test.cpp` —— 同一负载在 `JOBS=1/2/4/8` 下输出逐字节相同
  （把"确定性"从约定变成测试 ✓）。
* **风险**：① macOS QoS/线程优先级（帧车道要 user-interactive 级别）→ 平台 API 留在 app 侧注册回调，
  不进 core（core-purity ✗）；② 车道优先可能饿死后台上传/解码 → 后台任务必须能在 1 帧内让出；
  ③ 过度设计：模型只保留"车道 + parallelFor + submit/cancel"三件，**不做** fiber/依赖图/工作窃取复杂度；
  ④ 音频例外要写进文档，否则后来者会"顺手统一"而破坏 harness ✗。
