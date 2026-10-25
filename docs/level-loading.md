# 关卡加载与 Timeline（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

## 关卡加载（Level loading）

一次加载分两步：解析 JSON → `Timeline::build()`。磁盘 I/O 与 JSON 解析占了几乎全部。

**容器**：除明文 `.adofai`，还支持 `.adofai.xz`（xz/LZMA2）与 `.adofai.zst`（zstd）——
`../Song.adofai`（audio-as-chart 编解码器）就用这两个格式存谱面（4 分钟谱 ~1.5 GB 文本 → 50–80 MB）。
`core/level/ByteSource.cpp` 的 `sniffLevelArchive` 按 **magic** 识别（不看扩展名），再交给 **`archive/`** 模块在**内存里**解压，然后走下面的解析路径，
不写任何临时文件。xz 用 `lzma_stream_decoder_mt`（与 `../Song.adofai` 的 `xz.c` 同配置）：
那些谱是多 block 压出来的，单线程只有 ~0.5 GB/s，MT 到 ~2.8 GB/s（1.18 GB 实测 2549 ms → 534 ms）。
实测直接读 `.adofai.xz`：43 MB → 1.18 GB 谱面 ~2.4 s、50 MB → 1.40 GB 谱面 ~2.9 s（含解码，跑动区间 ±0.15 s）。
内存上有三处专门处理（都是量出来的，1.18 GB 输出 / 10 核 / 64 MiB 字典 / 19 blocks）：
  * `mt.memlimit_threading` 给 1 GiB 上限（`../Song.adofai` 那边是 UINT64_MAX）：解码 475 → 585 ms，
    解码器峰值 1.67 → 1.04 GB，+110 ms 换 0.6 GB；
  * 解压前从 xz 流尾的 footer + index 反推出解压后总大小并一次 `reserve` 到位（zstd 用
    `ZSTD_getFrameContentSize`）。靠 `std::string` 自己增长时新旧缓冲同时存在，峰值会白多 ~2.4 GB
    （实测 1.67 → 4.06 GB）；多留一个 chunk 免得最后一次 resize 又触发全长拷贝；
  * 于是"解码 + 1.18 GB 输出"峰值 4.06 → 2.19 GB；整份加载（含解析结构）峰值 5.32 → 3.20 GB
    （1.40 GB 那张：5.92 → 4.41 GB）。明文路径不受影响。
**`.ilytx`（Re_ADOJAS 的自研容器）**：压缩层与 `.adofai.xz` **完全一样**（都是 xz magic），区别
只在解压后的载荷 —— 它是 **tar**（`manifest.json` + `level.ilybin` + `audio/…`），不是 JSON。
所以"是不是 ilytx"只能**解压之后**才知道（core 的 `isTarPayload()`：ustar magic 在偏移 257）：
  * 半窗流式路径在首块就认出 tar、**主动让路**（它解析不了 tar），日志写明理由；
  * 整份解压之后由 `archive/Ilytx.cpp` 拆 tar、`archive/Ilybin.cpp` 解 ilybin，再**翻译成等价的
    `.adofai` 明文**交回给下面这条唯一的解析路径（不建第二条 LevelData 填充链）。
    ⇒ ilytx 与 .adofai 的语义/像素天然一致（`tests/ilytx_test.cpp` 逐位比对 + 出图逐字节相同）。
规范与取舍（不解 `angle`/`twirl` 派生段、忽略 `extraProps`、不抽内置音频、翻译的文本开销）见
`docs/ilytx.md`。

`tests/level_parse_test.cpp` 会把每个明文 fixture 在内存里压成 xz/zstd 再加载一遍，要求逐位一致，
并检查截断的流是干净失败而不是崩。

**流式窗口（压缩输入默认路径）**：`archive/LevelArchive.hpp` 的 `ArchiveStream`（实现 core 的 `WindowSource` 接口，见文件顶部那节）用两块**固定地址**的半窗
（默认 4 MB，`ADOCAO_WINDOW_KB` 可调）交替解压；消费方 `WindowParser`（同在 `LevelData.cpp`）按根成员
逐个处理、把残缺的值 carry 到下一块开头，于是 10 GB 文本不再需要 10 GB 匿名内存——那 10 GB 匿名页装不下
时会被系统压缩/写 swap，实测吞吐 1.35 GB/s → 0.12 GB/s，而且每次访问都要换回来。

