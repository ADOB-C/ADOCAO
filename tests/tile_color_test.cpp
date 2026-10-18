// 砖色模型对拍：金标由 tests/gen_tile_color_gold.mjs 从 Re_ADOJAS 的逻辑生成。
// 覆盖六个 trackStyle 的 fill/stroke 派生 + 七种 trackColorType + 三种 pulse + Volume 幅度。
#include "core/level/TileColor.hpp"
#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

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

    // ColorTrack 查询语义：持久段 / justThisTile 覆盖 / inactive 跳过
    {
        const char* json = R"({"angleData":[0,90,180,270,0,90],"settings":{
            "bpm":120,"trackStyle":"Minimal","trackColor":"101010"},
            "actions":[
              {"floor":1,"eventType":"ColorTrack","trackColor":"FF0000","trackStyle":"Standard"},
              {"floor":2,"eventType":"ColorTrack","trackColor":"00FF00","justThisTile":true},
              {"floor":3,"eventType":"ColorTrack","trackColor":"0000FF","active":false}]})";
        LevelData lv;
        if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL colors: load\n"); fail++; }
        else if (lv.colorEvents.size() != 3) {
            std::printf("FAIL colors: %zu events\n", lv.colorEvents.size()); fail++;
        } else {
            auto hexAt = [&](int f) { const TileColors c = lv.tileColorsAt(f, 0.0); return hex3(c.fill) + "/" + hex3(c.stroke); };
            if (lv.colorEventAt(0) != nullptr) { std::printf("FAIL colors: floor0 should be default\n"); fail++; }
            if (hexAt(0) != "101010/101010") { std::printf("FAIL colors floor0: %s\n", hexAt(0).c_str()); fail++; }
            if (hexAt(1) != "ff0000/b30000") { std::printf("FAIL colors floor1: %s\n", hexAt(1).c_str()); fail++; }
            if (hexAt(2) != "00ff00/00b300") { std::printf("FAIL colors floor2 (justThisTile): %s\n", hexAt(2).c_str()); fail++; }
            if (hexAt(3) != "ff0000/b30000") { std::printf("FAIL colors floor3 (inactive skipped): %s\n", hexAt(3).c_str()); fail++; }
            if (hexAt(5) != "ff0000/b30000") { std::printf("FAIL colors floor5: %s\n", hexAt(5).c_str()); fail++; }
        }
    }

    // TweenColor：duration 是**拍**（× 该层时长 = 秒），start = startTimeAt(floor)；
    // 生效前显示上一段目标色，生效中按 ease 插值，结束后到位。
    {
        const char* json = R"({"angleData":[0,90,0,90,0,90,0,90],"settings":{
            "bpm":120,"trackColor":"FF0000","trackColorType":"Single","trackStyle":"Standard"},
            "actions":[
              {"floor":2,"eventType":"ColorTrack","trackColor":"00FF00","justThisTile":true,
               "duration":1,"ease":"Linear"},
              {"floor":4,"eventType":"ColorTrack","trackColor":"0000FF","duration":2,"ease":"Linear"}]})";
        LevelData lv;
        Timeline tl;
        if (!lv.loadFromBuffer(json, std::strlen(json))) { std::printf("FAIL tween: load\n"); fail++; }
        else {
            tl.build(lv, false);
            auto col = [&](int f, double t) {
                const TileColors c = lv.tileColorsAt(f, t, 0.0f, &tl);
                return hex3(c.fill) + "/" + hex3(c.stroke);
            };
            // 用 timeline 自己的时间（不同角度的砖拍数不同，别硬编码）
            const double beat4 = tl.durationAt(4), start4 = tl.startTimeAt(4);
            const double dur4 = 2.0 * beat4;
            if (col(5, start4 - 0.05) != "ff0000/b30000") {
                std::printf("FAIL tween before: %s\n", col(5, start4 - 0.05).c_str()); fail++;
            }
            if (col(5, start4 + dur4 * 0.5) != "800080/5a005a") {
                std::printf("FAIL tween mid: %s\n", col(5, start4 + dur4 * 0.5).c_str()); fail++;
            }
            if (col(5, start4 + dur4 + 0.05) != "0000ff/0000b3") {
                std::printf("FAIL tween after: %s\n", col(5, start4 + dur4 + 0.05).c_str()); fail++;
            }
            if (col(3, start4 + dur4 * 0.5) != "ff0000/b30000") {
                std::printf("FAIL tween range: %s\n", col(3, start4 + dur4 * 0.5).c_str()); fail++;
            }
            // justThisTile：只影响这一层，且自己也有 fade（1 拍）
            const double beat2 = tl.durationAt(2), start2 = tl.startTimeAt(2);
            const double dur2 = 1.0 * beat2;
            if (col(2, start2 - 0.05) != "ff0000/b30000") {
                std::printf("FAIL jtt before: %s\n", col(2, start2 - 0.05).c_str()); fail++;
            }
            if (col(2, start2 + dur2 * 0.5) != "808000/5a5a00") {
                std::printf("FAIL jtt mid: %s\n", col(2, start2 + dur2 * 0.5).c_str()); fail++;
            }
            if (col(2, start2 + dur2 + 0.05) != "00ff00/00b300") {
                std::printf("FAIL jtt after: %s\n", col(2, start2 + dur2 + 0.05).c_str()); fail++;
            }
            if (col(3, start2 + dur2 * 0.5) != "ff0000/b30000") {
                std::printf("FAIL jtt leak: %s\n", col(3, start2 + dur2 * 0.5).c_str()); fail++;
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
