# `.adocao` 二进制容器规范（v1）

> 状态：**读写都已落地** —— `archive/AdocaoFormat.hpp`、`archive/AdocaoColumns.{hpp,cpp}`、
> `archive/AdocaoWriter.{hpp,cpp}`、`archive/AdocaoReader.{hpp,cpp}`、CLI `adocao pack`。
> 读取路径已接通：core 的 `sniffLevelArchive` 认 magic `ADO1` → `LevelData::loadFromBuffer`
> 经 `ArchiveBackend::decodeAdocao` 钩子分派（**core 不依赖 archive**，依赖倒置不变）→
> 填好 `LevelData` 后与两条 JSON 路径**共用同一个 `finishLoad()` 收尾**。
> 端到端验证：MYC 用 `.adocao` 与用明文 JSON 出的 64×64 PNG **逐字节相同**
> （md5 `8fdbed25f383aaabea32d0677f89eae1`，6,770,913 层）。

## 0. 为什么要有这个格式

真实谱面大到无法传播：`primer-final.adofai.xz`（68,411,078 层）**xz -9e 之后仍是 357 MB**，
未压缩 7.38 GB；MYC 明文 611 MB。目标是把这类谱面压到能分发的体量，且**逐位无损**。

先算清楚现状的账（实测）：

| 谱 | 未压缩 | xz -9e | 每层 |
|---|---|---|---|
| primer（68.4M 层，118 blocks）| 7.38 GB | 357 MB | 115.7 → 5.47 B |
| Unity.wav_rate | 1.58 GB | 49.4 MB | 143 → 4.5 B |

结论：xz 对**文本 JSON** 已经很强，单纯"换成二进制再 xz"赢不了多少（二进制 double 还丢掉了
十进制文本的公共前缀，可能更差）。真正的杠杆是**领域编码**。

## 1. 设计原则

**列式存储 + 每列按其"取值基数"选编码。** 与谱面来源无关：

* MYC 的 6,770,912 个 `angleData` 值里只有 **52 个不同值**（二元组 293/2704、三元组 682，
  最常见三元组 `(13,25,13)` 出现 850,292 次）→ 字典 + 6 bit 下标；
* action 各列的基数同样很小：`bpmMultiplier` 10、`beatsPerMinute` 9、
  `angleOffset`/`rotation`/`opacity` **各只有 1 个取值**（→ 整列省略），`floor` 单调（→ 差分）。

> 早期草案里有一个"音频谱存 PCM/int16"的机制，**已删除**：那是 `Song.adofai`
> （audio-as-chart 工具）专有的，真正的观赏谱不长那样。字典编码顺带覆盖了它
> （音频谱的取值集合就是 int16 的 65536 个 → 2 B/层），而且不需要任何"可逆性证明"。

三条硬规矩：

1. **逐位无损**：double 列按**位模式**建字典（`-0.0` / `NaN` 也原样往返）；解码出来的 double
   位模式必须与输入完全相同。**不改变任何数值语义**，所以"浮点精度规则"不受影响 ——
   我们只改"表示"，不改"值"。
2. **编码器自检**：每次编码都先算候选、再解回来逐位比对，取"能用且最小"的那个；
   任何输入最坏只是退回 `Raw`，**不会编错**。
3. **失败必须响亮**：crc 坏 / 截断 / `version` 不符 / 未知 codec / 越界下标 → 明确失败，
   **绝不静默回退**（这条被 2026-10 的两次"静默回退让测试全绿"教训逼出来）。

另有一条分工：`writerCommit` / `inputHash` **只作溯源**，**绝不作兼容闸门** ——
版本门禁只认 `version`。没有 git 的构建写全 0 并标 unknown，**不影响可读性**。

## 2. 文件布局

```
Header (76 B) | SectionEntry × sectionCount (40 B each) | padding → 8 B 对齐 | 各段载荷
```

所有整数**小端**、**定宽**；段偏移 **8 B 对齐**（为将来的 mmap 直读）；段大小与元素数一律 `u64`
（从格式层消除 2^31 边界）。

