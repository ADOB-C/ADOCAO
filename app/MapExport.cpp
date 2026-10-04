#include "app/MapExport.hpp"

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/map/LevelMap.hpp"
#include "core/util/Logger.hpp"

#include "core/map/PngStream.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "core/timeline/PositionSolver.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

uint32_t parseHex(const std::string& h, uint32_t fallback) {
    std::string s = h;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return fallback;
    uint32_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return fallback;
    }
    return s.size() == 6 ? (v << 8) | 0xFFu : v;   // 6 位 → 补满 alpha
}

}  // namespace

int exportLevelMap(const std::string& levelPath, const std::string& outPath,
                   const std::string& sizeStr, const std::string& bgStr,
                   const std::string& tilesStr, bool native) {
    LevelMapOptions opts;
    if (!sizeStr.empty()) {
        int w = 0, h = 0;
        if (std::sscanf(sizeStr.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            opts.width = w; opts.height = h;
        } else {
            LOG_W("Map: 无法解析 --map-size '%s'（应为 WxH），用默认 %dx%d",
                  sizeStr.c_str(), opts.width, opts.height);
        }
    }
    if (!tilesStr.empty()) {
        long long a = 0, b = -1;
        if (std::sscanf(tilesStr.c_str(), "%lld-%lld", &a, &b) == 2) {
            opts.firstTile = a; opts.lastTile = b;
        } else {
            LOG_W("Map: 无法解析 --map-tiles '%s'（应为 A-B）", tilesStr.c_str());
        }
    }
    opts.nativeScale = native;
    if (!bgStr.empty()) {
        if (bgStr == "transparent" || bgStr == "none") opts.bgRgba = 0x00000000u;
        else opts.bgRgba = parseHex(bgStr, opts.bgRgba);
    }

    LevelData level;
    if (!level.loadFromFile(levelPath)) {
        LOG_E("Map: 加载失败 %s", levelPath.c_str());
        std::fprintf(stderr, "Failed to load level: %s\n", levelPath.c_str());
        return 1;
    }
    Timeline timeline;
    timeline.build(level, /*exportOnly=*/true);
    LOG_I("Map: %d 层，开始出图 %dx%d", (int)level.tiles.size(), opts.width, opts.height);

    std::vector<uint8_t> rgba;
    if (!renderLevelMap(timeline, opts, rgba)) {
        std::fprintf(stderr, "Map rendering failed\n");
        return 1;
    }
    if (!stbi_write_png(outPath.c_str(), opts.width, opts.height, 4,
                        rgba.data(), opts.width * 4)) {
        std::fprintf(stderr, "Failed to write PNG: %s\n", outPath.c_str());
        return 1;
    }
    LOG_I("Map: 已写出 %s", outPath.c_str());
    std::printf("map %dx%d -> %s (%d tiles%s%s)\n", opts.width, opts.height,
                outPath.c_str(), (int)level.tiles.size(),
                opts.nativeScale ? ", native 1px=1unit" : "",
                (opts.firstTile || opts.lastTile >= 0) ? ", cropped" : "");
    return 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// 整谱 1 像素 = 1 层：流式分块写出
//
// 画布 = 包围盒（1 单位 = 1 像素）：MYC 是 194 万 x 78.5 万 px，整张 1.5 万亿像素，
// 攒缓冲不可能 —— 所以按 4096² 分块、每块一条独立 zlib 流（PngStreamWriter）顺序写，
// 内存只与"打开着的块"有关。逐层扫描本身很便宜（677 万层 70 ms），所以：
//   每个 slab（画布 4096 行）先把落在其中的线段收集起来 → 按 y 排序 → 顺序流过每一行，
//   每行只写一次；某列第一次有墨时才开对应的块（并把之前的空行补上）。
// slab 之间互不相干 → 多线程直接按 slab 分发。
// ─────────────────────────────────────────────────────────────────────────────
int exportLevelMapNativeAll(const std::string& levelPath, const std::string& outDir,
                            const std::string& tilesStr, int block, int threads) {
    LevelData level;
    if (!level.loadFromFile(levelPath)) {
        std::fprintf(stderr, "Failed to load level: %s\n", levelPath.c_str());
        return 1;
    }
    Timeline tl;
    tl.build(level, /*exportOnly=*/true);
    const int n = (int)level.tiles.size();
    const auto& times = tl.tileStartTimes();
    if (n < 2 || times.size() < (size_t)n) { std::fprintf(stderr, "bad level\n"); return 1; }

    long long a = 0, b = n - 1;
    if (!tilesStr.empty()) std::sscanf(tilesStr.c_str(), "%lld-%lld", &a, &b);
    a = std::max(0LL, a); b = std::min<long long>(n - 1, b);
    if (b <= a) { std::fprintf(stderr, "empty tile range\n"); return 1; }

    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    glm::dvec2 red, blue;
    for (long long i = a; i <= b; ++i) {
        PositionSolver::positionAtTile(tl, times[(size_t)i], (int)i, red, blue);
        mnx = std::min(mnx, std::min(red.x, blue.x)); mxx = std::max(mxx, std::max(red.x, blue.x));
        mny = std::min(mny, std::min(red.y, blue.y)); mxy = std::max(mxy, std::max(red.y, blue.y));
    }
    const long long W = (long long)std::ceil(mxx - mnx) + 3;
    const long long H = (long long)std::ceil(mxy - mny) + 3;
    const double offX = -mnx + 1.0, offY = mxy + 1.0;
    const int NSLAB = (int)((H + block - 1) / block);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    std::printf("native-all: 画布 %lld x %lld px（1 像素 = 1 层）, %d slab, 裁剪到墨迹包围盒\n",
                W, H, NSLAB);

    std::atomic<int> next{0};
    std::atomic<long long> written{0}, pixels{0};
    auto worker = [&]() {
        // slab 内的一条线段（设备坐标）
        struct Seg { double x0, y0, x1, y1; };
        struct Box { long long minX = 0, maxX = -1, minY = 0, maxY = -1; };
        struct Sample { long long y; double x; };
        std::vector<Seg> segs;
        for (;;) {
            const int s = next.fetch_add(1);
            if (s >= NSLAB) break;
            const long long sy0 = (long long)s * block;
            const long long sy1 = std::min<long long>(H - 1, sy0 + block - 1);
            segs.clear();
            glm::dvec2 pr, pb, cr, cb;
            PositionSolver::positionAtTile(tl, times[(size_t)a], (int)a, pr, pb);
            for (long long i = a + 1; i <= b; ++i) {
                PositionSolver::positionAtTile(tl, times[(size_t)i], (int)i, cr, cb);
                const glm::dvec2 pairs[2][2] = { {pr, cr}, {pb, cb} };
                for (auto& q : pairs) {
                    const double x0 = q[0].x + offX, x1 = q[1].x + offX;
                    const double y0 = offY - q[0].y, y1 = offY - q[1].y;
                    if (std::max(y0, y1) < (double)sy0 || std::min(y0, y1) > (double)sy1) continue;
                    segs.push_back({x0, y0, x1, y1});
                }
                pr = cr; pb = cb;
            }
            if (segs.empty()) continue;

            // ① 先算每一列（tx）里墨迹的包围盒 —— 这就是"裁掉纯黑"的关键
            std::unordered_map<long long, Box> boxes;
            for (const Seg& g : segs) {
                const long long xa = std::max(0LL, (long long)std::floor(std::min(g.x0, g.x1)) - 1);
                const long long xb = std::min(W - 1, (long long)std::ceil(std::max(g.x0, g.x1)) + 1);
                const long long ya = std::max(sy0, (long long)std::floor(std::min(g.y0, g.y1)));
                const long long yb = std::min(sy1, (long long)std::ceil(std::max(g.y0, g.y1)));
                if (xa > xb || ya > yb) continue;
                for (long long tx = xa / block; tx <= xb / block; ++tx) {
                    Box& bx = boxes[tx];
                    const long long cx0 = std::max(xa, tx * block), cx1 = std::min(xb, (tx + 1) * block - 1);
                    if (bx.maxX < 0) { bx = {cx0, cx1, ya, yb}; }
                    else {
                        bx.minX = std::min(bx.minX, cx0); bx.maxX = std::max(bx.maxX, cx1);
                        bx.minY = std::min(bx.minY, ya);  bx.maxY = std::max(bx.maxY, yb);
                    }
                }
            }

            // ② 逐列流式写出裁剪后的图
            for (auto& kv : boxes) {
                const long long tx = kv.first;
                Box& bx = kv.second;
                const long long w = bx.maxX - bx.minX + 1, h = bx.maxY - bx.minY + 1;
                if (w <= 0 || h <= 0) continue;
                std::vector<Sample> samples;
                for (const Seg& g : segs) {
                    const long long xa = (long long)std::floor(std::min(g.x0, g.x1));
                    const long long xb = (long long)std::ceil(std::max(g.x0, g.x1));
                    if (xb < tx * block || xa > (tx + 1) * block - 1) continue;   // 不属于这一列
                    const double gy0 = std::min(g.y0, g.y1), gy1 = std::max(g.y0, g.y1);
                    const long long ya = std::max(bx.minY, (long long)std::floor(gy0));
                    const long long yb = std::min(bx.maxY, (long long)std::ceil(gy1));
                    const double dy = g.y1 - g.y0;
                    for (long long y = ya; y <= yb; ++y) {
                        const double t = (dy == 0.0) ? 0.0 : ((double)y + 0.5 - g.y0) / dy;
                        const double x = g.x0 + (g.x1 - g.x0) * (t < 0 ? 0 : (t > 1 ? 1 : t));
                        const long long xi = (long long)x;
                        if (xi >= tx * block && xi <= (tx + 1) * block - 1) samples.push_back({y, x});
                    }
                }
                std::sort(samples.begin(), samples.end(),
                          [](const Sample& u, const Sample& v) { return u.y < v.y; });
                char name[512];
                std::snprintf(name, sizeof name, "%s/x%07lld_y%07lld_w%lld_h%lld.png",
                              outDir.c_str(), bx.minX, bx.minY, w, h);
                PngStreamWriter ws;
                if (!ws.open(name, (uint32_t)w, (uint32_t)h)) continue;
                std::vector<uint8_t> row((size_t)w * 4, 0);
                for (size_t k = 3; k < row.size(); k += 4) row[k] = 255;
                size_t si = 0;
                for (long long y = bx.minY; y <= bx.maxY; ++y) {
                    std::fill(row.begin(), row.end(), 0);
                    for (size_t k = 3; k < row.size(); k += 4) row[k] = 255;
                    while (si < samples.size() && samples[si].y == y) {
                        const long long lx = (long long)samples[si].x - bx.minX;
                        if (lx >= 0 && lx < w) {
                            const size_t off = (size_t)lx * 4;
                            row[off] = 222; row[off + 1] = 187; row[off + 2] = 123;
                            if (lx + 1 < w) { row[off + 4] = 222; row[off + 5] = 187; row[off + 6] = 123; }
                        }
                        ++si;
                    }
                    ws.writeRow(row.data());
                }
                if (ws.close()) { written.fetch_add(1); pixels.fetch_add(w * h); }
            }
            if ((s % 24) == 0) std::printf("  slab %d/%d（已写块 %lld）\n", s, NSLAB, written.load());
        }
    };

    const int T = std::max(1, threads > 0 ? threads : (int)std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    LevelMapOptions ov;
    ov.width = ov.height = 4096;
    ov.firstTile = a; ov.lastTile = b;
    std::vector<uint8_t> rgba;
    if (renderLevelMap(tl, ov, rgba) &&
        stbi_write_png((outDir + "/_overview.png").c_str(), ov.width, ov.height, 4,
                       rgba.data(), ov.width * 4)) {
        std::printf("  缩略索引: %s/_overview.png\n", outDir.c_str());
    } else {
        std::fprintf(stderr, "  ⚠ 缩略索引写出失败\n");
    }
    std::printf("native-all 完成：%lld 个块, 共 %.1f 万像素, 画布 %lld x %lld\n",
                written.load(), (double)pixels.load() / 1e4, W, H);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 流式拼接：按 y 顺序逐行输出，活动块解码一次、离开它的 y 范围就释放。
// 峰值内存 ≈ 同时活动的几块（每块 w*h*4）+ 两行缓冲，与总画布无关。
// ─────────────────────────────────────────────────────────────────────────────
int stitchLevelMapTiles(const std::string& dir, const std::string& outPng, int scale) {
    namespace fs = std::filesystem;
    if (scale < 1) scale = 1;
    struct Tile { long long x, y; int w, h; std::string path; unsigned char* px = nullptr; };
    std::vector<Tile> tiles;
    long long W = 0, H = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        long long x, y, w, h;
        if (std::sscanf(fn.c_str(), "x%lld_y%lld_w%lld_h%lld.png", &x, &y, &w, &h) != 4) continue;
        Tile t; t.x = x; t.y = y; t.w = (int)w; t.h = (int)h; t.path = e.path().string();
        W = std::max(W, x + w); H = std::max(H, y + h);
        tiles.push_back(std::move(t));
    }
    if (tiles.empty()) { std::fprintf(stderr, "拼图：%s 里没找到分块\n", dir.c_str()); return 1; }
    std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return a.y < b.y; });
    const long long outW = (W + scale - 1) / scale, outH = (H + scale - 1) / scale;
    std::printf("拼接：%zu 块 → 画布 %lld x %lld px，输出 %lld x %lld（scale %d, 降采样=max）\n",
                tiles.size(), W, H, outW, outH, scale);
    PngStreamWriter ws;
    if (!ws.open(outPng, (uint32_t)outW, (uint32_t)outH)) return 1;
    std::vector<uint8_t> out((size_t)outW * 4, 0);
    std::vector<uint32_t> acc;                       // 降采样累加（每像素 RGB 各 32 位打包）
    if (scale > 1) acc.assign((size_t)outW * 3, 0);
    std::vector<uint8_t> full((size_t)W * 4, 0);     // 原生一行的拼装缓冲（7.8 MB @1.94M）
    size_t ti = 0;
    long long rowsIn = 0;
    for (long long y = 0; y < H; ++y) {
        // 该行的活动块：先解码所有 y 覆盖本行的块
        for (auto& t : tiles) {
            if (t.px || y < t.y || y >= t.y + t.h) continue;
            int cw = 0, ch = 0, comp = 0;
            t.px = stbi_load(t.path.c_str(), &cw, &ch, &comp, 4);
            if (!t.px) { std::fprintf(stderr, "  解码失败: %s\n", t.path.c_str()); }
        }
        if (scale == 1) {
            std::fill(full.begin(), full.end(), 0);
            for (size_t k = 3; k < full.size(); k += 4) full[k] = 255;
        }
        for (auto& t : tiles) {
            if (!t.px || y < t.y || y >= t.y + t.h) continue;
            const long long ry = y - t.y;
            const unsigned char* src = t.px + (size_t)ry * t.w * 4;
            if (scale == 1) {
                for (int i = 0; i < t.w; ++i) {
                    const long long ox = t.x + i;
                    if (ox < 0 || ox >= W) continue;
                    std::memcpy(&full[(size_t)ox * 4], src + (size_t)i * 4, 4);
                }
            } else {
                for (int i = 0; i < t.w; ++i) {
                    const long long ox = (t.x + i) / scale;
                    if (ox < 0 || ox >= outW) continue;
                    const unsigned char* sp = src + (size_t)i * 4;
                    // max（有墨就点到）—— 1 像素宽的线用平均会被稀释成 1/scale²，直接消失
                    uint32_t& r0 = acc[(size_t)ox * 3 + 0];
                    uint32_t& g0 = acc[(size_t)ox * 3 + 1];
                    uint32_t& b0 = acc[(size_t)ox * 3 + 2];
                    if (sp[0] > r0) r0 = sp[0];
                    if (sp[1] > g0) g0 = sp[1];
                    if (sp[2] > b0) b0 = sp[2];
                }
            }
        }
        ++rowsIn;
        if (scale == 1) {
            ws.writeRow(full.data());
        } else if (rowsIn == scale) {
            for (long long i = 0; i < outW; ++i) {
                out[(size_t)i * 4 + 0] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 0]);
                out[(size_t)i * 4 + 1] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 1]);
                out[(size_t)i * 4 + 2] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 2]);
                out[(size_t)i * 4 + 3] = 255;
            }
            ws.writeRow(out.data());
            std::fill(acc.begin(), acc.end(), 0);
            rowsIn = 0;
        }
        // 释放已经走完的块
        for (auto& t : tiles) {
            if (t.px && y >= t.y + t.h - 1) { stbi_image_free(t.px); t.px = nullptr; }
        }
        if ((y % 50000) == 0 && y) std::printf("  拼到第 %lld / %lld 行\n", y, H);
    }
    if (scale > 1 && rowsIn > 0) {                    // 余数行
        for (long long i = 0; i < outW; ++i) {
            out[(size_t)i * 4 + 0] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 0]);
            out[(size_t)i * 4 + 1] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 1]);
            out[(size_t)i * 4 + 2] = (uint8_t)std::min<uint32_t>(255, acc[(size_t)i * 3 + 2]);
            out[(size_t)i * 4 + 3] = 255;
        }
        ws.writeRow(out.data());
    }
    if (!ws.close()) { std::fprintf(stderr, "拼接：写出失败 %s\n", outPng.c_str()); return 1; }
    std::printf("拼接完成：%s（%lld x %lld）\n", outPng.c_str(), outW, outH);
    return 0;
}
