#include "core/map/LevelMap.hpp"

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/timeline/PositionSolver.hpp"
#include "core/util/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include "core/util/CountLimits.hpp"



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

struct Seg { double x0, y0, x1, y1; uint32_t rgba; };

}  // namespace

bool renderLevelMap(const Timeline& tl, const LevelMapOptions& opts, std::vector<uint8_t>& out) {
    // 整张图 = 一条带（保持老行为的唯一实现，避免两份绘制代码漂移）
    return renderLevelMapBands(tl, opts, opts.height, [&](int, int rows, const std::vector<uint8_t>& data) {
        out = data;
        (void)rows;
        return true;
    });
}

bool renderLevelMapBands(const Timeline& tl, const LevelMapOptions& opts, int bandRows,
                         const std::function<bool(int, int, const std::vector<uint8_t>&)>& sink) {
    if (opts.width <= 0 || opts.height <= 0) return false;
    if ((size_t)opts.width * (size_t)opts.height > opts.maxPixels) {
        LOG_E("Map: %dx%d 超过内存上限（maxPixels=%zu）", opts.width, opts.height, opts.maxPixels);
        return false;
    }
    const LevelData* lv = tl.level();
    if (!lv) return false;
    // #0：int64 算，收窄前显式判（走到这里之前 maxPixels 通常已经拦住了，但仍不许静默截断）
    const int64_t nFull = (int64_t)lv->tiles.size();
    if (!fitsInt32(nFull)) {
        LOG_E("LevelMap 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32", (long long)nFull);
        return false;
    }
    const int n = (int)nFull;
    if (n < 2 || tl.tileCount() < (int64_t)n) {
        LOG_E("Map: 层数不足（n=%d, times=%lld）", n, (long long)tl.tileCount());
        return false;
    }

    int i0 = (int)std::max<long long>(0, opts.firstTile);
    int i1 = opts.lastTile < 0 ? n - 1 : (int)std::min<long long>(n - 1, opts.lastTile);
    if (i1 <= i0) { LOG_E("Map: 层区间为空（%d..%d）", i0, i1); return false; }

    // ── 第一遍：红/蓝两星的世界坐标包围盒（不存点，677 万 tile 也只占常数内存）
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    glm::dvec2 red, blue;
    for (int i = i0; i <= i1; ++i) {
        PositionSolver::positionAtTile(tl, tl.startTimeAt(i), i, red, blue);
        const double xs[2] = { red.x, blue.x }, ys[2] = { red.y, blue.y };
        for (int k = 0; k < 2; ++k) {
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
    const Rgba bg = unpack(opts.bgRgba);
    const double rawHalfW = opts.lineWidthPx > 0.0f
        ? (double)opts.lineWidthPx * 0.5
        : scale * opts.thicknessScale * 0.5;
    const double loHalfW = 0.5, hiHalfW = (opts.lineWidthPx > 0.0f) ? 64.0 : 6.0;
    if (rawHalfW < loHalfW || rawHalfW > hiHalfW) {
        LOG_W("Map: 线宽 %.2f px 超出可绘制范围 [%.1f, %.1f] px → 按边界画（不是静默改参数）",
              rawHalfW * 2.0, loHalfW * 2.0, hiHalfW * 2.0);
    }
    const double halfW = std::max(loHalfW, std::min(hiHalfW, rawHalfW));

    // ── 第二遍：先把线段收集成平表（148k 层 ≈ 9.5 MB），再按 y 分桶
    std::vector<Seg> segs;
    segs.reserve((size_t)(i1 - i0 + 1) * 2);
    {
        glm::dvec2 prevRed, prevBlue;
        bool havePrev = false;
        for (int i = i0; i <= i1; ++i) {
            PositionSolver::positionAtTile(tl, tl.startTimeAt(i), i, red, blue);
            if (havePrev) {
                uint32_t col = opts.fillRgba;
                if (opts.timeColor) {
                    const double tot = std::max(1e-9, tl.totalDuration());
                    col = timeColorAt(tl.startTimeAt(i) / tot);      // 六档彩虹，按真实谱面时间
                }
                const glm::dvec2 a = toPx(prevRed), b = toPx(red);
                segs.push_back({a.x, a.y, b.x, b.y, col});
                const glm::dvec2 ab = toPx(prevBlue), bb = toPx(blue);
                segs.push_back({ab.x, ab.y, bb.x, bb.y, col});
            }
            prevRed = red; prevBlue = blue; havePrev = true;
        }
    }
    LOG_I("Map: %dx%d，%d 层，世界范围 %.1f x %.1f，缩放 %.4f px/单位，线宽 %.2f px，%zu 段",
          opts.width, opts.height, n, bw, bh, scale, halfW * 2.0, segs.size());

    const int bands = (int)((H + (size_t)bandRows - 1) / (size_t)bandRows);
    std::vector<std::vector<uint32_t>> bucket((size_t)bands);
    const double pad = halfW + 2.0;
    for (const Seg& sg : segs) {
        const double lo = std::min(sg.y0, sg.y1) - pad, hi = std::max(sg.y0, sg.y1) + pad;
        int b0 = (int)std::floor(lo / bandRows), b1 = (int)std::floor(hi / bandRows);
        b0 = std::max(0, b0); b1 = std::min(bands - 1, b1);
        for (int b = b0; b <= b1; ++b) bucket[(size_t)b].push_back((uint32_t)(&sg - segs.data()));
    }

    std::vector<uint8_t> band;
    for (int b = 0; b < bands; ++b) {
        const int y0 = b * bandRows;
        const int rows = (int)std::min<size_t>((size_t)bandRows, H - (size_t)y0);
        band.assign((size_t)W * rows * 4, 0);
        if (bg.a > 0.0f) {
            for (size_t p = 0; p < (size_t)W * rows; ++p) {
                band[p * 4 + 0] = (uint8_t)std::lround(bg.r * 255.0f);
                band[p * 4 + 1] = (uint8_t)std::lround(bg.g * 255.0f);
                band[p * 4 + 2] = (uint8_t)std::lround(bg.b * 255.0f);
                band[p * 4 + 3] = (uint8_t)std::lround(bg.a * 255.0f);
            }
        }
        for (uint32_t si : bucket[(size_t)b]) {
            const Seg& sg = segs[si];
            const Rgba c = unpack(sg.rgba);
            if (opts.hardLine)
                drawSegmentHard(band, (int)W, rows, sg.x0, sg.y0 - y0, sg.x1, sg.y1 - y0, c);
            else
                drawSegment(band, (int)W, rows, sg.x0, sg.y0 - y0, sg.x1, sg.y1 - y0, halfW, c);
        }
        if (!sink(y0, rows, band)) return false;
    }

    return true;
}