### 2.1 Header（76 B）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | char[4] | `magic` | `"ADO1"`，接入 `core/level/ByteSource.cpp::sniffLevelArchive` |
| 4 | u16 | `version` | 当前 1；**唯一的兼容闸门** |
| 6 | u16 | `flags` | 保留（0）|
| 8 | u16 | `sectionCount` | |
| 10 | u16 | `reserved` | 0 |
| 12 | u64 | `fileSize` | 全文件字节数（截断检测）|
| 20 | u32 | `headerCrc` | 覆盖"头（本字段按 0 参与）+ **整张段表**"|
| 24 | u8[20] | `writerCommit` | 写入方 git commit（SHA-1 原始字节）；全 0 = unknown。**待做：构建期注入** |
| 44 | u8[32] | `inputHash` | 源谱规范化字节的 SHA-256；全 0 = unknown。**待做** |

### 2.2 SectionEntry（40 B）

`id(u8) | codec(u8) | flags(u16) | offset(u64) | compSize(u64) | rawSize(u64) | elemCount(u64) | crc32c(u32)`

* `crc32c` 覆盖该段 `compSize` 字节（Castagnoli；`"123456789"` 的标准检验值 `0xE3069283`）。
* `codec` 目前恒为 `Raw`：段内是**自描述**的列流（每个列头带自己的 codec）。
* 未知 `id` 一律跳过（前向兼容）。

### 2.3 段

| id | 段 | 内容 |
|---|---|---|
| 1 | `Settings` | 定长记录（见 §3.1）|
| 2 | `StringPool` | `u32 count` + 每项 `u32 len + bytes`；`v[0]` 必须是空串 |
| 3 | `AngleData` | **一列 double**（§4）|
| 4 | `Actions` | `u32 columnCount(=6)` + 每列 `u64 byteLen + bytes` |
| 5 | `PathData` | `u64 len + bytes`（仅当非空）|
| 6 | `Preserved` | 未识别成员的原始字节 —— **待做**（v1 不写，今天这些本来就被丢弃）|
| 7 | `Derived` | 预计算/检查点 —— **待做**（v1 不写）|

**只存输入，不存派生数据**：`Tile::position`（全局前缀和）、`direction`、`tileBPMs`、
`Timeline` 全部由 `angleData` + `actions` 重算。

## 3. 各段细节

### 3.1 Settings 定长记录

字段顺序固定（打包确定性）：`version(i32) | bpm(f32) | offset(f32) | countdownTicks(i32) |
zoom(f32) | rotation(f32) | relativeTo(u16 池下标) | position[2](f32) | hitsound(u16) |
hitsoundVolume(f32) | trackColor(u16) | secondaryTrackColor(u16) | backgroundColor(u16) |
stickToFloors(u8) | planetEase(u16) | trackDisappearAnimation(u16) | trackAnimation(u16) |
beatsBehind(f32) | beatsAhead(f32)`

* `f32` 按**位模式**原样存（float 列必须逐位无损）。
* 未知 settings 键与今天两条解析路径的行为一致（本来就只读已知字段）；无损回写靠 `Preserved` 段。

### 3.2 StringPool

**`actionStrTable` 的原序必须保留**（`FastAction::strId` 就是它的下标）；settings 用到的字符串按
固定顺序追加到末尾。`v[0]` 是空串（`strId = 0` 的约定）。

### 3.3 Actions（列式）

固定 6 列，顺序：`floor(int64) | type(u32) | strId(u32) | flag(u32) | val1(u32) | val2(u32)`。

* `floor` → `encodeIntColumn`（单调 → `DeltaVarint`）；
* `type` / `strId` / `flag` → `encodeU32Column`（小枚举 → `Dict`/`BitPack`/`Const`）；
* `val1` / `val2` 是 **float 的位模式**当 `u32` 存（逐位无损；`bpmMultiplier` 那种只有几十个取值
  → `Dict`）。

