// v7 事件列：建列 → 解列 → 逐字段对拍（语义相等：数字按 double 位、字符串逐字节）。
// 覆盖：缺失 / null / pair 分量 null / 6-8 位颜色 / 嵌套对象 / 非 ASCII / 稀疏字段。
#include "archive/AdocaoEventColumns.hpp"
#include "archive/AdocaoSchema.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <rapidjson/document.h>

using namespace adofai::adocao;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}

// 语义比较：对象成员集合相同、逐值相等（数字按 double 位比较）
static bool sameValue(const rapidjson::Value& a, const rapidjson::Value& b);
static bool sameObject(const rapidjson::Value& a, const rapidjson::Value& b) {
    if (a.MemberCount() != b.MemberCount()) return false;
    for (auto it = a.MemberBegin(); it != a.MemberEnd(); ++it) {
        auto o = b.FindMember(it->name);
        if (o == b.MemberEnd()) return false;
        if (!sameValue(it->value, o->value)) return false;
    }
    return true;
}
static bool sameValue(const rapidjson::Value& a, const rapidjson::Value& b) {
    if (a.GetType() != b.GetType()) return false;
    switch (a.GetType()) {
        case rapidjson::kNullType: return true;
        case rapidjson::kFalseType:
        case rapidjson::kTrueType: return a.GetBool() == b.GetBool();
        case rapidjson::kNumberType: {
            const double x = a.GetDouble(), y = b.GetDouble();
            return std::memcmp(&x, &y, sizeof x) == 0;   // 位相等
        }
        case rapidjson::kStringType:
            return a.GetStringLength() == b.GetStringLength() &&
                   std::memcmp(a.GetString(), b.GetString(), a.GetStringLength()) == 0;
        case rapidjson::kArrayType:
            if (a.Size() != b.Size()) return false;
            for (rapidjson::SizeType i = 0; i < a.Size(); ++i)
                if (!sameValue(a[i], b[i])) return false;
            return true;
        case rapidjson::kObjectType: return sameObject(a, b);
    }
    return false;
}

int main() {
    std::vector<std::string> events = {
        R"({"eventType":"MoveDecorations","floor":1,"tag":"a","opacity":90,"scale":[100,100],)"
        R"("positionOffset":[null,null],"color":"ffffff","visible":true,"eventTag":""})",
        R"({"eventType":"MoveDecorations","floor":5,"tag":"b","opacity":45,"scale":[50.5,50.5],)"
        R"("positionOffset":[1.25,-3.5],"color":"#ff000080","visible":false,"eventTag":"x"})",
        R"({"eventType":"MoveDecorations","floor":7,"tag":"日本語","nested":{"a":1,"b":[2,3]}})",
        R"({"eventType":"SetFilterAdvanced","floor":3,"filter":"CameraFilterPack_Blur_Movie",)"
        R"("enabled":true,"filterProperties":"\"filter_Radius\": 100000","targetType":"Camera"})",
        R"({"eventType":"Deco","floor":2,"position":[1.5,null],"color":"ff0000","image":"a/b.png"})",
    };
    std::vector<uint8_t> sources = {0, 0, 0, 0, 1};   // 最后一条来自 decorations

    SchemaBuilder sb;
    for (size_t i = 0; i < events.size(); ++i) {
        rapidjson::Document d;
        d.Parse(events[i].c_str(), events[i].size());
        sb.addEvent(d["eventType"].GetString(), (EventSource)sources[i], d);
    }
    sb.finalize();
    const Schema& schema = sb.schema();

    EventColumns cols;
    std::string err;
    check(buildEventColumns(schema, events, sources, cols, err), "建列成功" + std::string(err.empty() ? "" : "：" + err));

    std::vector<std::string> back;
    check(decodeEventColumns(schema, cols, events.size(), back, err), "解列成功" + std::string(err.empty() ? "" : "：" + err));

    bool allSame = true;
    for (size_t i = 0; i < events.size(); ++i) {
        rapidjson::Document a, b;
        a.Parse(events[i].c_str(), events[i].size());
        b.Parse(back[i].c_str(), back[i].size());
        if (!sameObject(a, b)) {
            std::printf("FAIL 事件 %zu 往返不等\n  原: %s\n  回: %s\n", i, events[i].c_str(), back[i].c_str());
            allSame = false;
            ++g_fail;
        }
    }
    check(allSame, "全部事件语义往返相等（缺失/null/分量 null/颜色/嵌套/非 ASCII）");

    // 负向：截断的 payload 必须显式失败
    EventColumns broken = cols;
    if (!broken.fields.empty()) broken.fields[0].payload.resize(broken.fields[0].payload.size() / 2);
    std::vector<std::string> junk;
    check(!decodeEventColumns(schema, broken, events.size(), junk, err), "截断的列必须显式失败");

    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("adocao_event_columns: 全部通过\n");
    return 0;
}