- **默认启用**（压缩输入）；`ADOCAO_WHOLE_DECOMPRESS=1` 强制退回整份解压，`ADOCAO_WINDOW_KB`
  调半窗大小。半窗大小实测（TNR 1.18 GB）：2 MB 1894 ms、4 MB 1928 ms、8 MB 1944 ms、16 MB 1960 ms、
  96 MB 2062 ms——越小越贴缓存，取 **4 MB**（纯速度/内存权衡：窗口大小不再受"单个值多大"限制，
  `skip`/`settings`/`path` 都是可续扫描器，超长 `levelDesc`、超大 `decorations` 数组照样跨窗流过去）。
- 已验证：40 个 fixture + 284 KB 合成谱（2000 个跨窗 action）在 **4 KB 半窗**下与整份解压逐位一致；
  256 KB 不可压伪数据的半窗拼接（0 / 1 B / 1 KB 三种 carry）逐字节一致；"单个值大于半窗"能报
  `stuck()` 而不是死循环；1.18 GB / 1.40 GB 两张真实 `.xz` 在 96 KB / 1 MB / 8 MB 半窗下与整份
  解压**逐位一致**（13 个节的 hash 全同）。`ADOCAO_WINDOW_REQUIRE=1` 把"窗口路径放弃"变成失败，
  用来证明用例真的覆盖到它（这个坑踩过一次：静默回退让测试全绿）；`ADOCAO_FAST_REQUIRE=1` 界定
  "哪些 fixture 本来快路径就吃不下、因此允许两条路都放弃"。
- **第四个坑（2026-10-15，`docs/2e9.md` §4 ①）：`parseNumber` 的指数回退。** 半窗正好在
  `e` / `e±` 之后用完时，它退回指数标记、当成"光秃秃的 e 不算指数"，于是 ① 提交了一个
  **截断错值**（`4.547473508864641` 而不是 `4.547473508864641e-13`）；② 残留的 `e` 被 carry 到
  下一窗，下一窗第一个字节就是 `e`（`parseNumber` 立刻失败）→ 整窗原样 carry → 半窗被自己占满
  → `stuck()` → **静默回退整份解压**。`Unity.wav_rate`（逐采样、**全是指数写法**）默认 4 MB 半窗
  3/3 拒收、8 MB 才收 —— 不是"窗口不够大"，是边界相位恰好绕开了。修法：把"`e`/`e±` 之后就是
  缓冲区末尾"与"`e` 之后不是数字"分开，前者返回 false（= 这里没有完整数字），由窗口 carry 重放。
  永久用例 `windowExponentBoundarySelfTest()`：定点相位 + 8 种半窗相位 × 11 种数字形式，
  **反面对照验过**（回退修复 → 立刻报"窗口路径拒收"）。顺带把回退从静默改成 `LOG_W`。
  诊断钩子：`ADOCAO_WINDOW_DBG=1` 会打出每窗的 `本次已提交 / 绝对消费 / carry / 首字节`——
  只看"本次已提交"会误判（窗口中途转移时它是相对本次调用的，不是窗口内偏移）。
- 覆盖范围：真有事件多样性的谱面也验过——6 张社区谱（Tempest 31.7 万事件、The Moon、Singularity、
  Fledgling、DONE 等）与 641 MB / 618 万事件 / **7 种事件类型**的 MYC，都对每个文件在内存里压成
  xz 与 zstd、再用 **4 KB 半窗 + 禁止回退**走窗口路径，与整份解压逐位一致；两张 `.xz` 巨谱
  （1.18 / 1.40 GB）同样逐位一致。`archiveRoundTrip()` 就长在 `tests/level_parse_test.cpp` 里，
  喂任何真实文件（含压缩谱）都会跑这套比对 —— 这一条正是被"你造的大谱只有一种事件"问出来的。
- 第三个坑：`skip`/`settings`/`path` 明明带状态可续，驱动却按"从该项起点重放"处理，于是
  **任何大于半窗的单个值都会放弃回退**（一个超大 `decorations` 元素、200 KB 的 `levelDesc` 都会）。
  现在这三类扫描器整窗提交、跨窗续扫，`angleData`/`actions` 才按元素边界 carry。
  永久用例：`windowHugeValueSelfTest()`（200 KB 字符串 + 500 个装饰物 / 4 KB 半窗）。