每列前缀 `u64 byteLen`，于是读取方可以跳过不关心的列（`u64` 允许单列 > 4 GB）。

## 4. 列编码（Codec）

列头（20 B）：`codec(u8) | bits(u8) | flags(u16) | count(u64) | aux(u64)`；
`aux` = 字典项数（`Dict` 用），其余编码为 0。

| codec | 值 | 载荷 | 用在 |
|---|---|---|---|
| `Raw` | 0 | 定宽原始（double/int64 8 B，u32 4 B）| 高基数兜底 |
| `Dict` | 1 | `aux × 8 B`（double/int64）或 `aux × 4 B`（u32）字典 + 位打包下标 | 基数小（主编码）|
| `DeltaVarint` | 2 | 首值 zigzag varint，其后 zigzag(相邻差) varint | 单调列（`floor`）|
| `Const` | 3 | 一个值（8 B / 4 B）| 整列同值 |
| `BitPack` | 4 | 位打包（位宽 = `ceil(log2(max+1))`）| 小枚举/位标志 |
| `JsonPassthrough` | 5 | 原样文本 | 保留（诊断/未知段）|

位打包是**低位在前**、1..32 位/值。`Dict` 的下标与 `Raw/Const` 的载荷都按 `count` 读取，
任何越界/截断都返回 `false`。

## 5. 实测

`adocao pack <谱> <out.adocao> --codec-report` 打印每列的编码、元素数、朴素/编码后字节、
字典项数与位宽。

**MYC（6,770,912 层 / 6,181,981 action）：611.18 MB → 19.58 MB = 3.033 B/层（31×，0 换页）**

| 列 | 编码 | 每值 | 字典 |
|---|---|---|---|
| `angleData` | Dict | 0.7501 B | 52 项 / 6 bit |
| `actions.floor` | DeltaVarint | 1.0000 B | — |
| `actions.type` | Dict | 0.2500 B | 4 项 / 2 bit |
| `actions.strId` | Const | 0.0000 B | 1 项 |
| `actions.flag` | BitPack | 0.1250 B | 1 bit |
| `actions.val1` | Dict | 0.7500 B | 40 项 / 6 bit |
| `actions.val2` | Dict | 0.3750 B | 7 项 / 3 bit |

`angles360`（5,564 层 / 14 action）→ 0.02 MB（3.168 B/层，`angleData` 字典 1,167 项 / 11 bit）。

### 5.1 音频谱（`Song.adofai` 那一类）：primer 只赢 1.10× —— **按停手线停止**

**primer（68,411,077 层 / 68,023,438 action）：7.38 GB JSON → 324.36 MB**（对明文 22.8×），
但**对 `xz -9e` 的 357.12 MB 只有 1.10×** —— 低于本格式的停手线（<1.5× 就停），
所以**不再为这一类继续加专用机制**。逐列一看就明白为什么：

| 列 | 编码 | 每值 | 说明 |
|---|---|---|---|
| `angleData` | **Const** | 0.0000 B | 6,841 万个角度**全部相同** |
| `actions.floor` | DeltaVarint | 1.0000 B | 68 MB |
| `actions.type` / `strId` / `flag` / `val2` | **Const** | 0 | 各列常量（共 96 B）|
| **`actions.val1`** | **Raw** | **4.0000 B** | **272 MB = 全文件的 84%** |

结论与教训：

* 这张谱的**音频数据在 `actions.val1`（每层的音量）**，而不是我早先以为的 `angleData`；
  6,841 万个值几乎互不相同 → 字典不划算（`Dict` 候选算出来比 `Raw` 还大）→ `Raw` 胜出。
  也就是说这 272 MB 是**实打实的 4 B/事件的信息量**，通用手段压不动。
* 早先"从中段采样估计字典基数"的启发式**对它完全无效**：`angleData` 是整列常量，
  任何采样点看到的都是同一个值（这也解释了当时连着两次量出"去重 1"）。
