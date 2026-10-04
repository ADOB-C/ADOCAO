#include "app/MapExport.hpp"

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "core/map/LevelMap.hpp"
#include "core/util/Logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
                   const std::string& sizeStr, const std::string& bgStr) {
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
    std::printf("map %dx%d -> %s (%d tiles)\n", opts.width, opts.height,
                outPath.c_str(), (int)level.tiles.size());
    return 0;
}
