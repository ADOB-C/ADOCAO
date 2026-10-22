// v7 自描述 schema 的用例：
//   * kind 推断（Enum/Color/Int/GridFloat/Pair/Nested/Bool）
//   * 三态（缺失 / null / 值）在 flags 上分得开
//   * 序列化往返逐字段相同、且同输入两次构建**逐字节相同**（打包确定性）
//   * 扩展性：语料里不存在的新类型自动出现在 schema 里（容器代码 0 行）
#include "archive/AdocaoSchema.hpp"

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

struct Built { Schema schema; std::vector<uint8_t> bytes; };

static Built build(const char* json) {
    rapidjson::Document d;
    d.Parse(json);
    SchemaBuilder b;
    for (auto it = d.Begin(); it != d.End(); ++it)
        b.addEvent((*it)["eventType"].GetString(), EventSource::Actions, *it);
    b.finalize();
    return Built{b.schema(), b.serialize()};
}

static const FieldDesc* findField(const Schema& s, const char* type, const char* field) {
    for (const auto& t : s.types) {
        if (s.name(t.nameId) != type) continue;
        for (uint16_t i = 0; i < t.fieldCount; ++i) {
            const auto& f = s.fields[t.firstField + i];
            if (s.name(f.nameId) == field) return &f;
        }
    }
    return nullptr;
}

int main() {
    const char* json = R"([
      {"eventType":"MoveDecorations","floor":1,"tag":"a","opacity":90,"scale":[100,100],
       "positionOffset":[null,null],"color":"ffffff","visible":true,"eventTag":""},
      {"eventType":"MoveDecorations","floor":5,"tag":"b","opacity":45,"scale":[50,50],
       "positionOffset":[1.25,-3.5],"color":"ff000080","visible":false,"eventTag":"x"},
      {"eventType":"SetFilterAdvanced","floor":3,"filter":"CameraFilterPack_Blur_Movie",
       "enabled":true,"filterProperties":"\"filter_Radius\": 100000","targetType":"Camera"},
      {"eventType":"FakeNewEvent","floor":9,"tag":"z","scale":[1,2],"nested":{"a":1,"b":2},
       "nullable":null}
    ])";
    const Built built = build(json);
    const Schema& s = built.schema;

    check(s.types.size() == 3, "发现 3 个类型（含语料里不存在的 FakeNewEvent）");

    auto kind = [&](const char* t, const char* f, Kind want, const char* what) -> const FieldDesc* {
        const FieldDesc* fd = findField(s, t, f);
        const bool ok = fd && fd->kind == want;
        if (!ok && fd) std::printf("     （%s.%s 实际 kind=%d，期望 %d）\n", t, f, (int)fd->kind, (int)want);
        check(ok, what);
        return fd;
    };
    const FieldDesc* tagFd = kind("MoveDecorations", "tag", Kind::Enum, "tag → Enum（少取值短串）");
    kind("MoveDecorations", "opacity", Kind::Int, "opacity → Int");
    kind("MoveDecorations", "scale", Kind::Pair, "scale → Pair");
    kind("MoveDecorations", "positionOffset", Kind::Pair, "positionOffset → Pair");
    kind("MoveDecorations", "color", Kind::Color, "color → Color（6/8 位 hex）");
    kind("MoveDecorations", "visible", Kind::Bool, "visible → Bool");
    kind("SetFilterAdvanced", "filterProperties", Kind::Enum, "filterProperties 是字符串列（不是 JSON 对象）");
    kind("FakeNewEvent", "nested", Kind::Nested, "未知类型里的对象 → Nested");
    check(tagFd && (tagFd->flags & 0x1) == 0, "tag 没有 null");
    const FieldDesc* po = findField(s, "MoveDecorations", "positionOffset");
    check(po && (po->flags & 0x8) != 0, "positionOffset 标记了'分量出现 null'（[null,null] 是分量级）");
    check(po && (po->flags & 0x2) == 0, "positionOffset 每条都写了（无缺失标记）");
    const FieldDesc* nl = findField(s, "FakeNewEvent", "nullable");
    check(nl && (nl->flags & 0x4) != 0, "整列都是 null 的字段单独标记");
    kind("MoveDecorations", "floor", Kind::Int, "floor → Int");

    Schema back;
    std::string err;
    check(SchemaBuilder::deserialize(built.bytes.data(), built.bytes.size(), back, err), "反序列化成功");
    check(back.types.size() == s.types.size() && back.fields.size() == s.fields.size(),
          "往返后类型/字段数量一致");
    bool same = back.names == s.names;
    for (size_t i = 0; i < s.fields.size() && same; ++i)
        same = back.fields[i].kind == s.fields[i].kind &&
               back.fields[i].flags == s.fields[i].flags &&
               back.fields[i].gridK == s.fields[i].gridK &&
               back.name(back.fields[i].nameId) == s.name(s.fields[i].nameId);
    check(same, "往返后逐字段相同");

    const Built again = build(json);
    check(again.bytes == built.bytes, "同输入两次构建逐字节相同（打包确定性）");

    std::vector<uint8_t> bad = built.bytes;
    bad.resize(bad.size() / 2);
    Schema junk;
    check(!SchemaBuilder::deserialize(bad.data(), bad.size(), junk, err), "截断的 schema 必须显式失败");

    if (g_fail) { std::printf("%d 项失败\n", g_fail); return 1; }
    std::printf("adocao_schema: 全部通过\n");
    return 0;
}