* **向量化的收益边界**：观赏谱（MYC）31×，音频谱 1.10× —— 差别的来源不是"格式好不好"，
  而是**数据本身的基数**。格式已经把能省的都省了（其他六列合计 68 MB，理论上都来自 floor 的差分）。

### 5.2 载荷列的"共享"浪费 + 段级压缩（Twirl 专项，实测）

**问题**：`Actions` 的 6 列是**所有事件共享**的 —— 一条没有载荷的事件（典型是 `Twirl`）仍然要在
`val1`/`val2` 的下标流里占位。MYC 实测：

* actions **99.6% 是 Twirl**（6,155,053 / 6,181,981）；Twirl 的 `val1`/`val2`/`strId` 非零比例**全是 0%**；
* Twirl 现在占 floor 5.87 MB + type 1.47 MB + **val1 4.40 MB + val2 2.20 MB** = 13.94 MB；
* **按类型稀疏化载荷列可省 6.60 MB / 19.58 MB = 33.7%**（val1 4.40 + val2 2.20）。

**改法（保持事件顺序、不改语义）**：`val1`/`val2`/`strId` 改成**按类型稀疏**的列 ——
每列只写"真正使用它的类型"的那些事件，列头加 `u64 usedCount`；读取方按固定的"类型 → 用哪些列"
表，遇到不用的类型就跳过（值取默认 0）。事件顺序完全不变，因此 `processActions` 的语义逐位不变。

**更大的杠杆是段级压缩**（P4）—— 实测 `.adocao` 再 xz -9e：

| 谱 | 明文 | `.adocao` | + xz -9e | 说明 |
|---|---|---|---|---|
| MYC（615 万连续 Twirl）| 611.18 MB | 19.58 MB | **~11 KB** | 极度规则，**不代表一般谱面** |
| The Moon | 15.60 MB | 1.25 MB | **<10 KB** | 同上 |
| angles360（人工合成）| 0.03 MB | 0.02 MB | 0.01 MB | 1167 个角度 → 压不动多少 |
| **primer（音频谱）** | 7.38 GB（xz-JSON 357 MB）| 324.36 MB | **195.43 MB** | **对 xz-JSON 1.83×**，越过停手线 |

**于是修正 §5.1 的停手结论** ✗：pimer 的"只有 1.10×"**只在"不压缩"的前提下成立**；
段级压缩后是 1.83×，**P4 应当提上来做**。注意 MYC/The Moon 的 KB 级结果不可外推
（那是"数据几乎完全可预测"的极端情形）。

**下一步（音频谱）**：`DeltaF32` 列编码（对**数值**相邻差差分再压）。依据：primer 的 `val1`
按**位模式** `Raw` 存（324 MB）时，仅 xz 就能到 195 MB（1.66×）→ 说明那些 float 里有真实结构，
而位模式表示把它丢掉了。**先实测数值差分后的熵再决定**。

### 5.3 赫兹谱（audio-as-chart）的"隐式"编码能省多少（实测）

用户指出"它的地图基本上全是直线" —— 这正是赫兹谱的签名：**角度就是采样值**，被量化到一个很小的
调色板。实测 MYC（6,770,912 层 / 6,181,981 action）：

* 调色板 **52 项**，前 4 名合计约 **80%**；**严格轴对齐（90° 的整数倍）占 47.1%**，其余多为 45° 系列
  → 在整图尺度上"斜线也是直线"，所以视觉印象与数据一致；
* "**每层一条 Twirl 且 floor 连续**"覆盖 **90.42%**（5,589,469 / 6,181,981）；例外只有 **592,512** 条
  （其中 565,584 条是"该层没有 Twirl"的缝隙，另有 PositionTrack 26,480 / SetSpeed 315 / Bookmark 133）；
* 于是这一族最省的形状是：角度调色板下标 **4.84 MB** + Twirl **位图 847 KB**（位图天然表达那 9.6%
  的缝隙）+ 非 Twirl 事件显式 **~0.2 MB** ≈ **5.9 MB**（现在是 19.58 MB，**−70%**），再 xz 就是 KB 级。