- 踩过的两个坑（都是"只在窗口中途发生状态转移时才出现"，小 fixture 掩盖）：
  1. 驱动把"相对当前游标的已提交字节数"当成了 `ArchiveStream::consume()` 要的**窗口内偏移**；
     第一次调用两者恰好相等，所以只在 actions 数组不从窗口开头起时才暴露 → carry 退到已解析
     对象中间，回放时重复解析（1.18 GB 谱 8 MB 半窗多出 9 个角度值 / 11660 个 action）。
  2. zstd：帧结束时半窗正好填满 → 该块 `eof` 未置位，下一块只含 carry 又被判"做不完"直接丢弃，
     结果**丢了末尾 1 KB**。现在尾块会交付一次。
- 实测（默认 4 MB 半窗 vs 强制整份解压，`loadFromFile` + 结构，都不换页）：

  | 谱面 | 窗口（默认 4 MB） | 整份解压 |
  |---|---|---|
  | TNR 1181 MB（.xz 43 MB） | **1912 ms / 峰值 3.22 GB** | 2247 ms / 4.27 GB |
  | Unity SC 1403 MB（.xz 50 MB） | **2277 ms / 峰值 3.42 GB** | 2671 ms / 4.77 GB |

  两个方向都赢：少一个"整份文本"的分配（峰值 −1.0~1.4 GB），也快 ~15%（不再分配+二次扫描整份
  文本，解析全程在缓存热的小窗口上做）。
- **试过并删掉**：解码线程与解析线程重叠（每半窗留 carry slack，生产线程提前解好另一半）。
  实测在三个半窗尺寸上**一致地慢 20~30 ms**（TNR 2082 vs 2062、1968 vs 1939、1957 vs 1926 ms）：
  xz 的 MT 解码本身就在吃内存带宽，和解析线程互相抢，重叠省不下时间，slack 还白削窗口容量。
  按"实测无收益就删"的规矩回退了，只留下真正的赢家（窗口本身）。
- **也试过并删掉：窗口内分段并行**（常驻线程池 + 按对象边界切段，段 0 主线程、其余派给 workers，
  逐位一致但更慢）。TNR 1.18 GB 实测：串行 1843 / 1838 / 1912 ms（16 / 32 / 64 MB 半窗）vs 并行
  2292 / 2331 / 2410 ms——一致更慢，且窗口越大差距越大。原因不是同步开销（那会随批次变小而消失）：
  **liblzma 的 MT 解码器本身已经把 10 个核吃满**（`mt.threads = lzma_cputhreads()`），再开 7 个解析
  线程只是互相抢核；这也解释了为什么"解码与解析重叠"同样没有收益。压缩输入的并行空间已经被解码器
  用掉了，除非哪天把解码降到单线程再把核让给解析（那会先亏掉 534 ms → 2549 ms 的解码时间）。

**每层结构（内存天花板在这里）**：TNR 1.18 GB / 915 万层，窗口路径峰值 **2.76 GB ≈ 300 B/层**
（含 xz 解码器的 1 GB 上限）。做掉的三块、以及各自的折算规则：

1. `tileHitsoundVolumes`：`unordered_map<int,float>` → 稠密 `vector<float>`（NaN = 无覆盖，空
   vector = 整谱无覆盖）。那张谱 457 万条覆盖 ≈ 209 MB → 37 MB：**峰值 3.22 → 3.06 GB**
   （1912 → 1860 ms）。折算 **44 B/条覆盖** —— 普通社区谱 0~2 条，等于没收益。
