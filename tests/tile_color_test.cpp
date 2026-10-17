// 砖色模型对拍：金标由 tests/gen_tile_color_gold.mjs 从 Re_ADOJAS 的逻辑生成。
// 覆盖六个 trackStyle 的 fill/stroke 派生 + 七种 trackColorType + 三种 pulse + Volume 幅度。
#include "core/level/TileColor.hpp"
#include "core/level/LevelData.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>

#include "tile_color_gold.inc"

using namespace adofai;

static std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static std::string hex3(const std::uint8_t c[3]) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x", c[0], c[1], c[2]);
    return buf;
}

int main() {
    int fail = 0;
    int n = 0;
    for (const auto& g : kTileColorGold) {
        TileColorConfig cfg;
        cfg.style = g.style;
        cfg.colorType = g.type;
        cfg.color = g.c1;
        cfg.secondary = g.c2;
        cfg.pulse = g.pulse;
        cfg.animDuration = g.anim;
        cfg.pulseLength = g.pulseLen;
        cfg.startFloor = g.startFloor;
        const TileColors out = computeTileColors(cfg, g.t, g.floor, g.amp);
        const std::string fill = hex3(out.fill), stroke = hex3(out.stroke);
        n++;
        if (fill != lower(g.fill) || stroke != lower(g.stroke)) {
            std::printf("FAIL %-9s %-8s t=%.2f floor=%d: got %s/%s want %s/%s\n",
                        g.style, g.type, g.t, g.floor, fill.c_str(), stroke.c_str(),
                        g.fill, g.stroke);
            fail++;
        }
    }

    // 不透明度：8 位 hex 的 AA / 255；6 位 = 1。
    {
        TileColorConfig cfg;
        cfg.color = "12345680";
        const TileColors out = computeTileColors(cfg, 0.0, 0);
        if (out.opacity < 127.9f / 255.0f || out.opacity > 128.1f / 255.0f) {
            std::printf("FAIL opacity: %f\n", (double)out.opacity);
            fail++;
        }
        cfg.color = "123456";
        if (computeTileColors(cfg, 0.0, 0).opacity != 1.0f) { std::printf("FAIL opacity 6-digit\n"); fail++; }
    }

    // 时间驱动的类型集合（决定"要不要逐帧重算"）。
    for (const char* t : {"Glow", "Blink", "Rainbow", "Volume"})
        if (!isTimeDrivenColorType(t)) { std::printf("FAIL time-driven %s\n", t); fail++; }
    for (const char* t : {"Single", "Stripes", "Switch", "Nope"})
        if (isTimeDrivenColorType(t)) { std::printf("FAIL static %s\n", t); fail++; }

    // settings → baseColorConfig（加载期只解析一次的那段）
    {
        const char* json = R"({"angleData":[0,90,180],"settings":{
            "bpm":120,"trackColor":"112233","secondaryTrackColor":"445566",
            "trackStyle":"Neon","trackColorType":"Blink","trackColorPulse":"Forward",
            "trackColorAnimDuration":0.5,"trackPulseLength":4,"trackGlowIntensity":33}})";
        LevelData lv;
        if (!lv.loadFromBuffer(json, std::strlen(json))) {
            std::printf("FAIL settings: load\n"); fail++;
        } else {
            const TileColorConfig cfg = lv.baseColorConfig();
            if (cfg.style != "Neon" || cfg.colorType != "Blink" || cfg.pulse != "Forward" ||
                cfg.animDuration != 0.5f || cfg.pulseLength != 4.0f ||
                cfg.glowIntensity != 33.0f || cfg.color != "112233" || cfg.secondary != "445566") {
                std::printf("FAIL settings: style=%s type=%s pulse=%s anim=%g len=%g glow=%g color=%s sec=%s\n",
                            cfg.style.c_str(), cfg.colorType.c_str(), cfg.pulse.c_str(),
                            (double)cfg.animDuration, (double)cfg.pulseLength,
                            (double)cfg.glowIntensity, cfg.color.c_str(), cfg.secondary.c_str());
                fail++;
            }
            // Neon + Single 本体 = 112233 → fill 000000 / stroke 112233
            TileColorConfig single = cfg;
            single.colorType = "Single";
            const TileColors out = computeTileColors(single, 0.0, 0);
            if (hex3(out.fill) != "000000" || hex3(out.stroke) != "112233") {
                std::printf("FAIL settings compute: %s/%s\n", hex3(out.fill).c_str(), hex3(out.stroke).c_str());
                fail++;
            }
        }
    }

    if (fail) {
        std::printf("%d 项失败（%d 例金标）\n", fail, n);
        return 1;
    }
    std::printf("ok 砖色模型：%d 例金标 + opacity + 时间驱动集合\n", n);
    return 0;
}