**风险与兜底**：位图化会改变"同一层里 Twirl 与其它事件"的原始交错顺序 → 顺序定义（例如"同层 Twirl
先于显式事件"）必须与 `processActions` 的语义一致，并由 `archiveRoundTrip` 的 **13 节逐位比对**兜住。
换句话说：这条优化是**可安全尝试**的，因为一旦顺序错了，逐位比对立刻会红。

**外推的边界**：以上是"赫兹谱 + 极规则"这一族的结果。手工谱请以 `angles360`（1,167 个角度、
几乎压不动）为参照，不要拿 MYC 的 KB 级去预期。

### 5.4 游程（RLE）在哪些列上真的有用（实测，含一个反直觉结果）

用户给了四张赫兹谱的实拍图：竖直**锯齿** / 45° **长直线** / 水平线 + **整圈**砖 / **L 形**拐角。
它们对应"相对角"列的三种结构：**同值长游程**（直线 / 整圈）、**短周期交替**（锯齿）、
**两段长游程拼接**（L 形）。实测各列的游程（MYC / The Moon）：

| 列 | 游程均值（MYC / The Moon）| 结论 |
|---|---|---|
| `angleData` | **1.0 / 1.1** | **RLE 反而更差**（5.08 MB → 11.58 MB）—— MYC 是锯齿型（音频速率 ±90 交替）|
| `type` | 117 / 15.4 | RLE 极佳（MYC 1.55 MB → 66 KB，23×）|
| `flag` | 11,179 / 1,023 | RLE 极佳（773 KB → 839 B，920×）|
| `val1` | 118 / 15.4 | RLE 极佳（4.64 MB → 92 KB，50×）|
| `val2` | 62,444 / 2,434 | RLE 极佳（2.32 MB → 166 B，14,000×）|
| `strId` | 全列一个值 | 已经是 `Const`（24 B）|
| `floor` | 差分后游程均值 1.0 | 需要"**对差分流再取游程**"：6.18 MB → 十几字节 |

**要点**：地图上的"直线"= **相对角恒定** = `angleData` 的**长游程**；但 MYC 恰好是**锯齿**那一型
（游程均值 1.0），所以 RLE 对它的角度列无用。也就是说**同族内也分两型**（直线型 / 锯齿型），
而"每列算出候选再取最小"的设计**两种都能自动选对**，不需要按谱面类型分派。

**要做的事**：加一个 `Rle` 列编码（值 + 游程长度 varint），并给单调列加
`DeltaRleVarint`（先差分再对差分取游程）。判据必须是"**实测字节数最小**"，
而不是按列名/类型硬编码 —— 否则直线型和锯齿型总有一边会选错。

### 5.5 这类谱的真实信息量：单元 × positionTrack 摆放（实测）

用户指出这类谱"都是拿这些单元拼起来的，成千上万个拿 positionTrack 排列"。实测 MYC 证实：

| 度量 | 实测 |
|---|---|
| 周期 8 的"单元"种类 | 846,363 个窗口里只有 **568 种**，**前 4 种占 75.7%** |
| 周期 2 的"单元" | **223 种**，最常见一种占 24.2%，前 4 种占 72.8% |
| `PositionTrack` | **26,480 条**；x 只有 **19 个不同值**、y 只有 **7 个**（x∈[-9,15]、y∈[-8.7,2]）= 小网格 |
| 摆放数据本身 | **0.20 MB**（26,480 × 8 B）；按"19×7 网格调色板"存约 **26 KB** |

**结论**：这类谱的真实信息量只有**几百 KB** —— 它本质是一段程序："在网格 (x,y) 上放第 U 号单元"，
重复 26,480 次；角度流是从几百个 motif 里挑的。其余全是生成出来的。