2. `FastAction`：内联 `std::string str`（**32 B，空的时候也占**）→ 16 位驻留 id
   （`internActionStrIn`/`actionStr`）。结构 48 → 16 B、actions 417 → 139 MB：
   **峰值 3.06 → 2.76 GB**、1860 → **1809 ms**。折算 **32 B/条 action**，所以普通谱也有收益：
   MYC 明文 618 万条 ≈ −198 MB（实测峰值 1.40 → 1.25 GB）、Tempest 31.7 万条 ≈ −10 MB
   （0.06 → 0.05 GB）、音频谱 912 万条 −292 MB。
   实现要点（两次踩坑换来的）：锁与 thread-local 缓存放 **.cpp 文件作用域**、按**驻留表地址**做
   缓存键 —— 类里不能有 `std::mutex` / `static inline thread_local` **数据成员**（MSVC 不接受，
   而且 mutex 让类不可拷贝）；别用"函数内局部静态 + `(this,strGen)` 代次"的变体（能编译、
   fixtures 全过，但 `windowBoundarySelfTest` 会报窗口路径与整份解压的 actions 不一致）；
   类名是 **`struct LevelData`** 而不是 `class`（改文件前先打印要改的确切文本）。
3. 流式窗口本身只服务压缩输入 → 对明文 0。

明文加载时间在这轮改动前后**无可测变化**（Tempest 35 ms、MYC 613 ms，3 次跑 ±几 ms）。
剩下的结构：`Tile` **24 B**（`index` 已删——它只被写入、没有任何读者，层号就是 `tiles` 里的位置；
`static_assert(sizeof(Tile) == 24)` 是护栏：1e9 层时每 8 B = 8 GB）、angleData 8 B/层（只在加载后
用于 midspin 检测）、timeline ~28 B/层。

**逐层常驻预算（这是"十亿层"目标的真正战场）**：实测 MYC = 1.69 GB / 6.77M 层 ≈ **250 B/层**，
其中大头按可动性分三类：
- ✅ **结构性安全（已做）**：
  * `Tile::index` 删除（−8 B/层）；
  * TrackVis 的 16 B/层改成**按需分配**（判据与消费方 `app/LevelScene.cpp` 的 `m_tileVisEnabled` **逐字相同**，
    那边 `if (!m_tileVisEnabled) return;` 之后才读；`tests/level_parse_test.cpp::trackVisAllocationSelfTest()`
    把"数组非空 ⟺ 消费方判据"钉死，改判据必须两处一起改）；
  * **逐实例 AABB（4×double = 32 B/实例）删除**：世界 AABB = 砖位置（`level.tiles[i].position`）+ 组内几何
    的局部包围盒（每组一份），本来就是**可推导的缓存**；现在剔除时按 SIMD 批现算进 scratch，表达式逐字相同
    （同样的 double 加法、同样喂 `CullSIMD::test4`）→ 剔除结果与像素**逐位不变**。
    实测（MYC，`--capture` 进程 peak RSS，同一二进制重复 ±0.6 MB）：**2466.3 → 2062.7 MB（−403.5 MB / −16.4%）**；
    结构推算 394.5 MB = 12.93M 个实例 × 32 B，差 ~9 MB 是分配器页开销 ✓ 对得上。
    **注意实例数 ≈ 砖 + 图标**：MYC 是 6.77M 砖 + 6.16M 图标，所以逐实例的那几块（pos/cull/可见/tileIdx/
    instVbo/colorVbo ≈ 89 B/实例）主要是**图标**在占 —— 下一步窗口化时别只盯砖。
    验收方式：`--capture` 在 3 个状态（The Moon t=1s zoom=100 / zoom=25、MYC t=30s）与改动前**逐字节相同**。
- ⚠️ **可证安全（要配套验证）**：timeline → checkpoint+重算（~28–48 B）、actions 只物化窗口内（15–70 B）、
  `Tile.angle/direction` 定点化。验收必须"整条下游逐位/逐样本对拍"（`tileStartTimes`、durations、打拍音波形）。
- ⛔ **禁止**：`Tile::position` 量化（精度规则 + 巨谱坐标范围），`angleData` 量化到 ≥1e-4 的格子
  （`Timeline.cpp` 的 `delta < 0.0001 → 整圈` 是语义阈值；`Unity.wav_rate` 实测 20 位小数、最小非零角差 4.5e-13°）。


