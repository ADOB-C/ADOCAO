#pragma once

#include <cstdint>
#include <vector>

class Timeline;

// 关卡地图全景：把整条路径按世界坐标的等比缩放画进一张 RGBA8 图。
//
// 真无头 —— 这里不碰 GL、不开窗口，也不需要 ffmpeg（PNG 由调用方用 stb 写出）。
// 为 677 万 tile 那种巨谱设计：分两遍扫描（先包围盒，再画），不保存全部点位；
// 逐段做解析式抗锯齿，所以亚像素的段落也能正确堆叠成"走向"。
struct LevelMapOptions {
    int   width  = 4096;
    int   height = 4096;
    float padding = 0.02f;              // 画布留白（比例）
    uint32_t fillRgba   = 0xDEBB7BFF;   // 轨道主色（与渲染器默认同一对颜色）
    uint32_t strokeRgba = 0x6F5D3DFF;   // 起点的颜色，终点渐变到 fillRgba
    uint32_t bgRgba     = 0x000000FF;   // 默认不透明黑（想透明就传 alpha=0）
    bool  gradient = true;              // 沿进度渐变，便于看出起点→终点
    bool  drawBlue = true;              // 同时画蓝星轨迹
    bool  markers  = true;              // 起点/终点小圆点
    float thicknessScale = 0.6f;        // 线宽 = 每 tile 像素数 * 该系数
    size_t maxPixels = 96ull * 1024 * 1024;  // 内存护栏（宽*高）
    long long firstTile = 0;            // 只画 [firstTile, lastTile]（含），用来看密集"结"的内部
    long long lastTile  = -1;           // -1 = 画到最后一层
    bool  nativeScale = false;          // true = 强制 1 像素 = 1 世界单位（即 1 层 ≈ 1 像素）
                                        //        放不下就按 maxPixels 失败并说明需要多大
};

// 成功时 out 为 width*height*4 的 RGBA8（自上而下），返回 true。
bool renderLevelMap(const Timeline& timeline, const LevelMapOptions& opts,
                    std::vector<uint8_t>& out);
