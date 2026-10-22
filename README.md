# ADOCAO

ADOFAI 谱面播放器。C++20 + OpenGL 4.3，Windows / Linux / macOS。

把 `.adofai` 拖进窗口就能看：角度、速度、相机、轨道动画、颜色、打击音色，都按谱面走。
主要目的是**大规模谱面** —— 实测 6,800 万层仍然能拖动和跳转，官方和多数 Web 实现做不到这个量级
（[目标](docs/2e9.md)是 21 亿层）。

同源的 Vulkan 版本：[ADOCAV](https://github.com/CHT-1192/ADOCAV)。

## 用

Releases 里有各平台的包，解压直接用。macOS 是未签名的 `.app`，第一次得右键打开。

不带参数启动会进启动器；也可以直接给命令行：

```bash
adocao --level chart.adofai --music song.ogg
adocao image out.png --level chart.adofai            # 无头出图
adocao pack chart.adofai chart.adocao                # 打成 .adocao 容器
adocao json chart.adocao back.adofai                 # 容器还原成明文
adocao --help
```

按键：

| 键 | 作用 |
|---|---|
| `Space` | 播放 / 暂停 |
| 拖动 / 滚轮 | 平移 / 缩放 |
| `Ctrl` `←` `→` | 上一个 / 下一个书签 |
| 点砖块 | 从这块开始播 |
| `Esc` | 退出 |

## 编译

```bash
build.bat          # Windows
./build.sh         # Linux / macOS
```

第一次运行会问缩放档位和是否静态链接，之后重跑就是增量构建。依赖全部走 CMake FetchContent
（GLFW、glm、RapidJSON、Dear ImGui、miniaudio、liblzma、libzstd），不用预装。

## 一些说明

- `.adocao` 是本项目的谱面容器：列式二进制、逐位无损，MYC 611 MB 的谱压到 6 KB。
  规范在 [`docs/adocao-format.md`](docs/adocao-format.md)。
- 不渲染装饰物。装饰系统跟 Unity 绑得太死（见 `TODO.md`），但容器会把装饰数据原样存下来。
- 不带音乐，谱面和音源自己准备。
- 谱面语义以 [Re_ADOJAS](https://github.com/adofaiex/Re_ADOJAS) 为准，有分歧时我们跟它。

## 文档

- [`docs/cli.md`](docs/cli.md)：命令行
- [`docs/rendering.md`](docs/rendering.md)：渲染、几何、像素门槛
- [`docs/level-loading.md`](docs/level-loading.md)：解析与加载
- [`docs/2e9.md`](docs/2e9.md)：当前目标与阶段
- [`docs/project-structure.md`](docs/project-structure.md)：目录与历史决策

## 致谢

- [Re_ADOJAS](https://github.com/adofaiex/Re_ADOJAS)：Three.js 实现，语义参考
- [ADOFAI-JS](https://github.com/adofaiex/ADOFAI-JS)：角度解析
- [ADOFAI_HitSound](https://github.com/Maicy0609/ADOFAI_HitSound)：打击音色合成

Apache 2.0。
