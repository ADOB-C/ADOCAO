#pragma once

#include <string>

// 把关卡地图全景写成 PNG（真无头：不初始化 GL、不开窗口）。
// sizeStr 形如 "4096x4096"（空 = 默认），bgStr 为 6/8 位 hex 或 "transparent"。
// padding < 0 = 用默认（0.02 = 四周留 2%）；--map-padding 0 则内容贴边。
// 返回 0 成功。
int exportLevelMap(const std::string& levelPath, const std::string& outPath,
                  const std::string& sizeStr, const std::string& bgStr,
                  const std::string& tilesStr = "", bool native = false,
                  bool timeColor = false, float padding = -1.0f, float lineWidthPx = -1.0f);
// lineWidthPx > 0 = 直接指定线宽（像素）；<= 0 = 用默认（thicknessScale + clamp）

// 整谱 1 像素 = 1 层：流式分块写出（只写有墨的块），可选按层区间裁剪。
// outDir 下产出 x####_y####.png 若干 + _overview.png（缩略索引）。
int exportLevelMapNativeAll(const std::string& levelPath, const std::string& outDir,
                            const std::string& tilesStr, int block, int threads,
                            bool timeColor = false);

// 把 --map-native-all 的分块拼回一张图。文件名自带绝对坐标 x%07lld_y%07lld_w%lld_h%lld.png，
// 所以不需要任何清单。
// scale == 1 走并行路径（先建按行墨点索引，再分带并行 deflate；见 .cpp 注释）：
//   threads <= 0 → 4（每个作业 2 P + 2 E 核）；ADOCAO_STITCH_SEQ=1 强制旧串行流式路径。
// scale > 1 做整数盒式降采样（输出宽 = 画布宽 / scale），仍是串行流式。
// 测试钩子（仅 scale == 1）：ADOCAO_STITCH_Y0 / ADOCAO_STITCH_ROWS 只输出源行区间，
//   便于和串行路径在同一横条上逐行对拍。
int stitchLevelMapTiles(const std::string& dir, const std::string& outPng, int scale, int threads = 0);

// 把 --map-native-all 的瓦片写成「索引色 1 bit」PNG（两种颜色，与瓦片逐像素一致）。
// 行字节只有 (W+7)/8 = 243 KB（对比 8bit RGBA 的 7.8 MB），deflate 代价同步降 32 倍。
int exportLevelMapMono1(const std::string& tileDir, const std::string& outPng, int threads = 0);