**对编码的含义**：
1. `PositionTrack` 的 val1/val2 天然落进 `Dict`（19 / 7 个取值 → 5 + 3 bit）—— 已有编码就够了；
2. 角度流需要**比 RLE 更贴形状**的候选：`Motif`（周期 p + motif 字典 + motif 下标流）。
   注意 RLE 对这种"短周期交替"无能为力（游程均值 1.0），而 `Motif` 正是为它准备的；
3. **判据仍然是"实测字节数最小"**：`Dict`/`Rle`/`Motif`/`Raw` 四个候选一起算，谁小用谁 ——
   直线型谱会在 angleData 上选 RLE，锯齿/单元型会选 Motif，人工谱会选 Dict 或 Raw。

**不手搓 motif 也可以**：xz 已经能吃到这类长程重复（实测 19.58 MB → ~11 KB）。
但 `Motif` 的价值在于**不依赖"完全可预测"**：真实的赫兹谱若 motif 顺序半随机，xz 可能只有 2~4×，
而 motif 编码能到 ~10~20×。所以两者都留着（段级压缩 + motif 候选），谁小用谁。

### 5.6 三个版本的对比：xz 已经吃下这一族，Motif 暂缓（实测）

同一张谱的三个版本（MYC / v300 = 同谱的"瘦 actions"版 / v282 = no three planets）：

| 谱 | 层数 | 调色板 | angleData 列 | **xz(角度下标流)** | 占比 | p=8 单元 | p=2 单元 | PT | PT_x | PT_y |
|---|---|---|---|---|---|---|---|---|---|---|
| MYC | 6,770,912 | 52 | 5,078,620 B | **3,652 B** | **0.1%** | 568 | 223 | 26,480 | 19 | 7 |
| v300 | 6,770,912 | 52 | **与 MYC 逐字节同长** | **3,652 B** | 0.1% | 568 | 223 | 26,480 | 19 | 7 |
| v282 | 6,377,688 | 52 | 4,783,702 B | **3,600 B** | 0.1% | 569 | 221 | 26,480 | 19 | 7 |

**结论**：
1. 这一族的重复性**完全落在 xz 的能力范围内**（角度流 0.1%）→ 专门写 `Motif` 编码**不值得**
   （那会是一个更差的 xz 复制品）。§5.5 里提的 Motif 因此**暂缓**。
   **可测的复活条件**：若某张谱的角度列 xz 后占比 **> 10%**（即 xz 吃不下），再回来做 Motif。
2. **JSON 的啰嗦程度不影响 `.adocao`**：v300（300.9 MB JSON）与 MYC（611 MB JSON）压出来几乎一样大
   （19.26 / 19.58 MB）—— 因为二进制存的是**解析后的结构**（16 B/action），
   JSON 层的字段名冗余被彻底消掉了。
3. 因此实现顺序（按实测收益）：**段级 xz（P4）** → 便宜的列级优化（`Rle`、稀疏载荷列、
   `PositionTrack` 走 `Dict`）→ `Motif` 只在触发条件满足时再做。

### 5.7 到底是 LZMA2 的哪一部分在干活（参数扫描 + 打乱对照）

对象：MYC 的角度下标流单独导出（5,078,184 B = 6,770,912 个 6-bit 下标，字典 52 项）。

**对照实验（只改"顺序结构"）**：

| 变体 | xz -9e |
|---|---|
| 原始 | **3,148 B**（0.062%）|
| 4 KiB 块内打乱（毁长程、留局部）| 2,170,989 B（42.8%）|
| 全局打乱（直方图不变、重复全毁）| 2,756,954 B（54.3%）|
| 纯直方图熵下界（52 符号，4.21 bit/字节）| ≈ 2,612 KB |

→ **相差约 880×**：靠"符号分布偏斜"几乎不省（下界本来就 2.6 MB），**省下来的全部来自顺序结构**。

**参数扫描（只改一个 LZMA2/LZMA1 参数）**：

