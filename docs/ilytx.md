# `.ilytx` —— Re_ADOJAS 的自研谱面容器（ADOCAO 的读取支持）

Re_ADOJAS（作者 Xbodwf，本机克隆 `~/Repos/Re_ADOJAS`）除了 `.adofai` 文本，
还导出自研的 **`.ilytx`**：把一个谱面的解析结果直接存成二进制，导入时跳过整份 JSON
扫描与相对角状态机。权威实现是那边仓库的 `src/lib/ilytx/`（`ilybin.ts` / `tar.ts` /
`xz.ts` / `types.ts`），本文件是**给我们自己看**的规范摘要 + 我们读到什么程度。

我们**只读不写**：要让 ADOCAO 导出这个格式得先跟对方的规范对齐，不在这一版范围内；
ADOCAO 自己的容器是列式的 `.adocao`（见 `docs/adocao-format.md`）。

## 1. 容器：xz(tar(成员…))

```
.ilytx  =  xz 压缩的 ustar tar，成员：
             manifest.json     {format:"ilytx", version:2, generator, created, tileCount, …}
             level.ilybin      → 见 §2（谱面核心）
             audio/<song>      内置音频（可选）
             decor/<img>       内置装饰图（可选）
             bg/<img>          内置背景图（可选）
```

* 成员名是**全路径**（`audio/x.ogg`）；长的走 ustar 的 `prefix` 拆分。
* **成员顺序不固定**（Re_ADOJAS 把 `level.ilybin` 放在最后，但读取方不该依赖位置）——
  我们按名字找 `level.ilybin`。
* 与 `.adofai.xz` 的关系：压缩层**完全一样**（都是 xz），区别只在解压后的载荷
  （tar vs JSON 文本）。所以"是不是 ilytx"只能**解压之后**才知道：
  core 的 `isTarPayload()` 就是那个判据（ustar magic 在偏移 257）。
  推论：改过扩展名的 `.ilytx` 照样能打开（与 `.adofai.xz` 一样的规矩：只看 magic）。

## 2. `level.ilybin`（他们叫 v1）

```
header:  "ILYB" | u8 version=1 | u8 tileMode | u16 sectionCount | u32 tileCount   （12 B）
section: u8 id | u32 byteLength | payload                       （按序排列，长度前缀）

tileMode: 0 = 对象模式（源数据是 f64）/ 1 = 紧凑模式（源数据是 f32）

section 内容：
  1 settings     UTF-8 JSON —— 等价于 .adofai 里**除** angleData/actions/decorations 的顶层对象
  2 direction    FloatSection —— 就是 `angleData` 的**值**（含 999 中心块、555/666/777/888 特殊砖）
  3 angle        FloatSection —— 相对角（**纯派生量**；v2 起写入端不再产出）
  5 twirl        ZigZag-varint 差分 —— 累计 Twirl 计数（也是派生量）
  6 actions      UTF-8 JSON [{floor, …}]
  7 decorations  UTF-8 JSON [{floor, …}]
  8 extraProps   UTF-8 JSON [[floor, {…}]]（仅非空时出现）
```

`FloatSection` 三种编码（第 1 字节是 kind）：

| kind | 布局 | 说明 |
|---|---|---|
| 0 | `u16 LE scale` + 逐项 `ZigZagVarint(Δ(round(v*scale)))` | 最省；scale ∈ {1,10,100,1000} |
| 1 | `tileCount × f64 LE` | 无损兜底（缩放整数对不上时） |
| 2 | `tileCount × f32 LE` | 紧凑模式原生精度 |

## 3. 我们怎么读（关键决定：**只翻译，不另建解析链**）

`archive/Ilybin.cpp` 解 ilybin，`archive/Ilytx.cpp` 拆 tar，然后**拼回等价的 `.adofai`
明文 JSON**：

```
{ "angleData":[direction…], "actions":[…], "decorations":[…], <settings 的成员> }
```

交给 core **原有的明文路径**（快路径 / DOM 回退 / `finishLoad`）——于是 ilytx 与 .adofai
的语义、像素、对拍断言天然一致，不存在"两条路各说一套"。

* **settings 用 RapidJSON 过一遍**：别人的导出物不能假设干净。有 `angleData`/`actions`/
  `decorations` 同名字段就删掉（否则会出现重复键），没有冲突时**原样透传文本**。
