#pragma once

#include <cstdint>
#include <vector>

namespace adofai { class Timeline; }   // 库侧类型：不许在全局前置声明（会和 using namespace 打架）

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
    bool  timeColor = false;            // 按**谱面时间**着色：t=0 为 timeFrom，结束为 timeTo
    uint32_t timeFrom = 0xFF0000FF;     // 红
    uint32_t timeTo   = 0xFF00FFFF;     // 品红
    bool  drawBlue = true;              // 同时画蓝星轨迹
    bool  markers  = false;             // 起点/终点小圆点（默认关：不做标记）
    float thicknessScale = 0.6f;        // 线宽 = 每 tile 像素数 * 该系数
    float lineWidthPx = 0.0f;           // >0 = 直接用这个线宽（像素），覆盖 thicknessScale。
                                        // 需要它是因为小 scale 下（如 16K：scale≈0.0084）乘出来
                                        // 只有 0.0025 px，会被 halfW 的 0.5 px 下限吃掉 —— 结果整条线
                                        // 只剩抗锯齿渗出，宽度在 2~3 px 之间抖（实测）。
    bool  hardLine = false;             // true = 无 AA 的 1 px 硬线（DDA 逐像素写满色）。
                                        // 要它是因为 blend() 是 alpha 叠加，而一个像素会被 ~382 段
                                        // 穿过 → 邻像素 0.5 的覆盖率反复叠加到饱和，实心宽度从名义
                                        // 1 px 膨胀到 3.6 px（实测）。想**更细**只能关掉 AA。
    size_t maxPixels = 256ull * 1024 * 1024;  // 内存护栏（宽*高）：IGA 16K（16384x6616 = 108 MPix
                                              // = 434 MB 缓冲）是正当请求；1px=1unit 的 1.5 Tpix 仍会被挡
    long long firstTile = 0;            // 只画 [firstTile, lastTile]（含），用来看密集"结"的内部
    long long lastTile  = -1;           // -1 = 画到最后一层
    bool  nativeScale = false;          // true = 强制 1 像素 = 1 世界单位（即 1 层 ≈ 1 像素）
                                        //        放不下就按 maxPixels 失败并说明需要多大
};

// 谱面时间 t∈[0,1] → 六档等距彩虹色（RRGGBBAA）：
//   ff0000 → ffff00 → 00ff00 → 00ffff → 0000ff → ff00ff
inline uint32_t timeColorAt(double t) {
    static const uint32_t kStops[6] = { 0xFF0000FFu, 0xFFFF00FFu, 0x00FF00FFu,
                                        0x00FFFFFFu, 0x0000FFFFu, 0xFF00FFFFu };
    if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
    const double x = t * 5.0;
    const int k = (int)x; const double f = x - (double)k;
    const uint32_t a = kStops[k], b = kStops[k < 5 ? k + 1 : 5];
    auto ch = [](uint32_t c, int sh) { return (double)((c >> sh) & 0xFF); };
    const double r = ch(a,24) + (ch(b,24)-ch(a,24))*f;
    const double g = ch(a,16) + (ch(b,16)-ch(a,16))*f;
    const double bl= ch(a, 8) + (ch(b, 8)-ch(a, 8))*f;
    return ((uint32_t)(r+0.5) << 24) | ((uint32_t)(g+0.5) << 16) | ((uint32_t)(bl+0.5) << 8) | 0xFFu;
}

// 成功时 out 为 width*height*4 的 RGBA8（自上而下），返回 true。
bool renderLevelMap(const adofai::Timeline& timeline, const LevelMapOptions& opts,
                    std::vector<uint8_t>& out);