**实例流那一条线已经做完了（2026-10）**：1 实例 = 1 砖、几何在 VS 展开、图标并入实例流
（与砖同一次 draw → 整条轨道 1 次 draw），
常驻从"每实例 ≈ 57 B × 12.93M（砖 6.77M + 图标 6.16M）"变成"**每砖 ≈ 17 B**"：
形状号 4 + 世界坐标 8 + 图标位 1 + 绘制序 4（`m_drawOrder`，见下）+ 可见位 1（TrackVis 才分配），
每形状约 130 B（`Shape` 的 12 条记录 + 包围盒）。`--capture` 进程 peak RSS 实测：

| 验收状态 | 形状数 | draw 次数 | peak RSS |
|---|---|---|---|
| angles360 fixture | 4838 | 4838 → **1** | 665 → **437 MB**（−34%）|
| Singularity | 8192 | 8192 → **1** | 1049 → **553 MB**（−47%）|
| The Moon | 196 | 196 → 1 | 513 → 513 MB |
| MYC（677 万砖）| 294 | 294 → 1 | 2073 → 1988 MB（−4%）|

**帧时间**（`--capture` 的 `render_ms`，30 帧取中位；`ADOCAO_CAPTURE_FRAMES=30`）：
145 可见实例 0.37 ms → 2509 0.67 ms → 5564 0.95 ms → **17569 1.09 ms**，而 320 fps 的预算是 3.13 ms；
第一帧另有约 35 ms 的一次性开销（形状表纹理上传 + VAO/pipeline 首次绑定），稳态里没有。
所以 canonical 表"取并集、白跑不活动 part"这个代价**实测不值得优化**（见 TODO）。

**MYC 只降 4% 是这一轮最有价值的信息**：同一个谱面走真无头 `adocao image`（只加载 + 解算、
完全不建 mesh）时 peak RSS 就是 **1560 MB** —— 也就是说"加载期常驻结构"占了那一帧的 3/4，
mesh 只占 ~430 MB。所以"十亿层"的下一仗在**加载路径**（`Tile`/`tileBPMs`/`FastAction`/timeline），
不在实例流；实例流这条线的天花板已经摸到（缺口见上面那条 ⚠️/⛔ 清单）。
`m_drawOrder` 那 4 B **不能省**：它复刻的是改造前的绘制顺序（`unordered_map` 迭代序 + 组内降序），
而深度 24 bit 下相邻砖会量化到同一个深度，顺序变了重叠处的赢家就变（实测 102 / 278 个像素）。

**actions 分块并行解析**（`parseActionRegionParallel`）：actions 数组里每个对象彼此独立，
所以先扫一遍找切分点（只做括号/字符串配对，不做字段提取，0.44 s/GB），再切 N 段并行解析，
写进**同一块缓冲的各自槽位**（先按扫描时数出的对象数一次 `resize`，末尾再由各段实际
保留数压实；没有丢弃 action 时位移为 0，零拷贝），最后 `resize` 到位。
- 必须写进同一个缓冲：早先每段各一个 vector 再合并，等于把整份 actions 写两遍，
  1.18 GB 谱面实测峰值 3.20 → 5.18 GB，在内存紧张的机器上直接翻车。
- 分段解析器（`parseActionRangeInto`）会校验"这一段正好停在下一个切分点上"，不对就整段
  退回单线程，宁可慢也不出错。
- 纯内存合成基准（全 SetHitsound 的 fat action，本机）：142 MB 文本 162.8 → 94.2 ms（8 段），
  305 MB 文本 335.9 → 201.3 ms，约 **1.7x**；其中约 2/3 是找切分点那趟扫描，所以再往上要
  么把这趟扫描也并行/流水化，要么换更快的切分点启发式。
- 真实谱面（301 MB，无换页）：整份加载 439 → 341 ms，峰值 RSS 1.43 → **1.13 GB**。
  1.2/1.4/1.5 GB 那三张上的对比测不出来：本机当时已被反复的大谱加载压出 3.4 GB swap，
  8 个线程读 1.18 GB 文本互相抢页，数字不可信（且并行/顺序都逐位一致，已核对）。
- 阈值：actions 区间在 **32 MB ~ 768 MB** 之间才自动分块。更小的用融合单遍更快；
  更大的反而更慢——1.18 GB 谱面上 2 段 900 ms、4 段 980 ms、8 段 1474 ms（而 611 MB 的
  谱面 8 段只要 80 ms），多线程读不同区段把内存带宽/局域性吃满了，加上找切分点那趟
  0.5 s 的扫描，得不偿失。1.2 GB 以上退回融合单遍解析（实测 TNR 2630 ms vs 分块 2942 ms）。
  真正的解法是让解析在小的常驻窗口上做（滑窗流水线），那时并行分段才是免费的。
