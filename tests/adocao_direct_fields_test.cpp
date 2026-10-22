// v7 直接喂字段（不经 JSON）与 JSON 路径的**对拍**：
// 同一份 action JSON，一条路走 `loadFromString`（JSON 解析），另一条路把字段拆成
// `EventFieldValue` 再 `ingestEventFields`，两边产出的 LevelData 必须一致。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "core/level/LevelData.hpp"

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}

static void addNum(std::vector<LevelData::EventFieldValue>& out, const char* name, double v) {
    LevelData::EventFieldValue f;
    f.role = LevelData::fieldRoleForName(name, std::strlen(name));
    f.type = LevelData::EventFieldValue::Num;
    f.num = v;
    out.push_back(f);
}
// 注意：**必须存字面量的裸指针**（生命周期 = 整个测试）。早先版本收 `const std::string&`
// 并用 `v.data()`，临时串在语句结束就析构 → 悬垂指针，eventType 变成随机值。
static void addStr(std::vector<LevelData::EventFieldValue>& out, const char* name, const char* v) {
    LevelData::EventFieldValue f;
    f.role = LevelData::fieldRoleForName(name, std::strlen(name));
    f.type = LevelData::EventFieldValue::Str;
    f.str = v;
    f.len = (uint32_t)std::strlen(v);
    out.push_back(f);
}
static void addBool(std::vector<LevelData::EventFieldValue>& out, const char* name, bool v) {
    LevelData::EventFieldValue f;
    f.role = LevelData::fieldRoleForName(name, std::strlen(name));
    f.type = LevelData::EventFieldValue::Bool;
    f.b = v;
    out.push_back(f);
}
static void addPair(std::vector<LevelData::EventFieldValue>& out, const char* name,
                    double a, bool aNull, double c, bool cNull, const char* str = "") {
    LevelData::EventFieldValue f;
    f.role = LevelData::fieldRoleForName(name, std::strlen(name));
    f.type = LevelData::EventFieldValue::Pair;
    f.a = a; f.c = c; f.nullA = aNull ? 1 : 0; f.nullC = cNull ? 1 : 0;
    if (str && *str) { f.str = str; f.len = (uint32_t)std::strlen(str); }
    out.push_back(f);
}

