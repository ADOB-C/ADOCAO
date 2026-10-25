// 粘性轨道（`stickToFloors`）的回归测试。
//
// 语义（Re_ADOJAS `PositionTrackManager` + `Player.updatePlanetsPosition`）：
//   stick = true（默认）→ 行星**跟着**被 PositionTrack 移动后的砖位走；
//   stick = false     → 行星走**原始**砖位（轨道飘走、行星不跟）。
//
// 两边都不能只看代码：这里直接断言
//   tilePositionAt(i)  = 移动后的砖位（渲染用）
//   planetPivotAt(i)   = stick=false 时去掉 PositionTrack 偏移的原始位置（行星用）
#include <cmath>
#include <cstdio>
#include <string>

#include "core/level/LevelData.hpp"

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}
static void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol;
    if (!ok) std::printf("FAIL %s（got %.4f want %.4f）\n", what.c_str(), got, want);
    else std::printf("ok   %s\n", what.c_str());
}

static bool writeChart(const char* path, const char* stickSetting, const char* eventStick,
                       bool withEvent) {
    std::string json = R"({
      "angleData": [0, 90, 0, 90, 0, 90, 0, 90],
      "settings": {"bpm": 120, "offset": 0, "countdownTicks": 0, "pitch": 100,
                   "trackColor": "debb7b", "secondaryTrackColor": "ffffff",
                   "backgroundColor": "000000", "trackStyle": "Standard",
                   "trackAnimation": "None", "beatsAhead": 0,
                   "trackDisappearAnimation": "None", "beatsBehind": 0,
                   "stickToFloors": )";
    json += stickSetting;
    json += R"(},
      "actions": [)";
    if (withEvent) {
        json += R"({"floor": 3, "eventType": "PositionTrack", "positionOffset": [5, 0],
         "relativeTo": [0, "ThisTile"], "rotation": 0, "scale": 100, "opacity": 100,
         "justThisTile": "Disabled", "editorOnly": "Disabled", "stickToFloors": )";
        json += eventStick;
        json += "}";
    }
    json += R"(]
    })";
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        return true;
    }
    return false;
}

static void caseRun(const char* path, const char* stickSetting, const char* eventStick,
                    bool expectStickyFrom3) {
    // 基准：同谱、**不带** PositionTrack 事件
    const std::string basePath = std::string(path) + ".base";
    if (!writeChart(basePath.c_str(), stickSetting, eventStick, false) ||
        !writeChart(path, stickSetting, eventStick, true)) {
        std::printf("FAIL 写谱\n"); ++g_fail; return;
    }
    LevelData lv, base;
    if (!lv.loadFromFile(path, nullptr) || !base.loadFromFile(basePath.c_str(), nullptr)) {
        std::printf("FAIL 加载 %s\n", path); ++g_fail; return;
    }

    const auto p3 = lv.tilePositionAt(3);
    const auto p5 = lv.tilePositionAt(5);
    const auto b3 = base.tilePositionAt(3);
    const auto b5 = base.tilePositionAt(5);
    checkNear(p3[0] - b3[0], 5.0, 1e-3, "tilePositionAt(3) = 基准 + 5（渲染用，事件生效）");
    checkNear(p5[0] - b5[0], 5.0, 1e-3, "之后各层继承该偏移");

    double x5 = 0.0, y5 = 0.0;
    lv.planetPivotAt(5, x5, y5);
    if (expectStickyFrom3) {
        checkNear(x5, p5[0], 1e-3, "stick=on：行星枢轴 = 移动后的砖位");
        check(lv.stickyAt(5), "stickyAt(5) = true");
    } else {
        checkNear(x5, b5[0], 1e-3, "stick=off：行星枢轴 = 原始（基准）砖位");
        check(!lv.stickyAt(5), "stickyAt(5) = false");
    }
    checkNear(y5, p5[1], 1e-3, "y 不受 x 偏移影响");
}

int main() {
    // ① settings 关掉，事件也不写 → 从 0 层起就不粘
    caseRun("sticky_settings_off.adofai", "false", "\"Disabled\"", false);
    // ② settings 开着，事件在某层显式关掉 → 从那一层起不粘
    caseRun("sticky_event_off.adofai", "true", "\"Disabled\"", false);
    // ③ 默认（都开） → 一直粘
    caseRun("sticky_on.adofai", "true", "\"Enabled\"", true);

    std::printf("sticky_track: %s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}
