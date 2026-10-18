#pragma once

#include <string>

// 运行中可执行文件所在目录（macOS 上解析符号链接）。唯一实现 —— 资产查找
// （AssetSetup）与日志路径（Application）都用它，别再各写一份。
std::string executableDirectory();
