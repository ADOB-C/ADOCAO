# ADOCAO

用 **C++20 + OpenGL 4.3** 从零写的 [A Dance of Fire and Ice](https://store.steampowered.com/app/977950/A_Dance_of_Fire_and_Ice/) 谱面播放器。

打开 `.adofai` 就能看谱、听歌、跟着行星踩格子 —— 几千万层的谱面也拖得动。
Windows / Linux / macOS 全平台，没有 Unity，也没有浏览器内核。

> 同源的 Vulkan 版本：[ADOCAV](https://github.com/CHT-1192/ADOCAV)

## 能做什么

- **播放**：相对角度、`SetSpeed` / `Twirl` / `Pause` / `Midspin`、MoveCamera、MoveTrack、
  AnimateTrack、颜色与闪屏事件、书签跳转
- **听歌**：AIFF / OGG / WAV / FLAC；27 种打击音色按谱面预合成，16-bit 硬削波混音
- **超大谱面**：视锥剔除 + 可见性缓存 + 实例化渲染，实测 6,800 万层的谱面照样能拖能跳
  （[目标](docs/2e9.md) 是 21 亿层）
- **无头出图**：`adocao image` 直接渲染成 PNG，不弹窗
- **命令行工具**：`adocao pack` 把谱面压成自研 `.adocao` 容器（MYC 611 MB → **6 KB**，逐位无损），
  `adocao json` 再还原成明文
- **启动器**：拖进谱面、选音色、调分辨率与缩放；也可以纯命令行

## 快速开始

**下载**：到 [Releases](../../releases) 取对应平台的压缩包，解压即用（macOS 是 `.app`）。

**从源码构建**：

```bash
# Windows
build.bat                 # 默认 Ultra 缩放；build.bat -Portable 静态链接

# Linux / macOS
chmod +x build.sh && ./build.sh
```

首次运行会问几个选项（缩放档位、是否静态链接）；之后不带参数重跑就是**增量构建**。
依赖（GLFW / glm / RapidJSON / Dear ImGui / miniaudio / liblzma / libzstd …）由 CMake 自动拉取。

## 用法

双击打开启动器，把 `.adofai` 拖进去即可。也支持命令行：

```bash
adocao --level "chart.adofai" --music "song.ogg"          # 直接播放
adocao                                                    # 不带参数 → 启动器
adocao image out.png --level "chart.adofai"               # 无头出图
adocao pack "chart.adofai" chart.adocao --compress=auto   # 打成 .adocao 容器
adocao json chart.adocao back.adofai                      # 容器还原成明文
adocao --help                                             # 全部开关
```

### 操作

| 按键 | 作用 |
|------|------|
| `Space` | 播放 / 暂停 |
| 鼠标拖动 | 平移视角（暂停时） |
| 滚轮 | 缩放 |
| `Ctrl` + `←` / `→` | 上一个 / 下一个书签 |
| 点击砖块 | 从那一块开始播 |
| `Esc` | 退出 |

## 文档

| 想了解 | 看这里 |
|---|---|
| 命令行全部开关 | [`docs/cli.md`](docs/cli.md) |
| 渲染与几何约定、像素门槛 | [`docs/rendering.md`](docs/rendering.md) |
| 解析器、加载路径、Timeline | [`docs/level-loading.md`](docs/level-loading.md) |
| `.adocao` 容器规范 | [`docs/adocao-format.md`](docs/adocao-format.md) |
| 项目目标（21 亿层）与阶段 | [`docs/2e9.md`](docs/2e9.md) |
| 目录结构、历史决策 | [`docs/project-structure.md`](docs/project-structure.md) |

## 说明

- 这是**播放器**，不是编辑器；谱面语义以 [Re_ADOJAS](https://github.com/adofaiex/Re_ADOJAS) 为最权威参考。
- 不支持装饰物渲染（装饰系统深度依赖 Unity），但 `.adocao` 会**无损保存**装饰数据。
- 不含歌曲 —— 请自备音源与谱面。

## 致谢

- [Re_ADOJAS](https://github.com/adofaiex/Re_ADOJAS) —— Three.js 实现，语义上的权威参考
- [ADOFAI-JS](https://github.com/adofaiex/ADOFAI-JS) —— 角度解析
- [ADOFAI_HitSound](https://github.com/Maicy0609/ADOFAI_HitSound) —— 打击音色合成

## 许可证

Apache 2.0
