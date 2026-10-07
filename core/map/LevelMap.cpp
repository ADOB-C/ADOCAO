#include "core/map/LevelMap.hpp"

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/util/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

struct Rgba { float r, g, b, a; };

Rgba unpack(uint32_t v) {
    return { (float)((v >> 24) & 0xFF) / 255.0f,
             (float)((v >> 16) & 0xFF) / 255.0f,
             (float)((v >> 8)  & 0xFF) / 255.0f,
             (float)( v        & 0xFF) / 255.0f };
}

inline void blend(uint8_t* px, const Rgba& c, float cov) {
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;
    const float a = cov * c.a;
    const float ia = 1.0f - a;
    px[0] = (uint8_t)std::lround(px[0] * ia + c.r * 255.0f * a);
    px[1] = (uint8_t)std::lround(px[1] * ia + c.g * 255.0f * a);
    px[2] = (uint8_t)std::lround(px[2] * ia + c.b * 255.0f * a);
    px[3] = (uint8_t)std::lround(px[3] * ia + 255.0f * a);
}

// 解析式抗锯齿线段：在 bbox 内按"到线段的距离"给覆盖率
void drawSegment(std::vector<uint8_t>& img, int W, int H,
                 double x0, double y0, double x1, double y1,
                 double halfW, const Rgba& c) {
    const double minX = std::min(x0, x1) - halfW - 1.0, maxX = std::max(x0, x1) + halfW + 1.0;
    const double minY = std::min(y0, y1) - halfW - 1.0, maxY = std::max(y0, y1) + halfW + 1.0;
    int ix0 = (int)std::floor(minX), ix1 = (int)std::ceil(maxX);
    int iy0 = (int)std::floor(minY), iy1 = (int)std::ceil(maxY);
    ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
    ix1 = std::min(ix1, W - 1); iy1 = std::min(iy1, H - 1);
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    for (int y = iy0; y <= iy1; ++y) {
        for (int x = ix0; x <= ix1; ++x) {
            const double px = (double)x + 0.5, py = (double)y + 0.5;
            double t = 0.0;
            if (len2 > 1e-12) t = ((px - x0) * dx + (py - y0) * dy) / len2;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            const double cx = x0 + dx * t, cy = y0 + dy * t;
            const double ddx = px - cx, ddy = py - cy;
            const double d = std::sqrt(ddx * ddx + ddy * ddy);
            const float cov = (float)(halfW + 0.5 - d);
            if (cov > 0.0f) blend(&img[((size_t)y * W + x) * 4], c, cov);
        }
    }
}

void drawSegmentHard(std::vector<uint8_t>& img, int W, int H,
                     double x0, double y0, double x1, double y1, const Rgba& c) {
    // 无 AA 的 1 px 线：沿长轴每步点亮**一个**像素（这里不能走 blend —— 见 hardLine 的注释）
    const double dx = x1 - x0, dy = y1 - y0;
    const int n = (int)std::ceil(std::max(std::abs(dx), std::abs(dy)));
    const uint8_t cr = (uint8_t)std::lround(c.r * 255.0f), cg = (uint8_t)std::lround(c.g * 255.0f);
    const uint8_t cb = (uint8_t)std::lround(c.b * 255.0f);
    for (int k = 0; k <= n; ++k) {
        const double t = n ? (double)k / (double)n : 0.0;
        const int x = (int)std::floor(x0 + dx * t), y = (int)std::floor(y0 + dy * t);
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        uint8_t* p = &img[((size_t)y * W + x) * 4];
        p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255;
    }
}

void drawDisc(std::vector<uint8_t>& img, int W, int H, double cx, double cy,
              double r, const Rgba& c) {
    int ix0 = std::max(0, (int)std::floor(cx - r - 1)), ix1 = std::min(W - 1, (int)std::ceil(cx + r + 1));
    int iy0 = std::max(0, (int)std::floor(cy - r - 1)), iy1 = std::min(H - 1, (int)std::ceil(cy + r + 1));
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x) {
            const double ddx = (double)x + 0.5 - cx, ddy = (double)y + 0.5 - cy;
            const double d = std::sqrt(ddx * ddx + ddy * ddy);
            const float cov = (float)(r + 0.5 - d);
            if (cov > 0.0f) blend(&img[((size_t)y * W + x) * 4], c, cov);
        }
}

}  // namespace