| 变体 | 输出 | 相对基线 |
|---|---|---|
| `preset=9e`（基线）| 3,148 B | — |
| `nice=8`（匹配最长只找 8）| 4,004 B | **+27%** |
| `lc=0,lp=0,pb=0`（关掉字面量上下文）| 3,088 B | **−2%**（完全无关）|
| `preset=1`（1 MiB 字典 / hc4）| 3,965 B | +26% |
| `dict=4KiB`（字典只留 4 KiB）| 4,634 B | **+47%** |
| `depth=4`（搜索只找 4 步）| 3,269 B | +4% |
| LZMA1 vs LZMA2（容器开销）| 3,127 vs 3,148 B | 21 B |

**结论**：**LZMA2 这一层没有贡献**（只多 21 B）；干活的是里面的 **LZMA1 LZ77 匹配编码器**，具体是
（a）**`rep0`~`rep3` 重复距离**（周期 motif 几乎免费的原因）＋（b）**可达距离超过 4 KiB 的匹配**
（重复是长程的）＋（c）**很多短匹配**（不是"一个超大周期"）。字面量上下文模型、匹配搜索深度、
LZMA2 分块/状态重置全都无关。这也解释了为什么**不需要写 `Motif` 编码**：
**LZ77 的 rep-match 本来就是一个自适应单元字典**，而且自动适配"单元在哪里重复"。

**对 P4 的实际结论**：
1. 字典留大（≥64 KiB，即 `preset=9e` 档）；
2. 按列分段压缩安全（单列重复距离只有几十 KiB，自己一段更密）；
3. **不要为字面量模型做字节整形**（转置/字节平面拆分无效）；
4. 可以用快预设（`preset=1` 也只要 3,965 B，对 0.062% 的基线而言 +26% 无意义）→ 解码更快。

## 6. 测试

* `tests/adocao_columns_test.cpp`（ctest `adocao_columns`，19 项）：逐位往返（含 NaN/次正规/±0/极值）、
  编码选择（Dict/Const/DeltaVarint/BitPack）、CRC32C 已知检验值，以及 **5 个负向对照**：
  截断 1 字节 / 空输入 / `count` 说谎 / 未知 codec / 越界下标 —— 都必须**失败**。
* `archiveRoundTrip`（在 `tests/level_parse_test.cpp` 里，**已接**）：每个 fixture 都会被
  pack 成 `.adocao` → 走**完整的** `loadFromBuffer`（含 magic 分派与 `finishLoad`）→ 与明文加载
  **13 节逐位比对**；并断言**可复现性**（同一输入两次 pack 必须逐字节相同）与两个负向对照
  （截断 / 改坏 settings 载荷 → 段 crc32c 必须抓住）。这些负向对照是**自证**的：
  若 crc 没起作用，`loadBuffer(...).ok` 会是真，用例立刻报错。

## 7. 待做清单

1. ~~**读取方**（P1-⑥）~~ **已完成**：`LevelArchiveKind::Adocao` + magic 认领（magic 的唯一来源在
   `core/level/ByteSource.hpp`，archive 侧 `static_assert` 钉住）+ `ArchiveBackend::decodeAdocao`
   钩子 + `AdocaoReader::unpackLevel`（段表/headerCrc/逐段 crc32c 全校验，坏数据明确失败）。
   待做的小尾巴：`WindowSource` 对 `.adocao` 的"逐段交付"语义（目前是整份读进内存再解，
   文件本身只有几十到几百 MB，所以不急）。
2. `writerCommit` / `inputHash`：构建期注入 git hash + 源谱 SHA-256。
2b. `DeltaF32` 列编码（float 按**数值**差分），专治 primer 那种"每层一个采样"的列 ——
   先实测差分后的熵再决定，不要凭估计上。
3. **压缩**：段可选 xz/zstd 包装（复用现有 backend，不引入新库）；列流先做可选 delta 滤波。
4. `Preserved` 段（供 ADOCAO-E 无损回写）。
5. **零拷贝**：段布局与内存布局一致时（`Raw` 且对齐）直接 mmap/memcpy。
6. `Derived` 段：预计算/检查点（与 Timeline 检查点化共用设计）。
