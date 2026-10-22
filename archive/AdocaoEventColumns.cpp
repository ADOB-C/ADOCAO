#include "archive/AdocaoEventColumns.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "archive/AdocaoColumns.hpp"

namespace adofai {
namespace adocao {
namespace {

constexpr uint8_t kPairNull0 = 0x01;
constexpr uint8_t kPairNull1 = 0x02;
constexpr uint8_t kColorAlpha = 0x04;   // 8 位 hex（含 alpha）
constexpr uint8_t kColorHash  = 0x08;   // 原串带 '#' 前缀
constexpr uint8_t kGridAsDouble = 0x10; // GridFloat 实际上按 double 存（网格校验失败时兜底）

inline void bitSet(std::vector<uint8_t>& bm, size_t i) {
    if (bm.size() <= i / 8) bm.resize(i / 8 + 1, 0);
    bm[i / 8] = (uint8_t)(bm[i / 8] | (1u << (i % 8)));
}
inline bool bitGet(const std::vector<uint8_t>& bm, size_t i) {
    return i / 8 < bm.size() && (bm[i / 8] >> (i % 8)) & 1u;
}
inline bool bitEmpty(const std::vector<uint8_t>& bm) { return bm.empty(); }

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
bool getU32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
    if (e - p < 4) return false;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return true;
}
void putVarU32(std::vector<uint8_t>& b, uint32_t v) {
    while (v >= 0x80) { b.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    b.push_back((uint8_t)v);
}
bool getVarU32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
    v = 0;
    int shift = 0;
    while (p < e) {
        const uint8_t c = *p++;
        v |= (uint32_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) return true;
        shift += 7;
        if (shift > 28) return false;
    }
    return false;
}

// 十进制网格的 10^k
inline double pow10i(int k) {
    double s = 1.0;
    for (int i = 0; i < k; ++i) s *= 10.0;
    return s;
}

// 把 hex 颜色解析成 u32（0xRRGGBB 或 0xRRGGBBAA）
bool parseColor(const rapidjson::Value& v, uint32_t& out, uint8_t& flags) {
    const char* s = v.GetString();
    const size_t n = v.GetStringLength();
    flags = 0;
    size_t i = 0;
    if (n > 0 && s[0] == '#') { flags |= kColorHash; i = 1; }
    const size_t len = n - i;
    if (len != 6 && len != 8) return false;
    uint32_t c = 0;
    for (size_t k = 0; k < len; ++k) {
        const char ch = s[i + k];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
        else return false;
        c = (c << 4) | d;
    }
    if (len == 8) flags |= kColorAlpha;
    out = c;
    return true;
}

std::string colorToString(uint32_t c, uint8_t flags) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    if (flags & kColorHash) s.push_back('#');
    const int digits = (flags & kColorAlpha) ? 8 : 6;
    for (int i = digits - 1; i >= 0; --i) s.push_back(kHex[(c >> (4 * i)) & 0xF]);
    return s;
}

}  // namespace

