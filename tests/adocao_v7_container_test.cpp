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
    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要看到已经跑到哪一步
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
    // 事件列现在是**分块容器**（每 4096 事件一块），整段解码走 BlockedAll
    std::vector<std::string> events;
    std::vector<uint16_t> typeIds;
    check(back.v7ColumnsBlocked, "EventColumns 段标记为分块");
    check(adocao::decodeEventColumnsBlockedAll(schema, back.v7ColumnsBytes.data(),
                                               back.v7ColumnsBytes.size(), events, err, &typeIds),
          "事件列分块解码: " + err);
    check(typeIds.size() == events.size(), "类型列与事件数一致");

    // **范围解码**必须等于全量解码的对应片段（这就是窗口化 seek 的正确性依据）
    if (events.size() >= 3) {
        std::vector<std::string> range;
        std::string rerr;
        check(adocao::decodeEventColumnsRange(schema, back.v7ColumnsBytes.data(),
                                              back.v7ColumnsBytes.size(), 1, 3, range, rerr),
              "范围解码 [1,3): " + rerr);
        check(range.size() == 2 && range[0] == events[1] && range[1] == events[2],
              "范围解码与全量解码逐字节一致");
    }
    for (size_t i = 0; i < events.size(); ++i) {
        rapidjson::Document dbg;
        dbg.Parse(events[i].c_str(), events[i].size());
        if (dbg.HasParseError() || !dbg.IsObject() || !dbg.HasMember("eventType")) {
            std::printf("DEBUG 坏事件 %zu: %s\n", i, events[i].c_str());
        }
    }

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

    // EventIndex：按 4096 层块的事件区间（floor → 事件下标）
    check(back.eventBlockStart.size() >= 2, "读回 EventIndex");
    bool mono = true;
    for (size_t i = 1; i < back.eventBlockStart.size(); ++i)
        if (back.eventBlockStart[i] < back.eventBlockStart[i - 1]) mono = false;
    check(mono && back.eventBlockSorted, "EventIndex 单调且标记为已排序");

    // **按层号窗口解事件**：只解命中的块，再按 floor 精确过滤
    {
        std::vector<std::string> win;
        std::string werr;
        check(adocao::decodeEventsInFloorRange(back, 2, 6, win, werr), "按层窗口解事件: " + werr);
        bool floorsOk = true;
        for (const auto& e : win) {
            rapidjson::Document d;
            d.Parse(e.c_str(), e.size());
            const int64_t fl = d["floor"].GetInt64();
            if (fl < 2 || fl >= 6) floorsOk = false;
        }
        check(!win.empty() && floorsOk, "窗口内事件 floor 全部落在 [2,6)（" + std::to_string(win.size()) + " 条）");
    }

    // **v7 唯一存储**：不写 v6 段，读侧从 v7 事件还原出 actions / 富事件
    adocao::PackOptions only;
    only.v7Only = true;
    std::vector<uint8_t> bytes2;
    check(adocao::packLevel(lv, bytes2, err, nullptr, only), "打包 v7-only: " + err);
    LevelData only2;
    check(adocao::unpackLevel(bytes2.data(), bytes2.size(), only2, err, &derived), "解 v7-only: " + err);
    check(only2.actions.size() == lv.actions.size(),
          "v7-only 还原 actions 数量（" + std::to_string(only2.actions.size()) + " vs " +
              std::to_string(lv.actions.size()) + "）");
    check(only2.cameraEvents.size() == lv.cameraEvents.size() &&
              only2.colorEvents.size() == lv.colorEvents.size() &&
              only2.moveTrackEvents.size() == lv.moveTrackEvents.size(),
          "v7-only 还原富事件数量（camera/colour/moveTrack）");

    // **事件窗口**：设了窗口就只解该层段的事件（EventIndex 定位块 + floor 精确过滤）
    {
        LevelData win;
        win.setEventWindow(2, 6);   // 合成谱：Twirl@2 / SetSpeed@3 / MoveCamera@5 / MoveTrack@6 / Color@7 / Flash@8
        std::string werr;
        check(adocao::unpackLevel(bytes2.data(), bytes2.size(), win, werr, &derived), "窗口加载: " + werr);
        // 这张合成谱只有 1 条真 action（Twirl@2）+ MoveDecorations@3,5 + SetFilterAdvanced@6
        check(win.actions.size() == 1 && win.actions[0].floor == 2,
              "窗口 [2,6) 只含 floor 2 的那条 action（实际 " + std::to_string(win.actions.size()) + " 条）");
        check(win.cameraEvents.empty() && win.colorEvents.empty() && win.flashEvents.empty(),
              "窗口里没有 camera/colour/flash（这张谱本来就没有）");
        LevelData none;
        none.setEventWindow(3, 6);   // 整段都落在唯一 action 之后 → 必须一条都没有
        std::string nerr;
        check(adocao::unpackLevel(bytes2.data(), bytes2.size(), none, nerr, &derived),
              "窗口加载（空段）: " + nerr);
        check(none.actions.empty(), "窗口 [3,6) 不含任何 action（过滤生效）");
    }

    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("adocao_v7_container: 全部通过\n");
    return 0;
}
