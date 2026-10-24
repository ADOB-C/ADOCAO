# 图像导出 `adocao image`（从 AGENTS.md 拆出）

> 本文 2026-10 从 `AGENTS.md` **整段搬出**（内容一字未改，只换了文件）；AGENTS.md 里
> 留了指针。改这一块之前先读它，改完按 AGENTS.md 的验收清单跑对应护栏。

## 图像导出（`adocao image`，真无头）

`adocao image out.png --level <谱> [--size WxH] [--bg HEX|transparent] [--time-color]
[--padding F] [--thickness N]` 把整条路径
（红/蓝两星 ✓）按世界坐标包围盒**等比**铺进一张 PNG ✓ —— **不碰 GL、不开窗口、不需要 ffmpeg** ✓，
所以在没有显示的机器上也能出图 ✓，CI 里也不用 Xvfb ✓。

- 位置取自 `PositionSolver::positionAtTile` ✓（和游戏里同一套解算 ✓）；颜色**默认纯色**：
  整条路径 `fillRgba = #DEBB7B`、背景 `bgRgba = #000000`（就是游戏里那两种颜色 ✓）。
  **看走向用 `--time-color`**：按**谱面时间**六档彩虹（`timeColorAt`，与 1px=1tile 瓦片同一函数 ✓）；
  不开它时整条路径只有一种线色（原来那条"按进度从 stroke 渐变到 fill 便于看走向"的 `--gradient`
  已删除 —— 那是彩虹的职责，不该由渐变兼任）。
  要做到**严格两种颜色**还要配 `--thickness 1`（无 AA 硬线）：AA 边缘会与背景混出中间色。
  带 AA 时的实测口径（MYC 16K 纯色）：610 种颜色，前两名就是 `#000000` 与 `#DEBB7B`，
  其余全是它与黑的混合；但其中 **2,257 个墨点（5.4%）不严格落在等比线上**，最大偏差约
  18 个 8-bit 单位。原因是 AA 是**逐段 8-bit 累加**（一个像素被 ~382 段穿过，每段
  `px += (c-px)*cov`，三通道各自取整 → 随机游走 σ≈10，最暗通道会被舍到 0），
  **不是**还有别的颜色源：`core/map/LevelMap.cpp` 里纯色模式全程只用 `fill` 一种线色。
- **`--padding F`**：画布留白比例（**默认 0 = 内容贴边、不留黑边**；给正数才四周留白，
  例如 `--padding 0.02` = 四边各留 2%）。实测四边留白与 `padding` 精确对应（16K 上 2% → 左右 327 px、
  上下 131 px；`--padding 0` → 墨包围盒 `x[0,16383] y[0,6615]`，四边 0.00%）。
- **线宽 / `--thickness N`**：默认 `halfW = clamp(scale x thicknessScale x 0.5, 0.5, 6.0)`。
  注意 `thicknessScale` 只在 native 1px=1unit 下才起效；普通分辨率下永远撞 **0.5 px 下限**，
  即名义上是 1 px 的线，实际整条线只有抗锯齿渗出 —— 16K 实测宽度在 **2~3 px 之间抖**
  （亮度阈值 50%：2 px x 2810 行 / 3 px x 3791 行）。
  **想更细只能关 AA**：`blend()` 是 alpha 叠加，而一个像素会被 ~382 段路径穿过 → 邻像素
  （覆盖率 0.5）被反复叠加到饱和，于是**名义 1 px 的 AA 线，实心部分量出来是 3.6 px**。
  所以 **`--thickness 1`（或任何 ≤1）= 无 AA 的 1 px 硬线**（`drawSegmentHard`：DDA 沿长轴
  每步点亮一个像素，直接写满色）：16K 实测墨点 **27,331**（AA 版 62,293，**2.3x 少**），
  水平游程 1-3 px（AA 版 4-7 px）。代价是高倍放大有锯齿 —— 1 px 线的固有性质。
  `--thickness N>1` 仍走 AA 加粗（N=2 → 核心 3.6 px，N=8 → 9.3 px；渲染出来比名义宽约 1 px，
  同为重叠饱和所致）。
  对照：瓦片拼接那条路（`stitch --scale 120` 的最大值降采样）是**均匀 2 px**（6536/6543 行），
  但由 120 px 源块量化而来 → 高倍看是阶梯。
- 为巨谱设计：**两遍扫描**（先包围盒、再逐段画 ✓）不保存点位 ✓；逐段解析式抗锯齿 ✓，
  亚像素的段落也能正确堆叠 ✓。实测 **6,770,913 层 → 4096² 6.3 s / 685 KB** ✓、
  **16384x6616（108 MPix）5.0 s / 4.2 MB** ✓、Singularity 997,665 层 → 2048² 0.9 s ✓。
  内存护栏 `maxPixels`（默认 **256 MPix**：16K 是 108 MPix = 434 MB 缓冲的正当请求；1px=1unit 的
  1.5 Tpix 仍会被挡 ✓）。
