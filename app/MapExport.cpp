#include "app/MapExport.hpp"

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/map/LevelMap.hpp"
#include "core/util/Logger.hpp"
#include "core/util/Progress.hpp"

#include "core/map/PngStream.hpp"
#include "core/map/PngBand.hpp"

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

#if defined(__APPLE__)
#include <pthread/qos.h>          // QoS → P/E 核（core 里不碰平台头，所以放在 app 层）
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

// 拼接用的瓦片（px 只在串行流式路径里懒加载；并行路径不保留解码结果）
struct StitchTile { long long x, y; int w, h; std::string path; unsigned char* px = nullptr; };

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
                   const std::string& tilesStr, bool native, bool timeColor, float padding,
                   float lineWidthPx) {
    LevelMapOptions opts;
    if (lineWidthPx > 0.0f) {                                 // --map-thickness：直接给线宽（像素）
        opts.lineWidthPx = lineWidthPx;
        if (lineWidthPx <= 1.0f) opts.hardLine = true;        // ≤1 px 只能走无 AA 硬线
    }
    if (padding >= 0.0f) opts.padding = padding;     // --map-padding 0 = 贴边（默认 0.02 = 四周留 2%）
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
    opts.timeColor = timeColor;
    if (!bgStr.empty()) {
        if (bgStr == "transparent" || bgStr == "none") opts.bgRgba = 0x00000000u;
        else opts.bgRgba = parseHex(bgStr, opts.bgRgba);
    }

    LevelData level;
    // 无头加载也要有反馈：巨谱的解析/解压能跑几十秒，以前这里是静默的。
    // CLI 进度行（stderr、\r 原地刷新、自带速率与 ETA），不碰 ImGui —— GUI 那条路仍走 ImGui。
    progress::reset();
    auto onLoad = [](float p, const char* what) {
        progress::update((long long)(p * 1000.0f), 1000, "%", what);
    };
    if (!level.loadFromFile(levelPath, onLoad)) {
        progress::finish();
        LOG_E("Map: 加载失败 %s", levelPath.c_str());
        std::fprintf(stderr, "Failed to load level: %s\n", levelPath.c_str());
        return 1;
    }
    progress::finish();
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
                            const std::string& tilesStr, int block, int threads, bool timeColor) {
    LevelData level;
    progress::reset();
    auto onLoadAll = [](float p, const char* what) {
        progress::update((long long)(p * 1000.0f), 1000, "%", what);
    };
    if (!level.loadFromFile(levelPath, onLoadAll)) {
        progress::finish();
        std::fprintf(stderr, "Failed to load level: %s\n", levelPath.c_str());
        return 1;
    }
    progress::finish();
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
        struct Seg { double x0, y0, x1, y1; uint32_t ti; };      // ti = 所属层序号（着色要用）
        struct Box { long long minX = 0, maxX = -1, minY = 0, maxY = -1; };
        struct Sample { long long y; double x; uint32_t ti; };    // 带上层序号，写像素时才能取时间
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
                    segs.push_back({x0, y0, x1, y1, (uint32_t)i});
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
                        if (xi >= tx * block && xi <= (tx + 1) * block - 1) samples.push_back({y, x, g.ti});
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
                            // TODO(timeColor)：这里的样本只带 (y,x)，取不到所属 tile 的时间 →
                            // 需要给 Sample 加一个 tile 序号/时间字段（3 处小改）再接 timeColorAt()。
                            const uint32_t tc = timeColor
                                ? timeColorAt(times[(size_t)samples[si].ti] / std::max(1e-9, tl.totalDuration()))
                                : 0xDEBB7BFFu;
                            const uint8_t tcR = (uint8_t)(tc >> 24), tcG = (uint8_t)(tc >> 16), tcB = (uint8_t)(tc >> 8);
                            row[off] = tcR; row[off + 1] = tcG; row[off + 2] = tcB;
                            if (lx + 1 < w) { row[off + 4] = tcR; row[off + 5] = tcG; row[off + 6] = tcB; }
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
// 并行拼接（scale == 1）。依据都是实测的，别改坏：
//   * 763 块瓦片两两不重叠（矩形相交对 = 0）→ 没有"谁覆盖谁"的歧义；
//   * 瓦片像素只有两类：不透明黑 0x000000FF 与墨色（半透明像素 0 个）→ 一行 = 背景 + 几个墨点；
//   * 拼接耗时 97% 是 deflate（最挤处 22.6 ms/行里组装只占 0.63 ms）→ 只有压缩值得并行。
// 于是先把"非背景像素"建成按行 CSR（9.05M 墨点 ≈ 72 MB），任意一行都能独立生产，
// 再交给 writePngParallel 分带并行压缩（与 1-bit 单张导出同一套已验证机制）。
// 像素与串行路径逐位一致（用 ADOCAO_STITCH_Y0/ROWS 取同一横条对拍验证过）。
// ─────────────────────────────────────────────────────────────────────────────
static int stitchNativeParallel(std::vector<StitchTile>& tiles, long long W, long long H,
                                long long y0, long long outH, const std::string& outPng,
                                int threads) {
    const uint32_t kBg = 0x000000FFu;                 // 与串行路径的填充一致：不透明黑
    std::vector<uint32_t> inkY, inkX, inkC;
    for (auto& t : tiles) {
        int w = 0, h = 0, c = 0;
        unsigned char* p = stbi_load(t.path.c_str(), &w, &h, &c, 4);
        if (!p) { std::fprintf(stderr, "  解码失败: %s\n", t.path.c_str()); continue; }
        for (int ry = 0; ry < h; ++ry) {
            const long long y = t.y + ry;
            if (y < y0 || y >= y0 + outH) continue;   // 被测试钩子裁掉的行不进索引
            const unsigned char* line = p + (size_t)ry * w * 4;
            for (int rx = 0; rx < w; ++rx) {
                const unsigned char* q = line + (size_t)rx * 4;
                const uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                                   ((uint32_t)q[2] << 8) | (uint32_t)q[3];
                if (v == kBg) continue;               // 背景（99.5% 的像素）不进索引
                inkY.push_back((uint32_t)y); inkX.push_back((uint32_t)(t.x + rx)); inkC.push_back(v);
            }
        }
        stbi_image_free(p);
    }
    std::printf("并行拼接：%zu 块 → %zu 个墨点，建按行索引\n", tiles.size(), inkY.size());
    std::vector<uint64_t> rowStart((size_t)(y0 + outH) + 1, 0);
    for (uint32_t y : inkY) rowStart[(size_t)y + 1]++;
    for (long long y = y0; y < y0 + outH; ++y) rowStart[(size_t)y + 1] += rowStart[(size_t)y];
    std::vector<uint32_t> xs(inkX.size()), cs(inkC.size());
    {
        std::vector<uint64_t> cur = rowStart;         // CSR 散射（按行计数排序）
        for (size_t i = 0; i < inkY.size(); ++i) {
            const uint64_t k = cur[(size_t)inkY[i]]++;
            xs[(size_t)k] = inkX[i]; cs[(size_t)k] = inkC[i];
        }
    }
    inkY.clear(); inkY.shrink_to_fit();
    inkX.clear(); inkX.shrink_to_fit();
    inkC.clear(); inkC.shrink_to_fit();

    const size_t rowBytes = (size_t)W * 4;
    std::vector<uint8_t> bgRow(rowBytes);
    for (size_t i = 0; i < rowBytes; i += 4) bgRow[i + 3] = 255;   // 不透明黑

    auto produced = std::make_shared<std::atomic<long long>>(0);
    auto produce = [&](long long ry, uint8_t* out) {
        std::memcpy(out, bgRow.data(), rowBytes);
        const long long y = y0 + ry;
        for (uint64_t k = rowStart[(size_t)y]; k < rowStart[(size_t)y + 1]; ++k) {
            uint8_t* d = out + (size_t)xs[(size_t)k] * 4;
            const uint32_t v = cs[(size_t)k];
            d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16);
            d[2] = (uint8_t)(v >> 8);  d[3] = (uint8_t)v;
        }
        progress::update(produced->fetch_add(1) + 1, outH, "行");
    };

    const int K = std::max(1, threads > 0 ? threads : 4);
    auto threadInit = [K](int idx) {
#if defined(__APPLE__)
        // 每个作业 2 P + 2 E（K=4）：前一半线程要性能核、后一半要效率核。
        // QoS 是 macOS 上唯一可靠的落核手段（本机实测 P/E 吞吐比 1.50x）。
        pthread_set_qos_class_self_np(idx * 2 < K ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
#else
        (void)idx;
#endif
    };
    // 小带：主线程必须按序写 band，2048 行的大带会被慢的 E 核 worker 拖住，
    // 让快的 P 核 worker 撞上 pendingCap 干等。同窗口 A/B 实测 256 行比 2048 行快 1.17x（CPU 相同）。
    const int bandRows = 256;                         // ~7.6 KB/行 → 每带约 2 MB 压缩数据
    progress::reset();
    const bool ok = writePngParallel(outPng, (uint32_t)W, (uint32_t)outH, 8, 6, nullptr, 0,
                                     bandRows, K, produce, 0, threadInit);
    progress::finish();
    if (!ok) { std::fprintf(stderr, "拼接：并行写出失败 %s\n", outPng.c_str()); return 1; }
    std::printf("拼接完成（并行 %d 线程 = 2 P + 2 E）：%s（%lld x %lld）\n",
                K, outPng.c_str(), W, outH);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 流式拼接（回退路径）：按 y 顺序逐行输出，活动块解码一次、离开它的 y 范围就释放。
// 峰值内存 ≈ 同时活动的几块（每块 w*h*4）+ 两行缓冲，与总画布无关。
// ADOCAO_STITCH_SEQ=1 或 scale > 1 走这里。
// ─────────────────────────────────────────────────────────────────────────────
int stitchLevelMapTiles(const std::string& dir, const std::string& outPng, int scale, int threads) {
    namespace fs = std::filesystem;
    if (scale < 1) scale = 1;
    std::vector<StitchTile> tiles;
    long long W = 0, H = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        long long x, y, w, h;
        if (std::sscanf(fn.c_str(), "x%lld_y%lld_w%lld_h%lld.png", &x, &y, &w, &h) != 4) continue;
        StitchTile t; t.x = x; t.y = y; t.w = (int)w; t.h = (int)h; t.path = e.path().string();
        W = std::max(W, x + w); H = std::max(H, y + h);
        tiles.push_back(std::move(t));
    }
    if (tiles.empty()) { std::fprintf(stderr, "拼图：%s 里没找到分块\n", dir.c_str()); return 1; }
    std::sort(tiles.begin(), tiles.end(), [](const StitchTile& a, const StitchTile& b) { return a.y < b.y; });
    // 测试钩子（仅 scale == 1）：只输出源行区间 [y0, y0+rows)，用来和串行路径逐行对拍
    long long y0 = 0, rows = H;
    bool forceSeq = false;
    if (scale == 1) {
        if (const char* s = std::getenv("ADOCAO_STITCH_Y0"))   y0 = std::atoll(s);
        if (const char* s = std::getenv("ADOCAO_STITCH_ROWS")) rows = std::atoll(s);
        if (const char* s = std::getenv("ADOCAO_STITCH_SEQ"))  forceSeq = std::atoi(s) != 0;
        y0 = std::max(0LL, std::min(y0, H));
        rows = std::max(0LL, std::min(rows, H - y0));
    }
    const long long outW = (W + scale - 1) / scale;
    const long long outH = (scale == 1) ? rows : (H + scale - 1) / scale;
    if (scale == 1 && !forceSeq)
        return stitchNativeParallel(tiles, W, H, y0, outH, outPng, threads);
    progress::reset();
    std::printf("拼接：%zu 块 → 画布 %lld x %lld px，输出 %lld x %lld（scale %d，串行流式）\n",
                tiles.size(), W, H, outW, outH, scale);
    PngStreamWriter ws;
    if (!ws.open(outPng, (uint32_t)outW, (uint32_t)outH)) return 1;
    std::vector<uint8_t> out((size_t)outW * 4, 0);
    std::vector<uint32_t> acc;                       // 降采样累加（每像素 RGB 各 32 位打包）
    if (scale > 1) acc.assign((size_t)outW * 3, 0);
    std::vector<uint8_t> full((size_t)W * 4, 0);     // 原生一行的拼装缓冲（7.8 MB @1.94M）
    size_t ti = 0;
    long long rowsIn = 0;
    const long long loopBegin = (scale == 1) ? y0 : 0;              // scale==1：只走选中的横条
    const long long loopEnd   = (scale == 1) ? y0 + outH : H;
    for (long long y = loopBegin; y < loopEnd; ++y) {
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
        progress::update(y - loopBegin + 1, loopEnd - loopBegin, "行");
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
    progress::finish();
    if (!ws.close()) { std::fprintf(stderr, "拼接：写出失败 %s\n", outPng.c_str()); return 1; }
    std::printf("拼接完成：%s（%lld x %lld）\n", outPng.c_str(), outW, outH);
    return 0;
}

// 索引色 1 bit 的整张导出：像素直接取自瓦片（与已验证产物一致）。
int exportLevelMapMono1(const std::string& tileDir, const std::string& outPng, int threads) {
    namespace fs = std::filesystem;
    struct Tile { long long x, y; int w, h; std::string path; };
    std::vector<Tile> tiles;
    long long W = 0, H = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(tileDir, ec)) {
        const std::string fn = e.path().filename().string();
        long long x, y, w, h;
        if (std::sscanf(fn.c_str(), "x%lld_y%lld_w%lld_h%lld.png", &x, &y, &w, &h) != 4) continue;
        tiles.push_back({x, y, (int)w, (int)h, e.path().string()});
        W = std::max(W, x + w); H = std::max(H, y + h);
    }
    if (tiles.empty()) { std::fprintf(stderr, "1bit: %s 里没有瓦片\n", tileDir.c_str()); return 1; }
    std::printf("1bit: %zu 块 → 画布 %lld x %lld px\n", tiles.size(), W, H);

    std::vector<uint32_t> ys, xs;
    for (auto& t : tiles) {
        int w = 0, h = 0, c = 0;
        unsigned char* p = stbi_load(t.path.c_str(), &w, &h, &c, 4);
        if (!p) { std::fprintf(stderr, "  解码失败: %s\n", t.path.c_str()); continue; }
        for (int ry = 0; ry < h; ++ry) {
            const unsigned char* line = p + (size_t)ry * w * 4;
            for (int rx = 0; rx < w; ++rx) {
                const unsigned char* q = line + (size_t)rx * 4;
                if (q[0] || q[1] || q[2]) { ys.push_back((uint32_t)(t.y + ry)); xs.push_back((uint32_t)(t.x + rx)); }
            }
        }
        stbi_image_free(p);
    }
    // CSR：按 y 把墨点排好，任意行 O(1) 取到 —— 这样每个 worker 都能独立产出任意行
    std::printf("  墨像素 %zu，建按行的 CSR 索引\n", ys.size());
    std::vector<uint64_t> rowStart((size_t)H + 1, 0);
    for (uint32_t y : ys) rowStart[(size_t)y + 1]++;
    for (long long y = 0; y < H; ++y) rowStart[(size_t)y + 1] += rowStart[(size_t)y];
    std::vector<uint32_t> xsSorted(xs.size());
    {
        std::vector<uint64_t> cur = rowStart;
        for (size_t i = 0; i < ys.size(); ++i) xsSorted[(size_t)cur[ys[i]]++] = xs[i];
    }
    ys.clear(); ys.shrink_to_fit(); xs.clear(); xs.shrink_to_fit();

    const size_t rowBytes = (size_t)((W + 7) / 8);
    auto produced = std::make_shared<std::atomic<long long>>(0);
    auto produce = [&](long long y, uint8_t* out) {
        std::memset(out, 0, rowBytes);
        for (uint64_t k = rowStart[(size_t)y]; k < rowStart[(size_t)y + 1]; ++k) {
            const uint32_t x = xsSorted[(size_t)k];
            out[x >> 3] |= (uint8_t)(1u << (7 - (x & 7)));
        }
        const long long done = produced->fetch_add(1) + 1;
        progress::update(done, (long long)H, "行");
    };
    // 索引 0 = 黑（背景），索引 1 = 轨道色（与瓦片同一颜色）
    const uint8_t pal[6] = { 0, 0, 0, 222, 187, 123 };
    const int bandRows = 8192;
    progress::reset();
    const bool okPar = writePngParallel(outPng, (uint32_t)W, (uint32_t)H, 1, 3, pal, sizeof pal,
                                        bandRows, threads, produce);
    progress::finish();
    if (!okPar) {
        std::fprintf(stderr, "1bit: 并行写出失败 %s\n", outPng.c_str());
        return 1;
    }
    std::printf("1bit 完成：%s（%lld x %lld，1 bit/px，%d 行/带，线程 %d）\n",
                outPng.c_str(), W, H, bandRows, threads);
    return 0;
}
