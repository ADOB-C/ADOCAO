#include "archive/AdocaoSchema.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include <rapidjson/document.h>
#include <cstdio>

#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {
namespace {

void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)(v >> 8));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
bool getU16(const uint8_t*& p, const uint8_t* e, uint16_t& v) {
    if (e - p < 2) return false;
    v = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    return true;
}
bool getU32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
    if (e - p < 4) return false;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return true;
}

}  // namespace

struct SchemaBuilder::Acc {
    uint32_t present = 0;
    uint32_t nulls = 0;
    uint32_t total = 0;
    bool sawInt = false, sawFloat = false, sawString = false, sawBool = false, sawColor = false;
    bool sawPair = false, sawObject = false, sawArrayOther = false;
    bool pairHasNull = false;   // [null,x] / [x,null] / [null,null]：分量级 null（编码时拆两列各带 null 掩码）
    bool allTagLike = true;
    size_t maxLen = 0;
    std::unordered_set<std::string> distinct;   // 有上限（见 finalize 前的截断）
    bool distinctOverflow = false;
    bool gridOk[7] = {true, true, true, true, true, true, true};   // 10^0..10^6
};

SchemaBuilder::~SchemaBuilder() = default;

SchemaBuilder::SchemaBuilder() {
    m_schema.names.emplace_back();   // [0] = ""
    m_typeIndex.assign(2 * 4096, -1);
}

uint16_t SchemaBuilder::internName(std::string_view s) {
    for (size_t i = 0; i < m_schema.names.size(); ++i)
        if (m_schema.names[i] == s) return (uint16_t)i;
    m_schema.names.emplace_back(s);
    return (uint16_t)(m_schema.names.size() - 1);
}

uint32_t SchemaBuilder::ensureField(uint16_t typeIdx, uint16_t nameId) {
    const uint64_t key = ((uint64_t)typeIdx << 32) | nameId;
    for (const auto& kv : m_fieldIndex)
        if (kv.first == key) return kv.second;
    const uint32_t idx = (uint32_t)m_schema.fields.size();
    FieldDesc fd;
    fd.nameId = nameId;
    m_schema.fields.push_back(fd);
    m_acc.emplace_back();
    m_fieldIndex.emplace_back(key, idx);
    auto& t = m_schema.types[typeIdx];
    if (t.fieldCount == 0) t.firstField = (uint16_t)idx;
    t.fieldCount++;
    return idx;
}

void SchemaBuilder::addEvent(std::string_view typeName, EventSource source,
                             const rapidjson::Value& obj) {
    m_finalized = false;
    const uint16_t nameId = internName(typeName);
    const size_t slot = (size_t)source * 4096 + nameId;
    if (slot >= m_typeIndex.size()) m_typeIndex.resize(slot + 1, -1);
    if (m_typeIndex[slot] < 0) {
        TypeDesc td;
        td.nameId = nameId;
        td.source = (uint8_t)source;
        m_schema.types.push_back(td);
        m_typeIndex[slot] = (int32_t)(m_schema.types.size() - 1);
    }
    const uint16_t typeIdx = (uint16_t)m_typeIndex[slot];
    m_schema.types[typeIdx].eventCount++;

    // 该事件写到的字段：其余字段算"缺失"（presence 位图在列侧维护）
    for (auto it = obj.MemberBegin(); it != obj.MemberEnd(); ++it) {
        // `eventType` 是**结构字段**（它决定类型），不作为普通列重复存一遍
        if (std::strcmp(it->name.GetString(), "eventType") == 0) continue;
        const uint16_t fname = internName(std::string_view(it->name.GetString(), it->name.GetStringLength()));
        const uint32_t fi = ensureField(typeIdx, fname);
        if (!m_acc[fi]) m_acc[fi] = std::make_unique<Acc>();
        Acc& a = *m_acc[fi];
        a.total++;
        const auto& v = it->value;
        if (v.IsNull()) { a.nulls++; a.present++; continue; }
        a.present++;
        if (v.IsBool()) { a.sawBool = true; continue; }
        if (v.IsInt64()) { a.sawInt = true; continue; }
        if (v.IsNumber()) {
            a.sawFloat = true;
            const double d = v.GetDouble();
            for (int k = 0; k <= 6; ++k) {
                const double s = std::pow(10.0, k);
                if (std::fabs(d * s - std::round(d * s)) > 1e-9) a.gridOk[k] = false;
            }
            continue;
        }
        if (v.IsString()) {
            const std::string_view s(v.GetString(), v.GetStringLength());
            a.sawString = true;
            a.maxLen = std::max(a.maxLen, s.size());
            a.allTagLike = a.allTagLike && looksLikeTag(s);
            if (isHexColor(s)) a.sawColor = true;
            if (!a.distinctOverflow) {
                if (a.distinct.size() < kMaxEnumDistinct) a.distinct.emplace(s);
                else if (!a.distinct.count(std::string(s))) a.distinctOverflow = true;
            }
            continue;
        }
        if (v.IsArray()) {
            if (v.Size() == 2) {
                a.sawPair = true;
                if (v[0].IsNull() || v[1].IsNull()) a.pairHasNull = true;
            }
            else a.sawArrayOther = true;
            continue;
        }
        if (v.IsObject()) { a.sawObject = true; continue; }
        a.sawArrayOther = true;
    }
}