bool buildEventColumns(const Schema& schema, const std::vector<std::string>& eventsJson,
                       const std::vector<uint8_t>& source, EventColumns& out, std::string& err) {
    err.clear();
    out = EventColumns{};
    if (eventsJson.size() != source.size()) { err = "事件与来源数量不一致"; return false; }
    const size_t n = eventsJson.size();

    // (source, typeNameId) → typeIdx
    std::unordered_map<uint32_t, uint16_t> typeByKey;
    for (uint16_t t = 0; t < schema.types.size(); ++t)
        typeByKey[((uint32_t)schema.types[t].source << 16) | schema.types[t].nameId] = t;

    out.typeIds.assign(n, 0);
    std::vector<uint32_t> seen(schema.types.size(), 0);      // 类型内第几个事件
    std::vector<uint32_t> eventIdxInType(n, 0);

    // 每个字段的临时收集
    struct Collect {
        std::vector<uint32_t> u32v;
        std::vector<int64_t> i64v;
        std::vector<double> dblv;
        std::vector<std::string> strv;
        std::vector<std::string> rawv;
        std::vector<double> pair0, pair1;
        std::vector<uint32_t> dictIds;
        std::unordered_map<std::string, uint32_t> dictIndex;
        std::vector<std::string> dictStrings;
        std::vector<uint8_t> nulls0, nulls1;
    };
    std::vector<Collect> col(schema.fields.size());

    std::vector<rapidjson::Document> docs(n);
    for (size_t e = 0; e < n; ++e) {
        docs[e].Parse(eventsJson[e].c_str(), eventsJson[e].size());
        if (docs[e].HasParseError() || !docs[e].IsObject()) { err = "事件 JSON 解析失败"; return false; }
        const rapidjson::Value& d = docs[e];
        auto it = d.FindMember("eventType");
        if (it == d.MemberEnd() || !it->value.IsString()) { err = "事件缺 eventType"; return false; }
        auto nameId = uint16_t(0);
        bool found = false;
        for (uint16_t i = 0; i < schema.names.size(); ++i)
            if (schema.names[i] == std::string(it->value.GetString(), it->value.GetStringLength())) {
                nameId = i;
                found = true;
                break;
            }
        if (!found) { err = "事件类型不在 schema 里"; return false; }
        auto tit = typeByKey.find(((uint32_t)source[e] << 16) | nameId);
        if (tit == typeByKey.end()) { err = "事件类型/来源不在 schema 里"; return false; }
        const uint16_t typeIdx = tit->second;
        out.typeIds[e] = typeIdx;
        eventIdxInType[e] = seen[typeIdx]++;
    }

    out.fields.resize(schema.fields.size());
    for (size_t t = 0; t < schema.types.size(); ++t) {
        const auto& td = schema.types[t];
        for (uint16_t fi = 0; fi < td.fieldCount; ++fi) {
            const uint32_t f = td.firstField + fi;
            out.fields[f].present.assign((td.eventCount + 7) / 8, 0);
        }
    }

    for (size_t e = 0; e < n; ++e) {
        const rapidjson::Value& d = docs[e];
        const uint16_t typeIdx = out.typeIds[e];
        const auto& td = schema.types[typeIdx];
        const uint32_t within = eventIdxInType[e];
        for (uint16_t fi = 0; fi < td.fieldCount; ++fi) {
            const uint32_t f = td.firstField + fi;
            const auto& fd = schema.fields[f];
            const char* fname = schema.name(fd.nameId).c_str();
            auto it = d.FindMember(fname);
            if (it == d.MemberEnd()) continue;             // 缺失
            bitSet(out.fields[f].present, within);
            Collect& c = col[f];
            const rapidjson::Value& v = it->value;
            if (v.IsNull()) { bitSet(out.fields[f].nulls, within); continue; }
            switch (fd.kind) {
                case Kind::Bool:
                    c.u32v.push_back(v.IsBool() && v.GetBool() ? 1u : 0u);
                    break;
                case Kind::Int:
                    c.i64v.push_back(v.IsInt64() ? v.GetInt64() : (int64_t)v.GetDouble());
                    break;
                case Kind::Float:
                    c.dblv.push_back(v.GetDouble());
                    break;
                case Kind::GridFloat: {
                    const double d2 = v.GetDouble();
                    const double sc = d2 * pow10i(fd.gridK);
                    const int64_t iv = (int64_t)std::llround(sc);
                    if (std::fabs(sc - (double)iv) <= 1e-9 &&
                        (double)iv / pow10i(fd.gridK) == d2) {
                        c.i64v.push_back(iv);
                    } else {
                        out.fields[f].subFlags |= kGridAsDouble;
                        c.dblv.push_back(d2);
                    }
                    break;
                }
                case Kind::Enum:
                case Kind::Tag:
                case Kind::String: {
                    const std::string s(v.GetString(), v.GetStringLength());
                    auto di = c.dictIndex.find(s);
                    if (di == c.dictIndex.end()) {
                        const uint32_t id = (uint32_t)c.dictStrings.size();
                        c.dictStrings.push_back(s);
                        c.dictIndex.emplace(s, id);
                        c.dictIds.push_back(id);
                    } else {
                        c.dictIds.push_back(di->second);
                    }
                    break;
                }
                case Kind::Color: {
                    // 先按字符串收集，收尾时再决定"全都能解析 → u32 列 / 否则字符串字典"
                    const std::string col = v.IsString() ? std::string(v.GetString(), v.GetStringLength()) : "";
                    c.strv.push_back(col);
                    break;
                }
                case Kind::Pair: {
                    const rapidjson::Value& a = v;
                    if (!a.IsArray() || a.Size() != 2) { err = "Pair 字段不是二元数组"; return false; }
                    if (a[0].IsNull()) { bitSet(out.fields[f].nulls0, within); c.pair0.push_back(0.0); }
                    else c.pair0.push_back(a[0].GetDouble());
                    if (a[1].IsNull()) { bitSet(out.fields[f].nulls1, within); c.pair1.push_back(0.0); }
                    else c.pair1.push_back(a[1].GetDouble());
                    break;
                }
                case Kind::Nested:
                case Kind::RawJSON: {
                    rapidjson::StringBuffer buf;
                    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
                    v.Accept(w);
                    c.rawv.emplace_back(buf.GetString(), buf.GetSize());
                    break;
                }
            }
        }
    }

    // 逐字段落盘
    for (size_t f = 0; f < schema.fields.size(); ++f) {
        const auto& fd = schema.fields[f];
        ColumnBlob& blob = out.fields[f];
        Collect& c = col[f];
        switch (fd.kind) {
            case Kind::Bool: {
                if (!encodeU32Column(c.u32v, blob.payload)) { err = "u32 列编码失败"; return false; }
                blob.valueCount = (uint32_t)c.u32v.size();
                break;
            }
            case Kind::Color: {
                // 全部能解析成 6/8 位 hex → 数值列；否则退回字符串字典（无损）
                std::vector<uint32_t> vals;
                uint8_t common = 0;
                bool ok = true, first = true, mixed = false;
                for (const auto& str : c.strv) {
                    uint32_t cv = 0;
                    uint8_t cf = 0;
                    rapidjson::Value tmp;
                    tmp.SetString(str.c_str(), (rapidjson::SizeType)str.size());
                    if (!parseColor(tmp, cv, cf)) { ok = false; break; }
                    if (first) { common = (uint8_t)(cf & 0x0C); first = false; }
                    else if ((cf & 0x0C) != common) mixed = true;
                    vals.push_back(cv);
                }
                if (ok && !mixed) {
                    blob.subFlags = (uint8_t)((blob.subFlags & ~0x0C) | common);
                    if (!encodeU32Column(vals, blob.payload)) { err = "颜色列编码失败"; return false; }
                    blob.valueCount = (uint32_t)vals.size();
                } else {
                    std::unordered_map<std::string, uint32_t> idx;
                    std::vector<uint32_t> ids;
                    for (const auto& str : c.strv) {
                        auto it = idx.find(str);
                        if (it == idx.end()) {
                            const uint32_t id = (uint32_t)blob.dictStrings.size();
                            blob.dictStrings.push_back(str);
                            idx.emplace(str, id);
                            ids.push_back(id);
                        } else ids.push_back(it->second);
                    }
                    if (!encodeU32Column(ids, blob.payload)) { err = "颜色字符串列编码失败"; return false; }
                    blob.valueCount = (uint32_t)ids.size();
                    blob.subFlags |= 0x40;   // 按字符串字典存
                }
                break;
            }
            case Kind::Int:
            case Kind::GridFloat: {
                if (fd.kind == Kind::GridFloat && (blob.subFlags & kGridAsDouble)) {
                    if (!encodeDoubleColumn(c.dblv, blob.payload)) { err = "double 列编码失败"; return false; }
                    blob.valueCount = (uint32_t)c.dblv.size();
                } else {
                    if (!encodeIntColumn(c.i64v, blob.payload)) { err = "int64 列编码失败"; return false; }
                    blob.valueCount = (uint32_t)c.i64v.size();
                }
                break;
            }
            case Kind::Float: {
                if (!encodeDoubleColumn(c.dblv, blob.payload)) { err = "double 列编码失败"; return false; }
                blob.valueCount = (uint32_t)c.dblv.size();
                break;
            }
            case Kind::Enum:
            case Kind::Tag:
            case Kind::String: {
                if (!encodeU32Column(c.dictIds, blob.payload)) { err = "字符串字典列编码失败"; return false; }
                blob.valueCount = (uint32_t)c.dictIds.size();
                blob.dictStrings = std::move(c.dictStrings);
                break;
            }
            case Kind::Pair: {
                std::vector<uint8_t> a, b;
                if (!encodeDoubleColumn(c.pair0, a) || !encodeDoubleColumn(c.pair1, b)) {
                    err = "pair 分量编码失败"; return false;
                }
                putU32(blob.payload, (uint32_t)a.size());
                blob.payload.insert(blob.payload.end(), a.begin(), a.end());
                blob.payload.insert(blob.payload.end(), b.begin(), b.end());
                blob.valueCount = (uint32_t)c.pair0.size();
                // 分量 null 位图在**收集阶段**就写进了 out.fields[f].nulls0/1（见上面的 Pair 分支），
                // 这里不能再赋空值覆盖（踩过：往返后 [null,null] 变成 [0,0]）。
                if (!blob.nulls0.empty()) blob.subFlags |= kPairNull0;
                if (!blob.nulls1.empty()) blob.subFlags |= kPairNull1;
                break;
            }
            case Kind::Nested:
            case Kind::RawJSON: {
                std::vector<uint8_t> raw;
                for (const auto& s : c.rawv) {
                    putVarU32(raw, (uint32_t)s.size());
                    raw.insert(raw.end(), s.begin(), s.end());
                }
                blob.payload = std::move(raw);
                blob.valueCount = (uint32_t)c.rawv.size();
                break;
            }
        }
    }
    return true;
}

