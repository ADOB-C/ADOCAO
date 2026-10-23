# `.adocao` → `.adofai` 导出规范

> 这份文档写给**其他实现者**（当前只有 `../ADOFAI.Lib`）。ADOCAO 本体不提供导出功能：
> 这里描述的是"怎么把容器还原成明文"的契约，用来验证容器是无损的，也用来让别的项目复用同一套语义。
> ADOFAI.Lib 侧用符号链接指到本文件，避免两份规范漂移。

## 0. 前提

只有同时满足下面两条的容器才能完整导出：

1. 有 `EventSchema`(14) + `EventColumns`(15) 两段；
2. 事件列里包含**全部**事件 —— 即打包时 `v7IncludeLegacy = true`（CLI 里已不再暴露，
   测试直接用 API）。默认包对 v6 段已承载的类型做了去重，那些事件只在 v6 段里。

不满足就**明确报错**，不要输出"看起来能播"的残缺谱面。

## 1. 数据从哪来

| 输出字段 | 来源 |
|---|---|
| `settings` | `Preserved`(6) 段：打包时捕获的 settings 原文，**原样输出**。不要逐键重建 —— ADOFAI 的 settings 有 50+ 个键，重建必然丢字段 |
| `angleData` / `pathData` | 同名段（`pathData` 存在时优先用它） |
| `actions` | v7 事件里 `source = Actions` 的那些，**保持容器内顺序** |
| `decorations` | v7 事件里 `source = Decorations` 的那些，同样保持顺序 |

事件在两段里的布局：`typeIds[event]` 给出类型，类型的字段（`SchemaBuilder` 的 `TypeDesc`）给出
`(name, kind)`；每字段一列，值按"同类型内的第几个事件"消费。

## 2. 值的三种状态

每个字段对每个事件都有三种状态，导出时必须区分：

- **missing**（`present` 位为 0）→ 这个键**不写**；
- **null**（`nulls` 位为 1）→ 写 `null`；
- **有值** → 按下表的 kind 写。

`Pair` 的两个分量各有独立的 null 位（`position: [null, 3]`），分量也可以是字符串
（`startTile: [3, "ThisTile"]`）。

## 3. kind → JSON

| kind | 写法 |
|---|---|
| `Bool` | `true` / `false` |
| `Int` | 整数（int64） |
| `Float` | 数字；用最短往返表示（`%.17g` 再裁尾零），必须能 `strtod` 回来逐位相同 |
| `GridFloat` | 数字 = `int / 10^gridK`（`gridK` 在字段描述里）；带 `kGridAsDouble` 时按 double 原样 |
| `Enum` / `Tag` / `String` | 字符串（字典展开） |
| `Color` | 字符串；`subFlags & 0x0C` 决定是否带 alpha（`rrggbb` / `rrggbbaa`，不带 `#`） |
| `Pair` | 两元素数组；分量按各自的数字/字符串形态写；`kPairRaw` 时整对是原始 JSON，直接透传 |
| `Nested` / `RawJSON` | 原始 JSON 文本，直接透传（不要重新序列化） |

**数字必须逐位无损**：容器里存的是原始 double，导出再读回来必须得到同一个 double。
格式化用"能唯一还原的最短表示"，不要用固定精度（`%.6f` 会把 `0.1` 变成 `0.100000` 再读成别的值）。

## 4. 顺序与格式

容器里**不存**原始 JSON 的空白/键序；导出器自己决定格式。当前参考实现（`tests/AdocaoJsonWriter.cpp`）：

- 根对象键序：`settings`、`angleData`（或 `pathData`）、`actions`、`decorations`；
- 每个事件：`eventType` 在最前，其余字段按 schema 的字段顺序；
- 紧凑输出，不加缩进。

只要**往返稳定**就行：同一容器导出两次得到同一份字节，导出再打包得到同一个容器
（见下面的验收）。

## 5. 验收（必须跑）

```
容器 A --(本规范)--> 明文 --> 重新打包 --> 容器 B
要求：A == B（逐字节）
```

这条比"逐字段比对"更强：任何顺序、格式、精度、空值的偏差都会让字节不同。
参考实现与用例：`tests/AdocaoJsonWriter.cpp` + `tests/adocao_v7_roundtrip_json_test.cpp`
（ctest 名 `adocao_v7_roundtrip_json`，覆盖 4 张测试谱）。

## 6. 常见坑（都真踩过）

1. **`endTile`/`startTile` 的第二个分量是字符串**（`"ThisTile"`）。按数字读会变成 `7.3e18`。
2. **`separateCountdownTime`** 在 settings 记录的**最末尾**（后加的字段），旧读者忽略尾部字节即可。
3. **settings 不能逐键重建**：必须用 `Preserved` 段的原文。
4. **缺失 ≠ null**：`"zoom": null` 与没有 `zoom` 键在语义上不同（前者显式空，后者用默认值）。
5. **颜色 alpha**：8 位十六进制才是带 alpha 的，别把 `rrggbbaa` 当成 `aarrggbb`。
