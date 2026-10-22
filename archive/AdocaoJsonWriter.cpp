#include "archive/AdocaoJsonWriter.hpp"

#include <cstring>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "archive/AdocaoEventColumns.hpp"
#include "archive/AdocaoSchema.hpp"

namespace adofai {
namespace adocao {

bool writeLevelJson(const LevelData& lv, std::string& out, std::string& err) {
    err.clear();
    out.clear();
    if (lv.v7ColumnsBytes.empty() || lv.v7SchemaBytes.empty()) {
        err = "没有 v7 事件段：请用 `adocao pack --v7-all` 打包后再回写（旧文件不保证完整）";
        return false;
    }
    Schema schema;
    if (!SchemaBuilder::deserialize(lv.v7SchemaBytes.data(), lv.v7SchemaBytes.size(), schema, err)) return false;
    std::vector<std::string> events;
    std::vector<uint16_t> typeIds;
    if (lv.v7ColumnsBlocked) {
        if (!decodeEventColumnsBlockedAll(schema, lv.v7ColumnsBytes.data(), lv.v7ColumnsBytes.size(),
                                          events, err, &typeIds)) return false;
    } else {
        EventColumns cols;
        if (!deserializeEventColumns(schema, lv.v7ColumnsBytes.data(), lv.v7ColumnsBytes.size(), cols, err)) return false;
        if (!decodeEventColumns(schema, cols, cols.typeIds.size(), events, err)) return false;
        typeIds = cols.typeIds;
    }

    // 按 schema 的 source 分回 actions / decorations（解码结果的顺序 = 原顺序，但两类是混排的）
    std::vector<size_t> actionIdx, decoIdx;
    for (size_t i = 0; i < typeIds.size(); ++i) {
        const auto& td = schema.types[typeIds[i]];
        (td.source == (uint8_t)EventSource::Decorations ? decoIdx : actionIdx).push_back(i);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();

    // ---- settings ----
    // 有原始切片就**原样输出**（ADOFAI 的设置键远多于我们建模的那几十个；逐键建模必然丢字段）。
    if (!lv.rawSettingsJson.empty()) {
        w.Key("settings");
        w.RawValue(lv.rawSettingsJson.c_str(), (rapidjson::SizeType)lv.rawSettingsJson.size(),
                   rapidjson::kObjectType);
        const auto& s2 = lv.settings;
        (void)s2;
    } else {
    const auto& s = lv.settings;
    w.Key("settings");
    w.StartObject();
    auto num = [&](const char* k, double v) { w.Key(k); w.Double(v); };
    auto str = [&](const char* k, const std::string& v) { w.Key(k); w.String(v.c_str()); };
    auto boolean = [&](const char* k, bool v) { w.Key(k); w.Bool(v); };
    w.Key("version"); w.Int(s.version);
    num("bpm", s.bpm);
    num("offset", s.offset);
    w.Key("countdownTicks"); w.Int(s.countdownTicks);
    boolean("separateCountdownTime", s.separateCountdownTime);
    num("zoom", s.zoom);
    num("rotation", s.rotation);
    str("relativeTo", s.relativeTo);
    w.Key("position");
    w.StartArray(); w.Double(s.position[0]); w.Double(s.position[1]); w.EndArray();
    str("song", s.songFilename);
    str("hitsound", s.hitsound);
    num("hitsoundVolume", s.hitsoundVolume);
    str("trackColor", s.trackColor);
    str("secondaryTrackColor", s.secondaryTrackColor);
    str("trackColorType", s.trackColorType);
    str("trackColorPulse", s.trackColorPulse);
    num("trackColorAnimDuration", s.trackColorAnimDuration);
    num("trackPulseLength", s.trackPulseLength);
    str("trackStyle", s.trackStyle);
    num("trackGlowIntensity", s.trackGlowIntensity);
    str("backgroundColor", s.backgroundColor);
    boolean("stickToFloors", s.stickToFloors);
    str("planetEase", s.planetEase);
    str("trackDisappearAnimation", s.trackDisappearAnimation);
    str("trackAnimation", s.trackAnimation);
    num("beatsBehind", s.beatsBehind);
    num("beatsAhead", s.beatsAhead);
    num("pitch", s.pitch);
    boolean("legacyCamRelativeTo", s.legacyCamRelativeTo);
    w.EndObject();
    }

    // ---- angleData / pathData ----
    if (!lv.pathData.empty()) {
        w.Key("pathData");
        w.String(lv.pathData.c_str(), (rapidjson::SizeType)lv.pathData.size());
    } else {
        w.Key("angleData");
        w.StartArray();
        for (double a : lv.angleData) w.Double(a);
        w.EndArray();
    }

    // ---- actions / decorations（v7 解码出的原样 JSON 对象）----
    auto emitList = [&](const char* key, const std::vector<size_t>& idx) {
        w.Key(key);
        w.StartArray();
        for (size_t i : idx) w.RawValue(events[i].c_str(), (rapidjson::SizeType)events[i].size(), rapidjson::kObjectType);
        w.EndArray();
    };
    emitList("actions", actionIdx);
    if (!decoIdx.empty()) emitList("decorations", decoIdx);

    w.EndObject();
    out.assign(buf.GetString(), buf.GetSize());
    return true;
}

}  // namespace adocao
}  // namespace adofai