bool decodeEventColumns(const Schema& schema, const EventColumns& cols, size_t eventCount,
                        std::vector<std::string>& eventsJson, std::string& err) {
    err.clear();
    eventsJson.clear();
    eventsJson.resize(eventCount);
    if (cols.typeIds.size() != eventCount) { err = "类型列与事件数不一致"; return false; }
    if (cols.fields.size() != schema.fields.size()) { err = "字段列数与 schema 不一致"; return false; }

    // 每字段解码成值数组，然后按位图顺序消费
    struct Decoded {
        std::vector<uint32_t> u32v;
        std::vector<int64_t> i64v;
        std::vector<double> dblv;
        std::vector<std::string> strv;   // 已展开成字符串（Enum/Tag/String/RawJSON）
        std::vector<double> pair0, pair1;
        size_t cursor = 0;
        bool ok = true;
    };
    std::vector<Decoded> dec(schema.fields.size());

    for (size_t f = 0; f < schema.fields.size(); ++f) {
        const auto& fd = schema.fields[f];
        const ColumnBlob& blob = cols.fields[f];
        Decoded& d = dec[f];
        const uint8_t* p = blob.payload.data();
        const uint8_t* e = p + blob.payload.size();
        switch (fd.kind) {
            case Kind::Bool:
            case Kind::Color: {
                if (blob.subFlags & 0x40) {   // 颜色按字符串字典
                    std::vector<uint32_t> ids;
                    if (!decodeU32Column(p, blob.payload.size(), ids)) { err = "颜色字符串列解码失败"; return false; }
                    for (uint32_t id : ids) {
                        if (id >= blob.dictStrings.size()) { err = "颜色字典下标越界"; return false; }
                        d.strv.push_back(blob.dictStrings[id]);
                    }
                } else if (!decodeU32Column(p, blob.payload.size(), d.u32v)) {
                    err = "u32 列解码失败"; return false;
                }
                break;
            }
            case Kind::Int:
            case Kind::GridFloat: {
                if (fd.kind == Kind::GridFloat && (blob.subFlags & kGridAsDouble)) {
                    if (!decodeDoubleColumn(p, blob.payload.size(), d.dblv)) { err = "double 列解码失败"; return false; }
                } else if (!decodeIntColumn(p, blob.payload.size(), d.i64v)) {
                    err = "int64 列解码失败"; return false;
                }
                break;
            }
            case Kind::Float:
                if (!decodeDoubleColumn(p, blob.payload.size(), d.dblv)) { err = "double 列解码失败"; return false; }
                break;
            case Kind::Enum:
            case Kind::Tag:
            case Kind::String: {
                std::vector<uint32_t> ids;
                if (!decodeU32Column(p, blob.payload.size(), ids)) { err = "字符串字典列解码失败"; return false; }
                for (uint32_t id : ids) {
                    if (id >= blob.dictStrings.size()) { err = "字符串字典下标越界"; return false; }
                    d.strv.push_back(blob.dictStrings[id]);
                }
                break;
            }
            case Kind::Pair: {
                if (blob.payload.size() < 4) { err = "pair 列截断"; return false; }
                uint32_t la = 0;
                if (!getU32(p, e, la) || (size_t)(e - p) < la) { err = "pair 列截断"; return false; }
                if (!decodeDoubleColumn(p, la, d.pair0)) { err = "pair 分量0解码失败"; return false; }
                p += la;
                if (!decodeDoubleColumn(p, (size_t)(e - p), d.pair1)) { err = "pair 分量1解码失败"; return false; }
                break;
            }
            case Kind::Nested:
            case Kind::RawJSON: {
                while (p < e) {
                    uint32_t len = 0;
                    if (!getVarU32(p, e, len) || (size_t)(e - p) < len) { err = "rawJSON 列截断"; return false; }
                    d.strv.emplace_back((const char*)p, len);
                    p += len;
                }
                break;
            }
        }
    }

    // 还原每个事件
    std::vector<uint32_t> seen(schema.types.size(), 0);
    for (size_t e = 0; e < eventCount; ++e) {
        const uint16_t t = cols.typeIds[e];
        if (t >= schema.types.size()) { err = "类型下标越界"; return false; }
        const auto& td = schema.types[t];
        const uint32_t within = seen[t]++;
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("eventType");
        w.String(schema.name(td.nameId).c_str(), (rapidjson::SizeType)schema.name(td.nameId).size());
        for (uint16_t fi = 0; fi < td.fieldCount; ++fi) {
            const uint32_t f = td.firstField + fi;
            const ColumnBlob& blob = cols.fields[f];
            if (!bitGet(blob.present, within)) continue;      // 缺失
            const char* fname = schema.name(schema.fields[f].nameId).c_str();
            w.Key(fname);
            if (bitGet(blob.nulls, within)) { w.Null(); continue; }
            Decoded& d = dec[f];
            const auto& fd = schema.fields[f];
            const auto take = [&](size_t sz) { if (d.cursor >= sz) return false; ++d.cursor; return true; };
            switch (fd.kind) {
                case Kind::Bool:
                    if (!take(d.u32v.size())) { err = "bool 列取值越界"; return false; }
                    w.Bool(d.u32v[d.cursor - 1] != 0);
                    break;
                case Kind::Int:
                    if (!take(d.i64v.size())) { err = "int 列取值越界"; return false; }
                    w.Int64(d.i64v[d.cursor - 1]);
                    break;
                case Kind::GridFloat: {
                    const int64_t iv = (blob.subFlags & kGridAsDouble) ? 0 : d.i64v[d.cursor++];
                    if (blob.subFlags & kGridAsDouble) w.Double(d.dblv[d.cursor++]);
                    else w.Double((double)iv / pow10i(fd.gridK));
                    break;
                }
                case Kind::Float:
                    w.Double(d.dblv[d.cursor++]);
                    break;
                case Kind::Enum:
                case Kind::Tag:
                case Kind::String:
                    w.String(d.strv[d.cursor++].c_str());
                    break;
                case Kind::Color: {
                    if (blob.subFlags & 0x40) w.String(d.strv[d.cursor++].c_str());
                    else {
                        const std::string s = colorToString(d.u32v[d.cursor++], blob.subFlags & 0x0C);
                        w.String(s.c_str(), (rapidjson::SizeType)s.size());
                    }
                    break;
                }
                case Kind::Pair: {
                    if (!take(d.pair0.size())) { err = "pair 列取值越界"; return false; }
                    const size_t k = d.cursor - 1;
                    w.StartArray();
                    if (bitGet(blob.nulls0, within)) w.Null();
                    else w.Double(d.pair0[k]);
                    if (bitGet(blob.nulls1, within)) w.Null();
                    else w.Double(d.pair1[k]);
                    w.EndArray();
                    break;
                }
                case Kind::Nested:
                case Kind::RawJSON: {
                    const std::string& s = d.strv[d.cursor++];
                    w.RawValue(s.c_str(), (rapidjson::SizeType)s.size(), rapidjson::kObjectType);
                    break;
                }
            }
        }
        w.EndObject();
        eventsJson[e].assign(buf.GetString(), buf.GetSize());
    }
    return true;
}

}  // namespace adocao
}  // namespace adofai
