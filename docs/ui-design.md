# 向导 UI 设计（统一栅格与组件）

这份文档定义启动器/向导的**唯一一套**版面规则。页面（`app/wizard/Page*.cpp`）只许调组件，
不许自己写坐标、宽度、行距。

## 1. 为什么要定死

到 2026-10-16，四个配置页各写各的常数：标签列都在 24，但控件列前后的宽度写成 140（Trail 的
DragFloat）/160（Colors 的 ColorEdit3）/298（Graphics 的 Combo）/300（Music 的 InputText），
右留白于是 24、48、72 三种；行距靠每行手写 `ImGui::Spacing()`，有的行有、有的没有。结果是
**同一类行在不同页对不齐**，而且改一处（比如页标题换 1.4x 字体）就会在最长那页压到按钮上 ✗。

## 2. 栅格（窗口 520x420 逻辑点）

```
│◀ 24 ▶│◀── 标签列 150 ──▶│◀────── 控件 322 ──────▶│◀ 24 ▶│
24     标签 x=24          控件列 x=174              右边距 = 496
       子项 x=48          子项控件列 x=198          控件右边缘 = 496（= winW-24）
```

| 量 | 值 | 常量（`WizardChrome.hpp`） |
|---|---|---|
| 页面左右留白 | 24 | `kFormPadX` |
| 标签列宽 | 150（→ 控件列 174，子项 198） | `kFormLabelW` / `kFormSubIndent` |
| 控件宽 | `winW - kFormPadX - 控件列 x`（322，子项 298） | `formCtrlW()` |
| 行距 | 8 | `kFormRowGap` |
| 同一行内控件间距 | 8 | `kFormGap` |
| 行内小按钮（Choose） | 62 | `kFormButtonW` |
| 与复选框同行的内联控件 | 150 | `kFormInlineW` |

**关键一条**：所有东西的**右边缘都在 496** —— 进度条、分隔线、每个控件、行内按钮的右边。
这是"看起来统一"的来源，比任何颜色/圆角都重要。

## 3. 字号三档（`app/FontSetup.hpp`）

| 用途 | 倍率 | 实测字号 | CJK |
|---|---|---|---|
| 正文（控件、标签） | 1.0x | 12.0 | ✓（合并了中日韩源） |
| **页标题**（`drawConfigHeader`） | 1.4x | 16.8 | ✗（只挂拉丁） |
| 大标题（Welcome 的 "Welcome to ADOCAO"） | 1.8x | 21.6 | ✗ |

比正文大一点、又明显小于大标题 —— 这就是页标题那一档的全部含义。改字号时记得
`tests/font_cjk_test.cpp` 会断言三档递增。

`PushFont` 在 ImGui 1.92 起 **size 必填**：要"用字体自己的字号"传 `font->LegacySize`；
传 `0.0f` 是"保持当前字号"，等于什么都没变 ✗。

## 4. 页头 / 导航（`WizardChrome.cpp`）

* 页头：`SetCursorPosY(18)` → 进度条（高 = `GetFrameHeight()`、宽 = 内容宽，**别压到 10px**：
  百分比文字被裁进条里，矮了就只剩中间一条 ✗）→ 两个 Spacing → 页标题（1.4x）→ 一个 Spacing →
  分隔线 → 一个 Spacing。
* 导航：底边 `y = winH - 56`，按钮 110x30；有 Back 时**成对居中**（总宽 `110*2+12`，间距 12），
  没有 Back 时单个居中。Back 按下时 `drawNavButton` 返回 **false**（只有 `back` 出参知道），
  所以调用处必须是 `if (drawNavButton(...) || back)`。

## 5. 组件（只用这些）

