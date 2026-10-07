#pragma once

// archive 模块唯一需要给"非实现方"看的接口：把解压后端注册给 core。
// 刻意不 include <lzma.h>/<zstd.h> —— 只想用压缩谱面的程序（例如 app）不该为了调这一行
// 去配压缩库的头文件路径。要直接使用 ArchiveStream 的（比如对拍测试）才 include LevelArchive.hpp。

namespace adofai {
namespace archive {
void install();
}
}  // namespace adofai