- 实现：`core/map/LevelMap.{hpp,cpp}`（纯 core ✓，只有 RGBA8 缓冲 ✓）+ `app/MapExport.cpp`
  （stb 写 PNG ✓）；CLI 分支在 `app/main.cpp`，**在任何 GL 初始化之前**返回 ✓。
- **一条命令覆盖两条路线**：默认矢量（5 秒出 16K）；`--1px` 内部自己出瓦片→拼接→**清掉中间产物**
  （`--keep-tiles <dir>` 保留、`--scale N` 降采样、`--1bit` 单色谱写索引色）。`map`/`mono` 两个
  子命令已并入 `image`（撞到会打印新写法）；`tiles`/`stitch` 保留为进阶零件（只出瓦片 / 只拼瓦片，
  便于检视或复用已有瓦片）。
- 已知可续：按 tile 区间取景（`--range A-B` ✓）用来看密集"结"的内部结构 ✓。

## TIFF 输出（`.tif` / `.tiff`）

`adocao image out.tif --level <谱> [--tiff-layout tile|strip] [--tiff-compress deflate|zstd]`
走 `core/map/TiffWriter`（自研，无 libtiff 依赖）：**每块独立压缩**、边算边写，
内存只保留一条带 + 线段表（16K 实测峰值 **50.8 MB**，整图 PNG 是 890 MB）。

**两种布局**（2026-10-23 实测，4096x3072、2025）：

| 布局 + 压缩 | 体积 | libtiff / ImageMagick | macOS 预览（ImageIO） |
|---|---|---|---|
| `tile` + `zstd` | 68,639 B | ✓ | ✗ |
| **`tile` + `deflate`（默认）** | 123,086 B | ✓ | ✗ |
| `strip` + `zstd` | 67,863 B | ✓ | ✗（Compression=50000 不支持） |
| `strip` + `deflate` | 123,624 B | ✓ | **✓** |

所以：

- **要 macOS 预览/快速查看能打开** → `--tiff-layout strip --tiff-compress deflate`；
- **要随机访问（看右下角只解几块）且体积最小** → 默认 `tile + zstd`（用 ImageMagick/libtiff 看）；
- **默认 `tile + deflate`**：布局按用户要求保持 tiled（随机访问好），压缩从 zstd 换成 deflate
  （zstd 只有 libtiff/ImageMagick 认，ffmpeg 直接报错；Krita/Kdenlive 静帧两条都行）；
- 走 **MLT→FFmpeg** 的环节（Kdenlive 图片序列/proxy）必须 `--tiff-layout strip` ——
  ffmpeg 不读 tiled 且**不报错**（解出全黑）；
- 体积优先：`--tiff-compress zstd`（小 1.8×，但 ffmpeg / macOS 预览都不认）。

### 具体软件能不能开（2026-10-23 实测）

| 读者 | tile + zstd | tile + deflate | strip + deflate |
|---|---|---|---|
| **Krita**（自带 libtiff 6.2.0） | **✓ 用户实测**（tile 打开正常；测试样图 `tiff_writer_tile.tif`） | ✓ | ✓ |
| **Kdenlive / MLT `qimage`**（Qt → libtiff 6.2.0） | **✓ 用户实测**（16K 直接拖进 Project Bin 能显示；另用 `melt qimage:` 解帧与 PNG 降采样逐像素相同） | ✓ | ✓ |
| Kdenlive / MLT `avformat`（ffmpeg） | ✗ `Unknown compression method 50000` | ✗ **全黑**（ffmpeg 不读 tiled） | ✓ |
| macOS 预览 / `sips`（ImageIO） | ✗ | ✗（不认 tiled） | ✓ |
| ImageMagick / libtiff `tiffinfo` | ✓ | ✓ | ✓ |

**结论（已由用户在两款软件里实际打开确认）**：Krita 与 Kdenlive 的**静帧**路径（都走 libtiff）能开 `tile + zstd`；
但 Kdenlive 里任何走 **ffmpeg** 的环节（图片序列、proxy、部分导出）只认 **strip + deflate** ——
ffmpeg 的 TIFF 解码器既不吃 tiled（解出来全黑，不报错）也不吃 zstd（报错）。

16K（16384x6616，2025）两档实测：

| 配置 | 体积 | 峰值 RSS | Preview |
|---|---|---|---|
| `tile + zstd`（默认） | **327,241 B** | 37.2 MB | ✗ |
| `strip + deflate` | 771,520 B | 50.8 MB | ✓ |
| （对照）整图 PNG | 4,657,533 B | 890 MB | ✓ |

**为什么 tile 也不能完全替代 strip**：macOS ImageIO **不认 tiled TIFF**（`sips` 返回 nil，
连 ImageMagick 生成的 tiled 文件也一样）—— 这是平台限制，不是我们的实现问题。

> 像素内容与 PNG **逐字节相同**（PIL 全图对拍，四种组合都验过）。
> BigTIFF（64 位偏移）在偏移会超过 4 GB 时自动启用（`TiffWriter` 内部判断）。