bool renderLevelMap(const Timeline& tl, const LevelMapOptions& opts, std::vector<uint8_t>& out) {
    if (opts.width <= 0 || opts.height <= 0) return false;
    if ((size_t)opts.width * (size_t)opts.height > opts.maxPixels) {
        LOG_E("Map: %dx%d 超过内存上限（maxPixels=%zu）", opts.width, opts.height, opts.maxPixels);
        return false;
    }
    const LevelData* lv = tl.level();
    if (!lv) return false;
    const int n = (int)lv->tiles.size();
    const auto& times = tl.tileStartTimes();
    if (n < 2 || times.size() < (size_t)n) {
        LOG_E("Map: 层数不足（n=%d, times=%zu）", n, times.size());
        return false;
    }

    int i0 = (int)std::max<long long>(0, opts.firstTile);
    int i1 = opts.lastTile < 0 ? n - 1 : (int)std::min<long long>(n - 1, opts.lastTile);
    if (i1 <= i0) { LOG_E("Map: 层区间为空（%d..%d）", i0, i1); return false; }

    // ── 第一遍：红/蓝两星的世界坐标包围盒（不存点，677 万 tile 也只占常数内存）
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    glm::dvec2 red, blue;
    for (int i = i0; i <= i1; ++i) {
        PositionSolver::positionAtTile(tl, times[i], i, red, blue);
        const double xs[2] = { red.x, blue.x }, ys[2] = { red.y, blue.y };
        const int cnt = opts.drawBlue ? 2 : 1;
        for (int k = 0; k < cnt; ++k) {
            minX = std::min(minX, xs[k]); maxX = std::max(maxX, xs[k]);
            minY = std::min(minY, ys[k]); maxY = std::max(maxY, ys[k]);
        }
    }
    const double bw = std::max(maxX - minX, 1e-9), bh = std::max(maxY - minY, 1e-9);
    if (opts.nativeScale) {
        // 1 像素 = 1 世界单位：画布由包围盒决定，放不下就明说需要多大
        const long long needW = (long long)std::ceil(bw) + 3, needH = (long long)std::ceil(bh) + 3;
        if (needW <= 0 || needH <= 0 ||
            (size_t)needW * (size_t)needH > opts.maxPixels) {
            LOG_E("Map: native 需要 %lld x %lld 像素（%.2f GB 缓冲），超过 maxPixels=%zu —— "
                  "改用 --map-tiles A-B 缩小层区间", needW, needH,
                  (double)needW * (double)needH * 4.0 / 1073741824.0, opts.maxPixels);
            return false;
        }
        LOG_I("Map: native 画布 %lld x %lld 像素（1 像素 = 1 单位）", needW, needH);
    }
    const double padX = opts.width * opts.padding, padY = opts.height * opts.padding;
    const double scale = opts.nativeScale ? 1.0
        : std::min((opts.width - 2 * padX) / bw, (opts.height - 2 * padY) / bh);
    const double offX = (opts.width  - bw * scale) * 0.5 - minX * scale;
    const double offY = (opts.height - bh * scale) * 0.5 + maxY * scale;   // 世界 Y 向上 → 图像 Y 向下
    auto toPx = [&](const glm::dvec2& p) {
        return glm::dvec2{ p.x * scale + offX, offY - p.y * scale };
    };

    // ── 画布（native 模式由包围盒定尺寸）
    if (opts.nativeScale) {
        const_cast<LevelMapOptions&>(opts).width  = (int)std::ceil(bw) + 3;
        const_cast<LevelMapOptions&>(opts).height = (int)std::ceil(bh) + 3;
    }
    const size_t W = (size_t)opts.width, H = (size_t)opts.height;
    out.assign(W * H * 4, 0);
    const Rgba bg = unpack(opts.bgRgba);
    if (bg.a > 0.0f) {
        for (size_t p = 0; p < W * H; ++p) {
            out[p * 4 + 0] = (uint8_t)std::lround(bg.r * 255.0f);
            out[p * 4 + 1] = (uint8_t)std::lround(bg.g * 255.0f);
            out[p * 4 + 2] = (uint8_t)std::lround(bg.b * 255.0f);
            out[p * 4 + 3] = (uint8_t)std::lround(bg.a * 255.0f);
        }
    }
    const Rgba fill   = unpack(opts.fillRgba);
    const double halfW = opts.lineWidthPx > 0.0f
        ? std::max(0.5, std::min(64.0, (double)opts.lineWidthPx * 0.5))            // 显式指定线宽
        : std::max(0.5, std::min(6.0, scale * opts.thicknessScale * 0.5));         // 按 scale 推（下限 0.5）

    // ── 第二遍：逐段画（红蓝各一条，顺序即进度 → 渐变）
    glm::dvec2 prevRed, prevBlue, cur;
    bool havePrev = false;
    for (int i = i0; i <= i1; ++i) {
        PositionSolver::positionAtTile(tl, times[i], i, red, blue);
        if (havePrev) {
            Rgba c = fill;
            if (opts.timeColor) {
                const double tot = std::max(1e-9, tl.totalDuration());
                c = unpack(timeColorAt(times[(size_t)i] / tot));   // 六档彩虹，按真实谱面时间
            }
            const glm::dvec2 a = toPx(prevRed), b = toPx(red);
            if (opts.hardLine) drawSegmentHard(out, (int)W, (int)H, a.x, a.y, b.x, b.y, c);
            else               drawSegment(out, (int)W, (int)H, a.x, a.y, b.x, b.y, halfW, c);
            if (opts.drawBlue) {
                const glm::dvec2 ab = toPx(prevBlue), bb = toPx(blue);
                if (opts.hardLine) drawSegmentHard(out, (int)W, (int)H, ab.x, ab.y, bb.x, bb.y, c);
                else               drawSegment(out, (int)W, (int)H, ab.x, ab.y, bb.x, bb.y, halfW, c);
            }
        }
        prevRed = red; prevBlue = blue; havePrev = true;
    }

    if (opts.markers && !opts.timeColor) {   // 彩虹已用颜色表达时间 → 起止点多余
        glm::dvec2 r0, b0, r1, b1;
        PositionSolver::positionAtTile(tl, times[(size_t)i0], i0, r0, b0);
        PositionSolver::positionAtTile(tl, times[(size_t)i1], i1, r1, b1);
        const double rad = std::max(3.0, std::min(14.0, scale * 1.5));
        const glm::dvec2 s0 = toPx(r0), s1 = toPx(r1);
        drawDisc(out, (int)W, (int)H, s0.x, s0.y, rad, Rgba{0.30f, 0.85f, 0.40f, 1.0f});  // 绿 = 起点
        drawDisc(out, (int)W, (int)H, s1.x, s1.y, rad, Rgba{0.95f, 0.30f, 0.30f, 1.0f});  // 红 = 终点
    }
    LOG_I("Map: %dx%d，%d 层，世界范围 %.1f x %.1f，缩放 %.4f px/单位，线宽 %.2f px",
          opts.width, opts.height, n, bw, bh, scale, halfW * 2.0);
    return true;
}
