# tile-geometry-lab —— "实际看" 砖块几何

中旋砖（`angleData` 里的 **数字 999**）曾经是 `createTileMesh(eA,eA)`，也就是 `ang == 0` 分支：
圆 r=0.30 + 一个旋转方块，在游戏里看着像"大圆 + 菱形塞在砖格里"。改了 **12M token** 都没定下来，
根因不是难，是**判断依据错了**：一直在 3D 透视截图里"看着像不像"，而这是个纯 2D 多边形 ——
顶点公式差一点，透视里肉眼分不出来。最后靠"把顶点摊平成 2D 图"当场看明白。

这个目录就是那套看图的工具。**形状对不对用肉眼看，几何有没有被改坏用 `ctest` 看** ——
机械护栏在 `tests/tile_geometry_test.cpp`（`ctest -R tile_geometry`：五边形三条不变量 +
负向对照 + `TileMesh.cpp` 调用点护栏）。

## 一、几何层：把**游戏里那份** C++ 的顶点摊平成 2D 图

编的不是复制品，就是 `render/TileGeometry.cpp` 本人，所以出来的形状就是画面上的形状。

```bash
tools/tile-geometry-lab/dump.sh geo.json                    # 编译 + dump 顶点（6 个用例）
node tools/tile-geometry-lab/json2svg.mjs geo.json > geo.svg
NODE_PATH=$(npm root -g) node tools/tile-geometry-lab/svgshot.cjs geo.svg geo.png 1100 800
# 然后看 geo.png（旧 = 圆+方块，新 = 五边形，顶点号都标着）
```

* `dump.cpp` 里想加用例就加一行 `caseOf("标题", "备注", [&]{ …createMidSpinMesh(a1, sc); })`；
* 编译/看图依赖：`clang++`（或 `CXX=`）、`node`、`playwright`（`NODE_PATH=$(npm root -g)`）、
  系统 Chromium（`CHROME=` 可覆盖路径，默认 macOS 的 `/Applications/Chromium.app`）。

## 二、渲染层：真机 A/B

```bash
./build/ADOCAO tools/tile-geometry-lab/midspin-sample.adofai --auto-play --no-hitsound --fullscreen
screencapture -x shot.png     # macOS；Linux 用 import/xwd，Windows 用 Win+Shift+S
```

`midspin-sample.adofai` 是合法 JSON 的 10 层合成谱，**第 2 层是中旋**，跑起来一秒内就能看到：

* 旧几何 → 砖格里一个 **"D"**（圆被左边框切掉的样子）；
* 新几何 → 砖格里一个 **">"**（五边形的尖角朝来路）。

要 A/B 就 `git stash` 改前/改后各跑一次，同一张谱、同一时刻截图对比。

## 三、对拍：同一张谱喂给 Re_ADOJAS（参考播放器）

```bash
# 先在 Re_ADOJAS 仓库里 `pnpm dev`（默认 http://127.0.0.1:3144）
NODE_PATH=$(npm root -g) node tools/tile-geometry-lab/adojas_chart.cjs midspin-sample.adofai
```

两边画出来应当一致（都是 `">"`）。

## 踩过的坑（省你一次）

| 坑 | 现象 | 正解 |
|---|---|---|
| Re_ADOJAS 用 **HashRouter** | `/editor` 只给首页，看着像"载入成功但没反应" | 必须 `http://127.0.0.1:3144/#/editor` |
| Chromium 全页截图 | `page.screenshot({fullPage:true})` 在 SVG 文档上卡到 30 s 超时 | 去掉 `fullPage`，viewport 给足尺寸 |
| 顶点顺序不是边界序 | 直接对顶点 0..4 求 shoelace 会得到 `3w²/2`（闭合边穿过矩形左边 → 自交） | 边界序是 `0,1,2,4,3`；或者直接按三角形求和（渲染器填的就是三角形） |
| Playwright 找不到 | `Cannot find module 'playwright'` | `NODE_PATH=$(npm root -g)`（本机 playwright 装在 `/opt/homebrew/lib/node_modules`） |
| 手动 `mktemp` 出来的谱没验过 | 上一版文档里的示例谱带中文省略号 `…`，**不是合法 JSON** | 示例谱也要过 `json.loads` + `./build/tests/adocao_level_parse_test <file>` |
