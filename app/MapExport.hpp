#pragma once

#include <string>

// 把关卡地图全景写成 PNG（真无头：不初始化 GL、不开窗口）。
// sizeStr 形如 "4096x4096"（空 = 默认），bgStr 为 6/8 位 hex 或 "transparent"。
// 返回 0 成功。
int exportLevelMap(const std::string& levelPath, const std::string& outPath,
                  const std::string& sizeStr, const std::string& bgStr,
                  const std::string& tilesStr = "", bool native = false);

// 整谱 1 像素 = 1 层：流式分块写出（只写有墨的块），可选按层区间裁剪。
// outDir 下产出 x####_y####.png 若干 + _overview.png（缩略索引）。
int exportLevelMapNativeAll(const std::string& levelPath, const std::string& outDir,
                            const std::string& tilesStr, int block, int threads);

// 把 --map-native-all 的分块拼回一张图（流式：逐行输出，不整张进内存）。
// 文件名自带绝对坐标 x%07lld_y%07lld_w%lld_h%lld.png，所以不需要任何清单。
// scale > 1 时做整数盒式降采样（输出宽 = 画布宽 / scale）。
int stitchLevelMapTiles(const std::string& dir, const std::string& outPng, int scale);