| 组件 | 作用 |
|---|---|
| `formStyleBegin()` / `formStyleEnd()` | 统一点间距（`ItemSpacing = (8, 8)`），成对包住页面主体 |
| `drawConfigHeader(ch, "Trail", 60.0f)` | 进度条 + 页标题 + 分隔线（百分比按页序算：20/40/60/80/100） |
| `formLabel(ch, "Resolution", sub)` | 画标签、把光标移到控件列 |
| `formCtrlW(ch, sub)` | 控件宽度，喂给 `SetNextItemWidth` |
| `formCheckbox(ch, "Preload", &v, sub)` | 左列复选框行（组头不放控件列；`sub` 缩进 24） |
| `drawNavButton(...)` | Back/Next/Start |

两种行型：

* **复选框行** —— 框和文字都在左列（`formCheckbox`）。它下面的从属行用 `sub = true` 缩进 24。
  例：Hitsounds 的 `Enable hitsounds` → `Override hit sound type`；Colors 的 `Auto stroke` → `Stroke`。
* **字段行** —— 标签在左列，控件在控件列（`formLabel` + `SetNextItemWidth(formCtrlW(ch))`）。
  布尔字段也是一个字段行：标签在左列、复选框在控件列（Graphics 的 `Fullscreen`）。
  例外：需要跟复选框同一行的小控件（`Override hit sound type` 的 Combo）用 `kFormInlineW`。

页面骨架：

```cpp
formStyleBegin();                        // ← 行距
drawConfigHeader(ch, "Graphics", 40.0f); // ← 页头

formLabel(ch, "Resolution");             // ← 字段行
ImGui::SetNextItemWidth(formCtrlW(ch));
ImGui::Combo("##reso", &st.resoIdx, ...);

formCheckbox(ch, "Auto stroke", &st.autoStroke);   // ← 复选框组头
formLabel(ch, "Stroke", /*sub=*/true);             // ← 从属行
ImGui::SetNextItemWidth(formCtrlW(ch, true));
ImGui::ColorEdit3("##stroke", ...);

formStyleEnd();                          // ← 弹样式（导航按钮是绝对定位，放这之后画）
```

## 6. 禁止（都是踩过的）

* ✗ 页面里写坐标/宽度常量（150/140/160/300 之类）→ 用组件。
* ✗ `st.page = Page::X` → 走 `nextPage()/prevPage()`（`WizardState.hpp` 有编译期自检：
  写成"跳到本页"就编不过；2026-10-16 真出过 Graphics 的 Next 点不动）。
* ✗ tooltip / 悬停说明。用户："其他地方都没有" —— 一个控件单独挂说明很突兀。
  参数含义写进 `docs/`（如 Cull block 的取舍在 `docs/rendering.md`）。
* ✗ 页面里自己加分隔线或成堆 `Spacing()` 当节奏 —— 页头已含节奏，行距由 `formStyleBegin` 统一。
* ✗ 想让 `SetNextItemWidth` 影响复选框（它不吃宽度，宽度会漏给下一项）。

## 7. 状态

* 禁用：`ImGui::BeginDisabled()` / `EndDisabled()`（Trail 整组跟随 `Trail` 复选框、依赖项跟随
  上一行的布尔值）。
* 错误：红色文字 `ImVec4(1.0f, 0.45f, 0.45f, 1.0f)`，贴左列。
* 主按钮：Next/Start（`drawNavButton` 的 `isStart`），`canContinue = false` 时置灰。

## 8. Welcome 页是唯一的例外

Welcome 是"hero"版式：居中大标题 + 居中文件行 + 两个按钮。**不要把表单栅格套进去** ——
它是落地面，不是表单页。共用的是：字号档、按钮尺寸（110x30）、`kFormButtonW`/`kFormGap`、
错误红字。

## 9. 改 UI 的验收

1. `ctest --test-dir build`（24 项，含字体三档递增）；
2. **逐页截图**核对（`screencapture -x -o -R<x>,<y>,<w>,<h>` 裁窗口区域，Retina 是 2x）——
   右边缘是否都在 496、有无重叠/被按钮盖住；
3. 导航有改动时用注入点击（`io.AddMousePosEvent` + `AddMouseButtonEvent`）真点一遍，
   别只看代码。
