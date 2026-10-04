#pragma once

#include <string>

// 把关卡地图全景写成 PNG（真无头：不初始化 GL、不开窗口）。
// sizeStr 形如 "4096x4096"（空 = 默认），bgStr 为 6/8 位 hex 或 "transparent"。
// 返回 0 成功。
int exportLevelMap(const std::string& levelPath, const std::string& outPath,
                  const std::string& sizeStr, const std::string& bgStr);