- 测试钩子：`ADOCAO_PARSE_PIECES=N` 强制 N 段（`=1` 退回单线程，对拍用）、
  `ADOCAO_PARSE_DBG=1` 打印找切分点/并行解析/压实的耗时。fixture 都低于阈值，
  所以对拍工具会额外用 `ADOCAO_PARSE_PIECES=4` 再加载一遍比对（否则这条路径 CI 跑不到）。

**两条解析路径，产出必须逐位一致**：

| 路径 | 实现 | 何时用 |
|---|---|---|
| 快路径 | `LevelData::tryFastParse()`：在原文上流式扫描，不建 DOM、不复制 | 默认 |
| 旧路径 | `LevelData::parseLegacy()`：`cleanJson()` + RapidJSON DOM | 快路径复现不了时回退；也是 A/B 基准 |

`cleanJson()`（`core/level/JsonCleaner.cpp`）就是这些年攒下来的**事实格式规范**：删 `\r`、
删前置/重复/尾随逗号、给漏写逗号的老文件补逗号。里面没有任何版本分支，所以快路径**逐条复现
这些规则**而不是去猜版本号；只要遇到复现不了的东西就返回 false、交回旧路径（此时对象还没被改
动过）。已知会回退的情况：字符串里的转义或裸 CR（RapidJSON 非 in-situ 也会解转义）、
`angleData` 里的嵌套数组/非数字、非整数 `floor`、DOM 会拒绝的 token（`True`/裸词/`+1`）等。
`ADOCAO_FORCE_DOM_PARSE=1` 强制走旧路径（对拍用）。

快路径的层次（顺序即数据流）：

1. `app/FileMap.hpp` —— **mmap** 只读映射（app 层：`core/` 禁平台头，见 purity 脚本）。
   `LevelData::loadFromFile()` 仍保留可移植的 `ifstream` 读法给测试/嵌入方。
2. `scanRootMembers()` —— 只遍历根对象直接成员：`angleData` / `settings` / `pathData` 记下
   区间，`decorations` 和其它未知成员整体跳过，**`actions` 边扫边解析**（解析器自己在配对的
   `]` 处停下并回报结束位置），于是整段 actions 只走一遍。
3. `parseAngleDataRegion()` —— 整数走快路径（≤15 位十进制整数在 double 里精确），带小数点/
   指数/超长的一律 `strtod`，与旧路径 `parseAngleDataFast` 同值。
4. `parseActionRegion()` / `parseActionObject()` —— 按 key 首字符分派（谱里 action 对象常带
   一堆不关心的键），字段先收集、最后按 eventType 组装，语义等同旧路径的逐分支读取。
5. `settings` 只有几 KB，仍交给 `cleanJson` + RapidJSON；两条路径共用 `readSettings()`，
   `finishLoad()` 也是共用的收尾（pathData→angleData、tile 位置、processActions、位置偏移）。

实测（同一台机器，双方都 `-O3 -march=native`，只算 `loadFromFile`，不含 `Timeline::build`）：

| 谱面 | 大小 | 旧 | 新 | 提升 | 旧 RSS | 新 RSS |
|---|---|---|---|---|---|---|
| Tempest | 25 MB | 253 ms | 43 ms | 5.9x | 0.25 GB | 0.06 GB |
| Won't You Make a Song with Me | 301 MB | 2733 ms | 461 ms | 5.9x | 2.85 GB | 1.43 GB |
| …MYC（fat actions，89 B/个） | 611 MB | 4772 ms | 695 ms | 6.8x | 5.08 GB | 1.71 GB |
| Thousand Nights Remembered | 1181 MB | 12845 ms | 1838 ms | 6.9x | 8.12 GB | 3.34 GB |
| Unity (SoundCloud) | 1403 MB | 14795 ms | 2764 ms | 5.3x | 8.30 GB | 3.82 GB |
| Unity.wav_rate | 1503 MB | 15639 ms | 2798 ms | 5.5x | 8.80 GB | 4.14 GB |