* 数字用 `std::to_chars`（**最短往返**、与 locale 无关）：`%.17g` 也行但更长，`%g` 会受
  `LC_NUMERIC` 影响（小数点是逗号就写坏 JSON ✗）。
* 派生段**一律不用**：section 3（angle）与 section 5（twirl）是 `(direction, twirl 事件)`
  的纯函数，core 自己按 `angleData` + actions 里的 Twirl 事件重算——这正是 .adofai 生态的
  既有做法（`angle` 从不落盘）。section 5 只做**完整性校验**（能完整解出来，否则算坏文件）。
* section 8（`extraProps`，逐层附加属性）**忽略**：ADOCAO 目前没有对应概念，读了也没处放。

## 4. 流式路径：ilytx 只能整份解压

`.adofai.xz` 走的是"半窗流式解压 + 边解边解析"（见 `docs/level-loading.md`）。
ilytx 的载荷是 tar，不是 JSON，所以 `WindowParser` 在首块就发现 `isTarPayload()` 为真、
**主动让路**（日志：`ilytx（tar 容器）只能整份解压`），由整份解压那条路接手。

代价是 ilytx 会**先试一次窗口、再整份解压**（多一次解压）。当前不优化：ilytx 的体量在
几十 MB 内，xz 解压是亚秒级；真到 2³¹ 层那种规模，正确做法是先用 `adocao pack` 转成
列式容器（那时才是 seek / 内存与 n 解耦的形态）。

## 5. 已知取舍

* **中间文本**：翻译成明文会多一份 ~10–15 B/层 的字符串（5564 层约 26 KB；677 万层约
  100 MB 量级）。这是"复用唯一解析路径"的代价，换的是一条没人敢维护的平行解析链。
* **只认 xz 压缩的 `.ilytx`**：裸 tar（没压缩）不在 `sniffLevelArchive` 的识别范围内——
  Re_ADOJAS 的导出**总是** xz 压缩（`xz.ts`），所以先不认。
* **内置音频/装饰图不抽取**：`audio/…`、`decor/…` 成员目前跳过（音乐仍按原方式：CLI
  `--music`、向导里选或同目录自动检测）。要支持得先决定"抽到哪、什么时候清"。
* **不解压多重 xz 流**：Re_ADOJAS 的写入方会把 tar 分片并行压成**多个拼接的 xz 流**；
  我们用的 liblzma 解码器本来就吃拼接流（`LZMA_CONCATENATED`，见 `archive/LevelArchive.cpp`），
  所以读得到——但这条要有真实的大文件回归才敢说"验过"（当前样本是单流）。

## 6. 测试样本（**用他们的编码器生成**）

`tests/gen_ilytx_fixtures.mjs` 直接 `import` 对方仓库的 `ilybin.ts` / `tar.ts` 来编码 ——
绝不在这边手抄格式再自产自销（"自己写、自己读"的对拍永远绿，AGENTS.md 记过这个教训）。
需要本机有克隆（`RE_ADOJAS_DIR`，默认 `~/Repos/Re_ADOJAS`）：

```bash
node tests/gen_ilytx_fixtures.mjs      # 重新生成 tests/level_fixtures/ilytx_*
```

产物与断言（`tests/ilytx_test.cpp`，ctest `ilytx`）：

| 样本 | 断什么 |
|---|---|
| `ilytx_ref.adofai` + `ilytx_object.ilytx` | angleData / actions / settings / 每砖位置 **逐位相同** |
| `ilytx_frac_ref.adofai` + `ilytx_frac_object.ilytx` | 小数角度也逐位相同（`to_chars` 最短往返够用） |
| `ilytx_frac_compact.ilytx` | f32 量化误差 **>0 且 <1e-3 度**（>0 是为了证明这条用例真走到了 f32） |
| `ilytx_missing_level.ilytx` / `_truncated` / `_badmagic` | **必须干净失败**（不许静默产半份谱面） |

端到端另外验过：`adocao image` 出图，`.ilytx` 与参照 `.adofai` 的 PNG **逐字节相同**
（5565 层，1024×1024）；`adocao pack` 把 `.ilytx` 转成 `.adocao` 后出图也逐字节相同。

## 7. 诊断

* `ADOCAO_ILYTX_DUMP=<文件>`：把"翻译出来的明文"落盘。遇到 `RapidJSON parse error at
  offset N` 时，用这个文件按偏移看现场（就是靠它定位到"settings 成员少了冒号"那次）。