const Schema& SchemaBuilder::finalize() {
    if (m_finalized) return m_schema;
    // **字段必须按类型连续存放**：addEvent 交错追加时，同一类型的字段会被别的类型插开，
    // 而 TypeDesc 只有 (firstField, fieldCount)。这里按"首次出现顺序"重排一次，并把
    // 统计数组 m_acc 一起置换（否则 kind 会按错位的那一列算 —— 踩过：AnimateTrack.beatsBehind
    // 明明是数字却被判成 Pair）。
    {
        std::vector<std::vector<uint32_t>> perType(m_schema.types.size());
        for (const auto& kv : m_fieldIndex) {
            const uint32_t typeIdx = (uint32_t)(kv.first >> 32);
            if (typeIdx < perType.size()) perType[typeIdx].push_back(kv.second);
        }
        std::vector<uint32_t> perm;
        perm.reserve(m_schema.fields.size());
        for (size_t t = 0; t < perType.size(); ++t) {
            auto& list = perType[t];
            std::sort(list.begin(), list.end());
            m_schema.types[t].firstField = (uint16_t)perm.size();
            m_schema.types[t].fieldCount = (uint16_t)list.size();
            for (uint32_t idx : list) perm.push_back(idx);
        }
        if (perm.size() == m_schema.fields.size()) {
            std::vector<FieldDesc> nf;
            nf.reserve(perm.size());
            std::vector<std::unique_ptr<Acc>> na;
            na.reserve(perm.size());
            for (uint32_t oldIdx : perm) {
                nf.push_back(m_schema.fields[oldIdx]);
                na.push_back(oldIdx < m_acc.size() ? std::move(m_acc[oldIdx]) : nullptr);
            }
            m_schema.fields = std::move(nf);
            m_acc = std::move(na);
            m_fieldIndex.clear();   // 索引已失效；finalize 之后不再 addEvent
        }
    }
    for (size_t i = 0; i < m_schema.fields.size(); ++i) {
        FieldDesc& fd = m_schema.fields[i];
        if (!m_acc[i]) { fd.kind = Kind::RawJSON; continue; }
        const Acc& a = *m_acc[i];
        if (a.present == 0) { fd.kind = Kind::RawJSON; continue; }
        if (a.nulls == a.present) { fd.kind = Kind::RawJSON; fd.flags |= 0x4; continue; }   // 全 null
        if (a.nulls) fd.flags |= 0x1;
        if (a.present < a.total) fd.flags |= 0x2;
        // 单一形态优先；混合一律 RawJSON（编码器仍能无损，只是压得少）
        if (a.sawObject) { fd.kind = Kind::Nested; continue; }
        if (a.sawPair && !a.sawArrayOther && !a.sawInt && !a.sawFloat && !a.sawBool && !a.sawString) {
            fd.kind = Kind::Pair;
            if (a.pairHasNull) fd.flags |= 0x8;   // 分量出现 null：拆两列，各自带 null 掩码
            continue;
        }
        if (a.sawArrayOther) { fd.kind = Kind::RawJSON; continue; }
        if (a.sawString && !a.sawInt && !a.sawFloat && !a.sawBool) {
            if (a.sawColor && a.maxLen <= 9) { fd.kind = Kind::Color; continue; }
            if (!a.distinctOverflow && a.distinct.size() <= kMaxEnumDistinct && a.maxLen <= kMaxEnumLen) {
                fd.kind = Kind::Enum;
                continue;
            }
            fd.kind = a.allTagLike ? Kind::Tag : Kind::String;
            continue;
        }
        if (a.sawBool && !a.sawInt && !a.sawFloat) { fd.kind = Kind::Bool; continue; }
        if (a.sawInt && !a.sawFloat) { fd.kind = Kind::Int; continue; }
        if (a.sawFloat) {
            // 网格判定：取**最小**能整型化的 k（k=0 也要所有值都是整数才成立）
            for (int k = 0; k <= 6; ++k) {
                if (a.gridOk[k]) { fd.kind = Kind::GridFloat; fd.gridK = (uint8_t)k; break; }
            }
            if (fd.kind != Kind::GridFloat) fd.kind = Kind::Float;
            continue;
        }
        fd.kind = Kind::RawJSON;
    }
    m_finalized = true;
    return m_schema;
}