旧路径的耗时构成（301 MB）：`ifstream+stringstream` 723 ms、`cleanJson` 1250 ms、`angleData`
strtod 260 ms、DOM+抽取 438 ms、tile 位置 27 ms、`processActions` 50 ms。所以真正的大头是
**字符串复制 + cleanJson + DOM 分配**，不是"解析和算时间线没重叠"：`Timeline::build` 只有
160~300 ms，快路径已经把 actions 和 angleData 合成一遍扫描，无须"增量算前缀时间线 + 回头重算"。

**回归测试**：`tests/level_parse_test.cpp` 对每个用例加载三遍（强制旧路径 / 快路径 / 快路径再
一次），按 13 个节（angleData、actions、settings、tiles、tileBPMs、tileHasTwirl…）比对 FNV
hash 与元素个数，逐位相同才通过；`ctest --test-dir build`。`tests/level_fixtures/` 里 40 个用例（跑起来是：36 个逐位通过 + 4 个双方都应拒绝）
每个对应 cleanJson 的一条规则，另有"双方都应拒绝"的畸形用例。也可以直接喂真实谱面：

```
./build/tests/adocao_level_parse_test ~/Documents/Charts/*/*.adofai
```

改动解析器后**必须**跑它；负向对照（故意丢 action）应当报 14 处不一致，否则说明用例没走到快路径
（这个坑真踩过：fixture 写成 `{"floor":...}` 而真实谱面是 `{ "floor": ...}`，一个多余的 `++p`
因此只在 fixture 上暴露）。


---

## Playback Engine

Direction-based angle algorithm matching ADOFAI-JS `_parseAngle`. BPM propagation via pre-indexed SetSpeed events (O(n+m)). Twirl toggles `isCW` before computing angle — affects current tile. Full rotation only when `delta < 0.0001`. Double precision throughout.

`precalculateTiming()` split into 4 phases: (0) actions-by-floor index, (1) sequential state propagation (isCW/BPM/angleDir/extraRot), (2) parallel tile geometry via `std::async` (>= 256 tiles threshold), (3) sequential prefix sum → `tileStartTimes`, (4) last-tile handling.

`startAt(wallClockSec, audioPosSec, offsetSec)` supports mid-playback start from any tile. `findTileIndex()` binary-searches `m_tileStartTimes`. Bookmark navigation (Ctrl+←/→, stopped only) uses `jumpToTile()`. Track selection: click tile while stopped, Space starts from selected tile.

`--auto-play` auto-starts playback 0.5s after loading.

## `editorOnly` 事件（2026-10-24 的坑，An-Startist 的 ADOFAN_PIXI 也踩过）

谱面里 `"editorOnly": "Enabled"` 的事件**只在编辑器里生效**，播放/渲染必须完全忽略
（Re_ADOJAS：`if (event.editorOnly && !isEditorMode) continue;`）。ADOCAO 没有编辑器模式，
所以它们一律丢弃。

踩坑经过：`parseActionObject` 里 `editorOnly` 原来**只在 rich 模式**下解析，而 `PositionTrack`
走快路径 → 标志永远是 false → MM2 的 `floor 1414 PositionTrack positionOffset [20,0]`（编辑器专用）
被当成普通事件，从那一层起整条轨道被推 **+20 世界单位**；相机锚点在 `floor 1408`（早于它），
于是相机没动、轨道右移，看起来像"相机位置/旋转中心错了"。

现在的处理：
- `parseActionObject`：`editorOnly` 不再要求 rich（快路径也读）；
- `buildAction` 收尾：`if (f.editorOnly) keep = false;`（覆盖 Twirl/SetSpeed/Pause/PositionTrack/
  ScaleRadius/AnimateTrack… 所有快路径事件）；
- `decodeSpecialEvent` 的 Flash 分支同样直接 return；
- MoveCamera / RecolorTrack / MoveTrack 走各自的 `k*EditorOnly` 标志，本来就有过滤。

**MM2 里 5 条 editorOnly 事件全是 PositionTrack**（floor 1414 / 2232 / 2650 / 3518 / 5085），
偏移分别是 [20,0] / [0,10] / [0,10] / [10,0] / [0,4] —— 全是给编辑器摆位用的。
