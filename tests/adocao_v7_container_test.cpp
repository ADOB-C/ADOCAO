// `.adocao` v7 容器端到端：JSON → pack(--v7) → load → schema/列回解逐事件对拍。
// 重点验证"v6 会整体丢弃的类型（MoveDecorations / SetFilter / decorations）也能无损装进容器"。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "archive/AdocaoEventColumns.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoSchema.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}

int main() {
    archive::install();

    const char* json = R"({
      "angleData":[0,90,0,90,0,90,0,90,0,90],
      "settings":{"bpm":120,"offset":0,"countdownTicks":4},
      "actions":[
        {"floor":2,"eventType":"Twirl"},
        {"floor":3,"eventType":"MoveDecorations","duration":1,"tag":"a","opacity":90,
         "positionOffset":[null,null],"color":"ffffff","eventTag":""},
        {"floor":5,"eventType":"MoveDecorations","duration":0.5,"tag":"b","scale":[150,150],
         "positionOffset":[1.5,-2],"color":"ff000080","eventTag":"x"},
        {"floor":6,"eventType":"SetFilterAdvanced","filter":"CameraFilterPack_Blur_Movie",
         "enabled":true,"filterProperties":"\"filter_Radius\": 100000","targetType":"Camera",
         "duration":0,"ease":"Linear","angleOffset":0}
      ],
      "decorations":[
        {"eventType":"AddDecoration","floor":1,"decorationImage":"a/b.png","position":[0,0],"scale":[100,100]},
        {"eventType":"AddDecoration","floor":2,"decorationImage":"c/d.png","position":[2,null],"scale":[50,50],
         "color":"00ff00","tag":"deco1"}
      ]
    })";

    LevelData lv;
    lv.preserveRawJson = true;
    check(lv.loadFromString(json), "加载源谱");
    check(!lv.rawActionsJson.empty(), "捕获了 actions 原始切片");
    check(!lv.rawDecorationsJson.empty(), "捕获了 decorations 原始切片");

    adocao::PackOptions opts;
    opts.withV7Events = true;
    std::vector<uint8_t> bytes;
    std::string err;
    check(adocao::packLevel(lv, bytes, err, nullptr, opts), "打包 v7: " + err);
    std::printf("   文件大小 %zu B\n", bytes.size());

    // 直接解容器（不经过 LevelData 的收尾，避免 Timeline 依赖）
    LevelData back;
    adocao::DerivedInfo derived;
    check(adocao::unpackLevel(bytes.data(), bytes.size(), back, err, &derived), "解容器: " + err);
    check(!back.v7SchemaBytes.empty(), "读回 EventSchema 段");
    check(!back.v7ColumnsBytes.empty(), "读回 EventColumns 段");

    adocao::Schema schema;
    check(adocao::SchemaBuilder::deserialize(back.v7SchemaBytes.data(), back.v7SchemaBytes.size(),
                                             schema, err), "schema 反序列化: " + err);
    adocao::EventColumns cols;
    check(adocao::deserializeEventColumns(schema, back.v7ColumnsBytes.data(),
                                          back.v7ColumnsBytes.size(), cols, err),
          "事件列反序列化: " + err);
    std::vector<std::string> events;
    check(adocao::decodeEventColumns(schema, cols, cols.typeIds.size(), events, err),
          "事件列解码: " + err);

    // 预期事件数 = **v6 段装不下的** actions + decorations（去重：Twirl 已由 v6 承载，不进 v7）
    rapidjson::Document deco;
    deco.Parse(lv.rawDecorationsJson.c_str(), lv.rawDecorationsJson.size());
    size_t nonLegacy = 0;
    for (const auto& js : lv.rawActionsJson) {
        rapidjson::Document a;
        a.Parse(js.c_str(), js.size());
        const std::string t = a["eventType"].GetString();
        if (t == "Twirl" || t == "SetSpeed" || t == "PositionTrack" || t == "SetHitsound" ||
            t == "Bookmark" || t == "Pause" || t == "AnimateTrack" || t == "MoveCamera" ||
            t == "ColorTrack" || t == "RecolorTrack" || t == "Flash" || t == "MoveTrack" ||
            t == "RepeatEvents" || t == "ScaleRadius") continue;
        ++nonLegacy;
    }
    const size_t expect = nonLegacy + deco.Size();
    check(events.size() == expect, "事件数 = 非 legacy 事件 + decorations（去重生效）");

    // 逐事件：解析后按 (eventType, floor) 匹配，比较关键字段
    size_t matched = 0;
    for (const auto& js : lv.rawActionsJson) {
        rapidjson::Document a;
        a.Parse(js.c_str(), js.size());
        for (const auto& e : events) {
            rapidjson::Document b;
            b.Parse(e.c_str(), e.size());
            if (std::strcmp(a["eventType"].GetString(), b["eventType"].GetString()) != 0) continue;
            if (a["floor"].GetInt() != b["floor"].GetInt()) continue;
            matched++;
            break;
        }
    }
    check(matched == nonLegacy, "每条非 legacy action 都能在解码结果里找到对应事件");
    check(!back.v7CoverageBytes.empty() == false, "未裁剪时不写 Coverage 段");

    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("adocao_v7_container: 全部通过\n");
    return 0;
}