std::vector<uint8_t> SchemaBuilder::serialize() const {
    std::vector<uint8_t> b;
    putU32(b, (uint32_t)m_schema.names.size());
    for (const auto& s : m_schema.names) {
        putU16(b, (uint16_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    putU32(b, (uint32_t)m_schema.types.size());
    for (const auto& t : m_schema.types) {
        putU16(b, t.nameId);
        b.push_back(t.source);
        putU16(b, t.firstField);
        putU16(b, t.fieldCount);
        putU32(b, t.eventCount);
    }
    putU32(b, (uint32_t)m_schema.fields.size());
    for (const auto& f : m_schema.fields) {
        putU16(b, f.nameId);
        b.push_back((uint8_t)f.kind);
        b.push_back(f.flags);
        b.push_back(f.gridK);
        putU32(b, f.columnId);
    }
    return b;
}

bool SchemaBuilder::deserialize(const uint8_t* p, size_t n, Schema& out, std::string& err) {
    err.clear();
    const uint8_t* e = p + n;
    out = Schema{};
    uint32_t nameCount = 0;
    if (!getU32(p, e, nameCount) || nameCount == 0 || nameCount > 1u << 20) { err = "schema: 名字表数量非法"; return false; }
    out.names.reserve(nameCount);
    for (uint32_t i = 0; i < nameCount; ++i) {
        uint16_t len = 0;
        if (!getU16(p, e, len) || (size_t)(e - p) < len) { err = "schema: 名字表截断"; return false; }
        out.names.emplace_back((const char*)p, len);
        p += len;
    }
    uint32_t typeCount = 0;
    if (!getU32(p, e, typeCount) || typeCount > 1u << 16) { err = "schema: 类型表数量非法"; return false; }
    out.types.resize(typeCount);
    for (auto& t : out.types) {
        if (!getU16(p, e, t.nameId)) { err = "schema: 类型表截断"; return false; }
        if (e - p < 1) { err = "schema: 类型表截断"; return false; }
        t.source = *p++;
        if (!getU16(p, e, t.firstField) || !getU16(p, e, t.fieldCount)) { err = "schema: 类型表截断"; return false; }
        if (!getU32(p, e, t.eventCount)) { err = "schema: 类型表截断"; return false; }
        if (t.nameId >= out.names.size()) { err = "schema: 类型名下标越界"; return false; }
    }
    uint32_t fieldCount = 0;
    if (!getU32(p, e, fieldCount) || fieldCount > 1u << 20) { err = "schema: 字段表数量非法"; return false; }
    out.fields.resize(fieldCount);
    for (auto& f : out.fields) {
        if (!getU16(p, e, f.nameId)) { err = "schema: 字段表截断"; return false; }
        if (e - p < 3) { err = "schema: 字段表截断"; return false; }
        f.kind = (Kind)*p++;
        f.flags = *p++;
        f.gridK = *p++;
        if (!getU32(p, e, f.columnId)) { err = "schema: 字段表截断"; return false; }
        if (f.nameId >= out.names.size()) { err = "schema: 字段名下标越界"; return false; }
        if ((uint8_t)f.kind > (uint8_t)Kind::RawJSON) { err = "schema: 未知 kind（必须 bump version）"; return false; }
    }
    return true;
}

bool buildSchemaFromLevel(::adofai::LevelData& lv, SchemaBuilder& sb, std::string& err) {
    err.clear();
    for (size_t ai = 0; ai < lv.rawActionCount(); ++ai) {
        const std::string_view js = lv.rawAction(ai);
        rapidjson::Document d;
        d.Parse(js.data(), js.size());
        if (d.HasParseError() || !d.IsObject()) { err = "action 切片解析失败"; return false; }
        auto it = d.FindMember("eventType");
        if (it == d.MemberEnd() || !it->value.IsString()) { err = "action 缺 eventType"; return false; }
        sb.addEvent(std::string_view(it->value.GetString(), it->value.GetStringLength()),
                    EventSource::Actions, d);
    }
    if (!lv.rawDecorationsJson.empty()) {
        rapidjson::Document d;
        d.Parse(lv.rawDecorationsJson.c_str(), lv.rawDecorationsJson.size());
        if (d.HasParseError() || !d.IsArray()) { err = "decorations 切片解析失败"; return false; }
        for (const auto& v : d.GetArray()) {
            if (!v.IsObject()) continue;
            auto it = v.FindMember("eventType");
            const char* name = (it != v.MemberEnd() && it->value.IsString()) ? it->value.GetString()
                                                                           : "Decoration";
            sb.addEvent(name, EventSource::Decorations, v);
        }
    }
    return true;
}

std::string schemaSummary(const Schema& s) {
    size_t rawJsonFields = 0, fields = s.fields.size();
    std::vector<size_t> kindCount((size_t)Kind::RawJSON + 1, 0);
    for (const auto& f : s.fields) {
        ++kindCount[(size_t)f.kind];
        if (f.kind == Kind::RawJSON) ++rawJsonFields;
    }
    static const char* kNames[] = {"Bool", "Int", "Float", "GridFloat", "Enum", "Color",
                                   "String", "Tag", "Pair", "Nested", "RawJSON"};
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof buf, "schema: %zu 类型 / %zu 字段；rawJSON 字段 %zu/%zu（%.0f%%）\n",
                  s.types.size(), fields, rawJsonFields, fields,
                  fields ? 100.0 * rawJsonFields / fields : 0.0);
    out += buf;
    for (size_t i = 0; i < kindCount.size(); ++i) {
        if (!kindCount[i]) continue;
        std::snprintf(buf, sizeof buf, "  %-9s %zu\n", kNames[i], kindCount[i]);
        out += buf;
    }
    return out;
}

}  // namespace adocao
}  // namespace adofai