int main() {
    const char* json = R"({
      "angleData":[0,90,0,90,0,90,0,90,0,90],
      "settings":{"bpm":120,"offset":0,"countdownTicks":4},
      "actions":[
        {"floor":2,"eventType":"Twirl"},
        {"floor":3,"eventType":"SetSpeed","speedType":"Multiplier","bpmMultiplier":1.5},
        {"floor":5,"eventType":"MoveCamera","duration":1,"ease":"OutCubic","zoom":150,
         "position":[null,3],"relativeTo":"Player","angleOffset":90,"eventTag":"cam1"},
        {"floor":6,"eventType":"MoveTrack","startTile":[3,"ThisTile"],"endTile":[9,"ThisTile"],
         "gapLength":1,"duration":2,"positionOffset":[2,-1],"rotationOffset":180,
         "scale":[150,150],"opacity":50,"ease":"InQuad"},
        {"floor":7,"eventType":"ColorTrack","trackColor":"ff0000","trackColorType":"Single",
         "trackStyle":"Neon","trackColorAnimDuration":2,"trackPulseLength":10,"trackGlowIntensity":25},
        {"floor":8,"eventType":"Flash","plane":"Foreground","startColor":"ffffff",
         "endColor":"000000","startOpacity":100,"endOpacity":0,"duration":0.5,"ease":"Linear"}
      ]
    })";

    LevelData viaJson;
    check(viaJson.loadFromString(json), "JSON 路径加载");
    const size_t n = viaJson.actions.size();
    std::printf("   JSON 路径: actions=%zu camera=%zu colour=%zu flash=%zu moveTrack=%zu\n",
                n, viaJson.cameraEvents.size(), viaJson.colorEvents.size(),
                viaJson.flashEvents.size(), viaJson.moveTrackEvents.size());

    // 直接喂字段（与上面每条事件一一对应）
    LevelData viaFields;
    std::string err;
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 2); addStr(f, "eventType", "Twirl");
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 Twirl: " + err);
    }
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 3); addStr(f, "eventType", "SetSpeed");
        addStr(f, "speedType", "Multiplier"); addNum(f, "bpmMultiplier", 1.5);
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 SetSpeed: " + err);
    }
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 5); addStr(f, "eventType", "MoveCamera");
        addNum(f, "duration", 1); addStr(f, "ease", "OutCubic"); addNum(f, "zoom", 150);
        addPair(f, "position", 0, true, 3, false);
        addStr(f, "relativeTo", "Player"); addNum(f, "angleOffset", 90); addStr(f, "eventTag", "cam1");
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 MoveCamera: " + err);
    }
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 6); addStr(f, "eventType", "MoveTrack");
        addPair(f, "startTile", 3, false, 0, false, "ThisTile");
        addPair(f, "endTile", 9, false, 0, false, "ThisTile");
        addNum(f, "gapLength", 1); addNum(f, "duration", 2);
        addPair(f, "positionOffset", 2, false, -1, false);
        addNum(f, "rotationOffset", 180); addPair(f, "scale", 150, false, 150, false);
        addNum(f, "opacity", 50); addStr(f, "ease", "InQuad");
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 MoveTrack: " + err);
    }
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 7); addStr(f, "eventType", "ColorTrack");
        addStr(f, "trackColor", "ff0000"); addStr(f, "trackColorType", "Single");
        addStr(f, "trackStyle", "Neon"); addNum(f, "trackColorAnimDuration", 2);
        addNum(f, "trackPulseLength", 10); addNum(f, "trackGlowIntensity", 25);
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 ColorTrack: " + err);
    }
    {
        std::vector<LevelData::EventFieldValue> f;
        addNum(f, "floor", 8); addStr(f, "eventType", "Flash");
        addStr(f, "plane", "Foreground"); addStr(f, "startColor", "ffffff");
        addStr(f, "endColor", "000000"); addNum(f, "startOpacity", 100);
        addNum(f, "endOpacity", 0); addNum(f, "duration", 0.5); addStr(f, "ease", "Linear");
        check(viaFields.ingestEventFields(f.data(), f.size(), err), "字段路径 Flash: " + err);
    }

    std::printf("   字段路径: actions=%zu camera=%zu colour=%zu flash=%zu moveTrack=%zu\n",
                viaFields.actions.size(), viaFields.cameraEvents.size(), viaFields.colorEvents.size(),
                viaFields.flashEvents.size(), viaFields.moveTrackEvents.size());
    check(viaFields.actions.size() == viaJson.actions.size(), "actions 数量一致");
    check(viaFields.cameraEvents.size() == viaJson.cameraEvents.size(), "camera 数量一致");
    check(viaFields.moveTrackEvents.size() == viaJson.moveTrackEvents.size(), "moveTrack 数量一致");
    check(viaFields.colorEvents.size() == viaJson.colorEvents.size(), "colour 数量一致");
    check(viaFields.flashEvents.size() == viaJson.flashEvents.size(), "flash 数量一致");
    bool same = viaFields.actions.size() == viaJson.actions.size();
    for (size_t i = 0; i < viaFields.actions.size() && same; ++i) {
        const auto& a = viaFields.actions[i];
        const auto& b = viaJson.actions[i];
        same = a.floor == b.floor && a.type == b.type && a.flag == b.flag &&
               a.val1 == b.val1 && a.val2 == b.val2 && a.strId == b.strId;
    }
    check(same, "FastAction 逐字段一致");
    if (viaFields.cameraEvents.size() == viaJson.cameraEvents.size() && !viaJson.cameraEvents.empty()) {
        const auto& a = viaFields.cameraEvents[0];
        const auto& b = viaJson.cameraEvents[0];
        check(a.floor == b.floor && a.zoom == b.zoom && a.x == b.x && a.y == b.y &&
                  a.rotation == b.rotation && a.relativeTo == b.relativeTo &&
                  a.angleOffset == b.angleOffset && a.flags == b.flags && a.tag == b.tag,
              "CameraEvent 关键字段一致");
    }
    if (viaFields.moveTrackEvents.size() == viaJson.moveTrackEvents.size() && !viaJson.moveTrackEvents.empty()) {
        const auto& a = viaFields.moveTrackEvents[0];
        const auto& b = viaJson.moveTrackEvents[0];
        check(a.floor == b.floor && a.startOff == b.startOff && a.startRel == b.startRel &&
                  a.endOff == b.endOff && a.endRel == b.endRel && a.gap == b.gap &&
                  a.durBeats == b.durBeats && a.ease == b.ease && a.flags == b.flags &&
                  a.offX == b.offX && a.offY == b.offY && a.rotDeg == b.rotDeg &&
                  a.scaleX == b.scaleX && a.opacity == b.opacity,
              "MoveTrackEvent 关键字段一致");
    }
    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("adocao_direct_fields: 全部通过\n");
    return 0;
}
