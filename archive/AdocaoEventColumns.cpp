#include "archive/AdocaoEventColumns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "core/util/Env.hpp"
#include "core/level/LevelData.hpp"

#include <rapidjson/document.h>
#include "archive/AdocaoColumns.hpp"

#include <zstd.h>

namespace adofai {
namespace adocao {
namespace {

constexpr uint8_t kPairNull0 = 0x01;
constexpr uint8_t kPairNull1 = 0x02;
constexpr uint8_t kColorAlpha = 0x04;   // 8 位 hex（含 alpha）
constexpr uint8_t kColorHash  = 0x08;   // 原串带 '#' 前缀
constexpr uint8_t kGridAsDouble = 0x10; // GridFloat 实际上按 double 存（网格校验失败时兜底）
constexpr uint8_t kPairRaw     = 0x20;   // 整对退回原始 JSON（分量不是标量时）
constexpr uint8_t kPairStr0    = 0x40;   // Pair 分量0 是字符串（startTile 的 "ThisTile"）
constexpr uint8_t kPairStr1    = 0x80;   // Pair 分量1 是字符串 // Pair 的两个分量都是整数 → 存 int64（差分更省）

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
        std::vector<std::string> pair0s, pair1s;       // 字符串分量（startTile 的 "ThisTile"）
        std::vector<std::string> pairRaw;              // 整对原样（分量不是标量时兜底）
        bool pair0IsStr = false, pair1IsStr = false;
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
                    if (!a.IsArray() || a.Size() != 2) {
                        char msg[256];
                        std::snprintf(msg, sizeof msg, "Pair 字段不是二元数组: %s.%s (type=%d, isArray=%d size=%d isNum=%d)",
                                      schema.name(schema.types[typeIdx].nameId).c_str(),
                                      schema.name(fd.nameId).c_str(), (int)typeIdx, (int)a.IsArray(),
                                      a.IsArray() ? (int)a.Size() : -1, (int)a.IsNumber());
                        err = msg;
                        return false;
                    }
                    // 分量可以是数字**或字符串**（`startTile: [3333, "ThisTile"]`）——
                    // 逐分量决定列类型，别硬当数字读（踩过：字符串被 GetDouble 成 7.3e18）。
                    // 分量若是对象/数组（如 AddParticle.velocity），整对退回原始 JSON。
                    if ((!a[0].IsNumber() && !a[0].IsString() && !a[0].IsNull()) ||
                        (!a[1].IsNumber() && !a[1].IsString() && !a[1].IsNull())) {
                        rapidjson::StringBuffer buf;
                        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
                        v.Accept(w);
                        c.pairRaw.emplace_back(buf.GetString(), buf.GetSize());
                        out.fields[f].subFlags |= kPairRaw;
                        break;
                    }
                    for (int comp = 0; comp < 2; ++comp) {
                        const bool isStr = a[comp].IsString();
                        if (a[comp].IsNull()) {
                            if (comp == 0) bitSet(out.fields[f].nulls0, within);
                            else bitSet(out.fields[f].nulls1, within);
                        } else if (isStr) {
                            (comp == 0 ? c.pair0IsStr : c.pair1IsStr) = true;
                        } else if (!a[comp].IsNumber()) {
                            char m[192];
                            std::snprintf(m, sizeof m, "Pair 分量既不是数字也不是字符串（%s.%s comp%d）",
                                          schema.name(schema.types[typeIdx].nameId).c_str(),
                                          schema.name(fd.nameId).c_str(), comp);
                            err = m;
                            return false;
                        }
                        if (comp == 0) {
                            if (isStr) c.pair0s.emplace_back(a[0].GetString(), a[0].GetStringLength());
                            else c.pair0.push_back(a[0].IsNumber() ? a[0].GetDouble() : 0.0);
                        } else {
                            if (isStr) c.pair1s.emplace_back(a[1].GetString(), a[1].GetStringLength());
                            else c.pair1.push_back(a[1].IsNumber() ? a[1].GetDouble() : 0.0);
                        }
                    }
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
                if (blob.subFlags & kPairRaw) {
                    std::vector<uint8_t> raw;
                    for (const auto& str : c.pairRaw) {
                        putVarU32(raw, (uint32_t)str.size());
                        raw.insert(raw.end(), str.begin(), str.end());
                    }
                    blob.payload = std::move(raw);
                    blob.valueCount = (uint32_t)c.pairRaw.size();
                    break;
                }
                std::vector<uint8_t> a, b;
                auto encodeComp = [&](bool second, std::vector<uint8_t>& dst) -> bool {
                    if (second ? c.pair1IsStr : c.pair0IsStr) {
                        const auto& vals = second ? c.pair1s : c.pair0s;
                        std::unordered_map<std::string, uint32_t> idx;
                        std::vector<uint32_t> ids;
                        std::vector<std::string> dict;
                        for (const auto& str : vals) {
                            auto it = idx.find(str);
                            if (it == idx.end()) {
                                const uint32_t id = (uint32_t)dict.size();
                                dict.push_back(str);
                                idx.emplace(str, id);
                                ids.push_back(id);
                            } else ids.push_back(it->second);
                        }
                        std::vector<uint8_t> enc;
                        if (!encodeU32Column(ids, enc)) return false;
                        putU32(dst, (uint32_t)dict.size());
                        for (const auto& ds : dict) {
                            putVarU32(dst, (uint32_t)ds.size());
                            dst.insert(dst.end(), ds.begin(), ds.end());
                        }
                        dst.insert(dst.end(), enc.begin(), enc.end());
                        return true;
                    }
                    return encodeDoubleColumn(second ? c.pair1 : c.pair0, dst);
                };
                if (!encodeComp(0, a) || !encodeComp(1, b)) { err = "pair 分量编码失败"; return false; }
                blob.subFlags |= (c.pair0IsStr ? kPairStr0 : 0) | (c.pair1IsStr ? kPairStr1 : 0);
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
                        std::vector<std::string>& eventsJson, std::string& err,
                        FieldBatch* outBatch, bool wantJson) {
    err.clear();
    eventsJson.clear();
    if (wantJson || !outBatch) eventsJson.resize(eventCount);
    // 字段角色只在 schema 上算一次（名字 → FieldRole）
    std::vector<LevelData::FieldRole> roles;
    if (outBatch) {
        roles.resize(schema.fields.size());
        for (size_t f = 0; f < schema.fields.size(); ++f) {
            const auto& nm = schema.name(schema.fields[f].nameId);
            roles[f] = LevelData::fieldRoleForName(nm.data(), nm.size());
        }
        outBatch->values.clear();
        outBatch->offsets.assign(1, 0);
        outBatch->offsets.reserve(eventCount + 1);
    }
    if (cols.typeIds.size() != eventCount) { err = "类型列与事件数不一致"; return false; }
    if (cols.fields.size() != schema.fields.size()) { err = "字段列数与 schema 不一致"; return false; }

    // 每字段解码成值数组，然后按位图顺序消费（JSON 与字段两条出口共用这段）
    struct Decoded {
        std::vector<uint32_t> u32v;
        std::vector<int64_t> i64v;
        std::vector<double> dblv;
        std::vector<std::string> strv;   // 已展开成字符串（Enum/Tag/String/RawJSON）
        std::vector<double> pair0, pair1;              // 数字分量
        std::vector<std::string> pair0s, pair1s;       // 字符串分量（startTile 的 "ThisTile"）
        std::vector<std::string> pairRaw;              // 整对原样（分量不是标量时兜底）
        bool pair0IsStr = false, pair1IsStr = false;
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
                if (blob.subFlags & kPairRaw) {
                    const uint8_t* q = p;
                    while (q < e) {
                        uint32_t len = 0;
                        if (!getVarU32(q, e, len) || (size_t)(e - q) < len) { err = "pair 原始列截断"; return false; }
                        d.strv.emplace_back((const char*)q, len);
                        q += len;
                    }
                    break;
                }
                if (blob.payload.size() < 4) { err = "pair 列截断"; return false; }
                uint32_t la = 0;
                if (!getU32(p, e, la) || (size_t)(e - p) < la) { err = "pair 列截断"; return false; }
                auto decodeComp = [&](bool second, const uint8_t* data, size_t n) -> bool {
                    if ((second ? (blob.subFlags & kPairStr1) : (blob.subFlags & kPairStr0)) != 0) {
                        const uint8_t* q = data;
                        const uint8_t* qe = data + n;
                        uint32_t dn = 0;
                        if (!getU32(q, qe, dn)) return false;
                        std::vector<std::string> dict;
                        dict.reserve(dn);
                        for (uint32_t i = 0; i < dn; ++i) {
                            uint32_t len = 0;
                            if (!getVarU32(q, qe, len) || (size_t)(qe - q) < len) return false;
                            dict.emplace_back((const char*)q, len);
                            q += len;
                        }
                        std::vector<uint32_t> ids;
                        if (!decodeU32Column(q, (size_t)(qe - q), ids)) return false;
                        for (uint32_t id : ids) {
                            if (id >= dict.size()) return false;
                            (second ? d.pair1s : d.pair0s).push_back(dict[id]);
                        }
                        return true;
                    }
                    return decodeDoubleColumn(data, n, second ? d.pair1 : d.pair0);
                };
                if (!decodeComp(0, p, la)) { err = "pair 分量0解码失败"; return false; }
                p += la;
                if (!decodeComp(1, p, (size_t)(e - p))) { err = "pair 分量1解码失败"; return false; }
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
                    const size_t k = d.cursor++;
                    if (blob.subFlags & kPairRaw) {
                        const std::string& raw = d.strv[k];
                        w.RawValue(raw.c_str(), (rapidjson::SizeType)raw.size(), rapidjson::kArrayType);
                        break;
                    }
                    w.StartArray();
                    if (bitGet(blob.nulls0, within)) w.Null();
                    else if (blob.subFlags & kPairStr0) w.String(d.pair0s[k].c_str());
                    else w.Double(d.pair0[k]);
                    if (bitGet(blob.nulls1, within)) w.Null();
                    else if (blob.subFlags & kPairStr1) w.String(d.pair1s[k].c_str());
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
        if (wantJson || !outBatch) eventsJson[e].assign(buf.GetString(), buf.GetSize());
    }

    // ── 第二遍：产出结构化字段（v7 → LevelData 直灌，不再经过 JSON 文本）──
    if (outBatch) {
        for (auto& d : dec) d.cursor = 0;
        std::vector<uint32_t> seen2(schema.types.size(), 0);
        for (size_t e = 0; e < eventCount; ++e) {
            const uint16_t t = cols.typeIds[e];
            const auto& td = schema.types[t];
            const uint32_t within = seen2[t]++;
            auto& values = outBatch->values;
            const size_t firstIdx = values.size();
            {
                LevelData::EventFieldValue et;
                et.role = LevelData::FieldRole::EventType;
                et.type = LevelData::EventFieldValue::Str;
                const auto& tn = schema.name(td.nameId);
                et.str = tn.data();
                et.len = (uint32_t)tn.size();
                values.push_back(et);
            }
            for (uint16_t fi = 0; fi < td.fieldCount; ++fi) {
                const uint32_t f = td.firstField + fi;
                const ColumnBlob& blob = cols.fields[f];
                if (!bitGet(blob.present, within)) continue;
                if (bitGet(blob.nulls, within)) continue;   // null = 没写（与 JSON 路径一致）
                Decoded& d = dec[f];
                LevelData::EventFieldValue v;
                v.role = roles[f];
                const auto& fd = schema.fields[f];
                switch (fd.kind) {
                    case Kind::Bool:
                        v.type = LevelData::EventFieldValue::Bool;
                        v.b = d.u32v[d.cursor++] != 0;
                        break;
                    case Kind::Int:
                        v.type = LevelData::EventFieldValue::Num;
                        v.num = (double)d.i64v[d.cursor++];
                        break;
                    case Kind::GridFloat: {
                        v.type = LevelData::EventFieldValue::Num;
                        if (blob.subFlags & kGridAsDouble) v.num = d.dblv[d.cursor++];
                        else v.num = (double)d.i64v[d.cursor++] / pow10i(fd.gridK);
                        break;
                    }
                    case Kind::Float:
                        v.type = LevelData::EventFieldValue::Num;
                        v.num = d.dblv[d.cursor++];
                        break;
                    case Kind::Enum:
                    case Kind::Tag:
                    case Kind::String: {
                        // **必须拷进 batch**：`dec` 是 decodeEventColumns 的局部变量，函数返回就析构，
                        // 直接把 `d.strv` 的指针交出去会悬垂（语料回归里表现为 style/relativeTo 变乱码）。
                        outBatch->strings.push_back(d.strv[d.cursor++]);
                        const std::string& sv = outBatch->strings.back();
                        v.type = LevelData::EventFieldValue::Str;
                        v.str = sv.data();
                        v.len = (uint32_t)sv.size();
                        break;
                    }
                    case Kind::Color: {
                        v.type = LevelData::EventFieldValue::Str;
                        if (blob.subFlags & 0x40) {
                            outBatch->strings.push_back(d.strv[d.cursor++]);
                            const std::string& sv = outBatch->strings.back();
                            v.str = sv.data();
                            v.len = (uint32_t)sv.size();
                        } else {
                            // 颜色：转成十六进制串（core 侧按字符串解析）。**存在 batch 里**，不能复用缓冲。
                            outBatch->strings.push_back(colorToString(d.u32v[d.cursor++], blob.subFlags & 0x0C));
                            const std::string& hex = outBatch->strings.back();
                            v.str = hex.data();
                            v.len = (uint32_t)hex.size();
                        }
                        break;
                    }
                    case Kind::Pair: {
                        const size_t k = d.cursor++;
                        v.type = LevelData::EventFieldValue::Pair;
                        if (blob.subFlags & kPairRaw) {   // 不支持结构化：跳过（与"没写"不同，但这类很少）
                            (void)firstIdx;
                            continue;
                        }
                        v.nullA = bitGet(blob.nulls0, within) ? 1 : 0;
                        v.nullC = bitGet(blob.nulls1, within) ? 1 : 0;
                        if (blob.subFlags & kPairStr0) {
                            v.a = 0;
                        } else if (k < d.pair0.size()) {
                            v.a = d.pair0[k];
                        }
                        if (blob.subFlags & kPairStr1) {
                            if (k < d.pair1s.size()) {
                                outBatch->strings.push_back(d.pair1s[k]);
                                const std::string& ps = outBatch->strings.back();
                                v.str = ps.data();
                                v.len = (uint32_t)ps.size();
                            }
                        } else if (k < d.pair1.size()) {
                            v.c = d.pair1[k];
                        }
                        if ((blob.subFlags & kPairStr0) && k < d.pair0s.size()) {
                            // 第一分量也是字符串（罕见）：用 num 无从表达 → 跳过该字段
                            continue;
                        }
                        break;
                    }
                    case Kind::Nested:
                    case Kind::RawJSON: {
                        outBatch->strings.push_back(d.strv[d.cursor++]);
                        const std::string& sv = outBatch->strings.back();
                        v.type = LevelData::EventFieldValue::Str;
                        v.str = sv.data();
                        v.len = (uint32_t)sv.size();
                        break;
                    }
                }
                values.push_back(v);
            }
            outBatch->offsets.push_back((uint32_t)values.size());
        }
    }
    return true;
}

std::vector<uint8_t> serializeEventColumns(const Schema& schema, const EventColumns& cols) {
    std::vector<uint8_t> b;
    // 字典策略：**每列自带**（实测在整段 zstd 面前，全局池/前置编码都没有净收益：
    // 2025 上 72.3 KB → 74.4 KB，反而变大 —— zstd 已经跨列吃到重复前缀，别再叠一层映射开销）。
    putU32(b, (uint32_t)cols.typeIds.size());
    for (uint16_t t : cols.typeIds) { b.push_back((uint8_t)(t & 0xFF)); b.push_back((uint8_t)(t >> 8)); }
    // **列重排**：按 (kind, 原下标) 排序后再拼，让压缩器窗口里同类数据相邻。
    // 实测 Hello 2025：73,296 → 72,920 B（-0.5%，聊胜于无但零风险；解码侧用置换表还原）。
    std::vector<uint16_t> order(cols.fields.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (uint16_t)i;
    if (env::flag("ADOCAO_V7_NO_REORDER")) {
        // 诊断钩子：关掉重排做 A/B（不改变解码结果，只影响压缩率）
    } else
    std::sort(order.begin(), order.end(), [&schema](uint16_t a, uint16_t b) {
        const uint8_t ka = (uint8_t)schema.fields[a].kind, kb = (uint8_t)schema.fields[b].kind;
        return ka != kb ? ka < kb : a < b;
    });
    putU32(b, (uint32_t)order.size());
    for (uint16_t idx : order) putU32(b, idx);

    putU32(b, (uint32_t)cols.fields.size());
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const size_t f = order[oi];
        const ColumnBlob& c = cols.fields[f];
        b.push_back((uint8_t)schema.fields[f].kind);
        b.push_back(c.subFlags);
        putU32(b, c.valueCount);
        auto putBlob = [&](const std::vector<uint8_t>& v) {
            putU32(b, (uint32_t)v.size());
            b.insert(b.end(), v.begin(), v.end());
        };
        putBlob(c.present); putBlob(c.nulls); putBlob(c.nulls0); putBlob(c.nulls1); putBlob(c.payload);
        if (c.subFlags & ColumnBlob::kDictGlobal) {
            const uint32_t dn = (uint32_t)c.dictGlobalIds.size();
            putU32(b, dn);
            for (uint32_t gid : c.dictGlobalIds) putVarU32(b, gid);
        } else {
            const uint32_t dn = (uint32_t)c.dictStrings.size();
            putU32(b, dn);
            for (const auto& ds : c.dictStrings) {
                putVarU32(b, (uint32_t)ds.size());
                b.insert(b.end(), ds.begin(), ds.end());
            }
        }
    }
    return b;
}

bool deserializeEventColumns(const Schema& schema, const uint8_t* p, size_t n, EventColumns& out,
                             std::string& err, const std::vector<std::string>* pool) {
    err.clear();
    out = EventColumns{};
    const uint8_t* e = p + n;
    uint32_t eventCount = 0;
    if (!getU32(p, e, eventCount) || (size_t)(e - p) < (size_t)eventCount * 2) { err = "事件列：类型列截断"; return false; }
    out.typeIds.resize(eventCount);
    for (uint32_t i = 0; i < eventCount; ++i) {
        out.typeIds[i] = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        if (out.typeIds[i] >= schema.types.size()) { err = "事件列：类型下标越界"; return false; }
    }
    uint32_t orderCount = 0;
    if (!getU32(p, e, orderCount) || orderCount != schema.fields.size()) { err = "事件列：列序表长度不一致"; return false; }
    std::vector<uint32_t> order(orderCount);
    for (uint32_t i = 0; i < orderCount; ++i) {
        if (!getU32(p, e, order[i]) || order[i] >= orderCount) { err = "事件列：列序表越界"; return false; }
    }
    uint32_t fieldCount = 0;
    if (!getU32(p, e, fieldCount) || fieldCount != schema.fields.size()) { err = "事件列：字段数与 schema 不一致"; return false; }
    out.fields.resize(fieldCount);
    for (uint32_t oi = 0; oi < fieldCount; ++oi) {
        const uint32_t f = order[oi];
        ColumnBlob& c = out.fields[f];
        if (e - p < 2) { err = "事件列：字段头截断"; return false; }
        const uint8_t kind = *p++;
        c.subFlags = *p++;
        if (kind != (uint8_t)schema.fields[f].kind) { err = "事件列：kind 与 schema 不一致"; return false; }
        if (!getU32(p, e, c.valueCount)) { err = "事件列：字段头截断"; return false; }
        auto getBlob = [&](std::vector<uint8_t>& v) {
            uint32_t len = 0;
            if (!getU32(p, e, len) || (size_t)(e - p) < len) return false;
            v.assign(p, p + len);
            p += len;
            return true;
        };
        if (!getBlob(c.present) || !getBlob(c.nulls) || !getBlob(c.nulls0) || !getBlob(c.nulls1) ||
            !getBlob(c.payload)) { err = "事件列：字段数据截断"; return false; }
        uint32_t dictCount = 0;
        if (!getU32(p, e, dictCount)) { err = "事件列：字典头截断"; return false; }
        if (c.subFlags & ColumnBlob::kDictGlobal) {
            c.dictGlobalIds.reserve(dictCount);
            for (uint32_t i = 0; i < dictCount; ++i) {
                uint32_t gid = 0;
                if (!getVarU32(p, e, gid)) { err = "事件列：全局字典下标截断"; return false; }
                c.dictGlobalIds.push_back(gid);
                if (pool) {
                    if (gid >= pool->size()) { err = "事件列：全局字典下标越界"; return false; }
                    c.dictStrings.push_back((*pool)[gid]);
                }
            }
        } else {
            c.dictStrings.reserve(dictCount);
            for (uint32_t i = 0; i < dictCount; ++i) {
                uint32_t len = 0;
                if (!getVarU32(p, e, len) || (size_t)(e - p) < len) { err = "事件列：字典截断"; return false; }
                c.dictStrings.emplace_back((const char*)p, len);
                p += len;
            }
        }
    }
    return true;
}

namespace {
// 块容器的段头：u32 version | u32 blockEvents | u32 blockCount | u32 reserved | u64 offsets[blockCount+1]
constexpr uint32_t kBlockedVersion = 1;
constexpr size_t kBlockedHeaderBytes = 16;

void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
}  // namespace

std::vector<uint8_t> serializeEventColumnsBlocked(const Schema& schema,
                                                  const std::vector<std::string>& eventsJson,
                                                  const std::vector<uint8_t>& source,
                                                  uint32_t blockEvents, size_t* outBlocks) {
    if (blockEvents == 0) blockEvents = 4096;
    const size_t n = eventsJson.size();
    const uint32_t blocks = (uint32_t)((n + blockEvents - 1) / blockEvents);
    std::vector<std::vector<uint8_t>> blobs(blocks);
    for (uint32_t b = 0; b < blocks; ++b) {
        const size_t first = (size_t)b * blockEvents;
        const size_t last = std::min(n, first + blockEvents);
        std::vector<std::string> evs(eventsJson.begin() + (long)first, eventsJson.begin() + (long)last);
        std::vector<uint8_t> src(source.begin() + (long)first, source.begin() + (long)last);
        EventColumns cols;
        std::string err;
        if (!buildEventColumns(schema, evs, src, cols, err)) { blobs[b].clear(); continue; }
        blobs[b] = serializeEventColumns(schema, cols);
    }
    std::vector<uint8_t> out;
    putU32(out, kBlockedVersion);
    putU32(out, blockEvents);
    putU32(out, blocks);
    putU32(out, 0);
    uint64_t off = kBlockedHeaderBytes + (uint64_t)(blocks + 1) * 8;
    for (uint32_t b = 0; b <= blocks; ++b) {
        putU64(out, off);
        if (b < blocks) off += blobs[b].size();
    }
    for (uint32_t b = 0; b < blocks; ++b) out.insert(out.end(), blobs[b].begin(), blobs[b].end());
    if (outBlocks) *outBlocks = blocks;
    return out;
}

bool decodeEventColumnsRange(const Schema& schema, const uint8_t* p, size_t n,
                             uint32_t firstEvent, uint32_t lastEvent,
                             std::vector<std::string>& outEvents, std::string& err,
                             uint32_t* outBlocks, std::vector<uint16_t>* outTypeIds) {
    err.clear();
    outEvents.clear();
    if (n < kBlockedHeaderBytes) { err = "分块事件列：段头截断"; return false; }
    const uint32_t version = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (version != kBlockedVersion) { err = "分块事件列：版本不认识"; return false; }
    const uint32_t blockEvents = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    const uint32_t blocks = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (blockEvents == 0 || blocks == 0) { err = "分块事件列：块参数非法"; return false; }
    if (n < kBlockedHeaderBytes + (size_t)(blocks + 1) * 8) { err = "分块事件列：块表截断"; return false; }
    if (outBlocks) *outBlocks = blocks;
    if (lastEvent > blocks * (uint64_t)blockEvents) lastEvent = (uint32_t)(blocks * blockEvents);
    if (firstEvent >= lastEvent) return true;
    const uint32_t b0 = firstEvent / blockEvents;
    const uint32_t b1 = (lastEvent - 1) / blockEvents;
    for (uint32_t b = b0; b <= b1; ++b) {
        const uint64_t o0 = rdU64(p + kBlockedHeaderBytes + (size_t)b * 8);
        const uint64_t o1 = rdU64(p + kBlockedHeaderBytes + (size_t)(b + 1) * 8);
        if (o0 > o1 || o1 > n) { err = "分块事件列：块偏移越界"; return false; }
        EventColumns cols;
        std::string cerr;
        if (!deserializeEventColumns(schema, p + o0, (size_t)(o1 - o0), cols, cerr)) {
            err = "分块事件列：块 " + std::to_string(b) + " 解码失败: " + cerr;
            return false;
        }
        std::vector<std::string> evs;
        if (!decodeEventColumns(schema, cols, cols.typeIds.size(), evs, cerr)) {
            err = "分块事件列：块 " + std::to_string(b) + " 还原失败: " + cerr;
            return false;
        }
        const uint32_t blkFirst = b * blockEvents;
        for (size_t i = 0; i < evs.size(); ++i) {
            const uint32_t idx = blkFirst + (uint32_t)i;
            if (idx < firstEvent || idx >= lastEvent) continue;
            outEvents.push_back(std::move(evs[i]));
            if (outTypeIds) outTypeIds->push_back(cols.typeIds[i]);
        }
    }
    return true;
}

bool decodeEventFieldsBlockedEach(const Schema& schema, const uint8_t* p, size_t n,
                                  EventFieldsFn fn, void* ctx, std::string& err,
                                  uint32_t firstEvent, uint32_t lastEvent) {
    err.clear();
    if (n < kBlockedHeaderBytes) { err = "分块事件列：段头截断"; return false; }
    const uint32_t version = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (version != kBlockedVersion) { err = "分块事件列：版本不认识"; return false; }
    const uint32_t blocks = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    const uint32_t blockEvents = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (blocks == 0 || blockEvents == 0) { err = "分块事件列：块参数非法"; return false; }
    if (n < kBlockedHeaderBytes + (size_t)(blocks + 1) * 8) { err = "分块事件列：块表截断"; return false; }
    // 只解覆盖 [firstEvent,lastEvent) 的块（窗口化加载）
    uint32_t b0 = 0, b1 = blocks - 1;
    if (firstEvent > 0 || lastEvent != 0xFFFFFFFFu) {
        if (lastEvent <= firstEvent) return true;
        b0 = std::min(firstEvent / blockEvents, blocks - 1);
        b1 = std::min((lastEvent - 1) / blockEvents, blocks - 1);
    }
    for (uint32_t b = b0; b <= b1; ++b) {
        const uint64_t o0 = rdU64(p + kBlockedHeaderBytes + (size_t)b * 8);
        const uint64_t o1 = rdU64(p + kBlockedHeaderBytes + (size_t)(b + 1) * 8);
        if (o0 > o1 || o1 > n) { err = "分块事件列：块偏移越界"; return false; }
        if (o1 == o0) continue;
        EventColumns cols;
        std::string cerr;
        if (!deserializeEventColumns(schema, p + o0, (size_t)(o1 - o0), cols, cerr)) {
            err = "分块事件列：块 " + std::to_string(b) + " 解码失败: " + cerr;
            return false;
        }
        FieldBatch batch;
        std::vector<std::string> unused;
        if (!decodeEventColumns(schema, cols, cols.typeIds.size(), unused, cerr, &batch, false)) {
            err = "分块事件列：块 " + std::to_string(b) + " 字段还原失败: " + cerr;
            return false;
        }
        for (size_t i = 0; i + 1 < batch.offsets.size(); ++i) {
            const uint32_t globalIdx = b * blockEvents + (uint32_t)i;
            if (globalIdx < firstEvent || globalIdx >= lastEvent) continue;
            const uint32_t o0 = batch.offsets[i], o1 = batch.offsets[i + 1];
            if (!fn(ctx, batch.values.data() + o0, (size_t)(o1 - o0), cols.typeIds[i], cerr)) {
                err = "分块事件列：块 " + std::to_string(b) + " 直灌失败: " + cerr;
                return false;
            }
        }
    }
    return true;
}

bool decodeEventColumnsBlockedAll(const Schema& schema, const uint8_t* p, size_t n,
                                  std::vector<std::string>& outEvents, std::string& err,
                                  std::vector<uint16_t>* outTypeIds) {
    uint32_t blocks = 0;
    if (!decodeEventColumnsRange(schema, p, n, 0, UINT32_MAX, outEvents, err, &blocks, outTypeIds)) return false;
    return true;
}

bool decodeEventsInFloorRange(const LevelData& lv, int64_t floorLo, int64_t floorHi,
                              std::vector<std::string>& outEvents, std::string& err) {
    err.clear();
    outEvents.clear();
    if (floorHi <= floorLo) return true;
    if (lv.v7SchemaBytes.empty() || lv.v7ColumnsBytes.empty()) { err = "没有 v7 事件段"; return false; }
    Schema schema;
    if (!SchemaBuilder::deserialize(lv.v7SchemaBytes.data(), lv.v7SchemaBytes.size(), schema, err))
        return false;

    uint32_t first = 0, last = UINT32_MAX;
    if (lv.eventBlockSorted && lv.eventBlockStart.size() >= 2 && lv.eventBlockShift <= 20) {
        const uint32_t bs = lv.eventBlockShift;
        const uint32_t lo = (uint32_t)((floorLo < 0 ? 0 : floorLo) >> bs);
        // **末块要取"包含 floorHi-1 的块"的结束**：用 floorHi>>bs 会漏掉同块内的窗口
        // （踩过：[2,6) 落在同一块，first==last==0 → 一条都解不出来）。
        const uint32_t hiBlock = (uint32_t)((floorHi - 1) >> bs);
        const uint32_t nb = (uint32_t)lv.eventBlockStart.size() - 1;
        first = lv.eventBlockStart[std::min(lo, nb)];
        last = lv.eventBlockStart[std::min(hiBlock + 1, nb)];
    }

    std::vector<std::string> raw;
    if (!lv.v7ColumnsBlocked) {
        // 非分块布局：只能全量解（记录在案）
        EventColumns cols;
        if (!deserializeEventColumns(schema, lv.v7ColumnsBytes.data(), lv.v7ColumnsBytes.size(), cols, err))
            return false;
        if (!decodeEventColumns(schema, cols, cols.typeIds.size(), raw, err)) return false;
    } else if (!decodeEventColumnsRange(schema, lv.v7ColumnsBytes.data(), lv.v7ColumnsBytes.size(),
                                        first, last == UINT32_MAX ? UINT32_MAX : last, raw, err)) {
        return false;
    }

    for (auto& js : raw) {
        rapidjson::Document d;
        d.Parse(js.c_str(), js.size());
        if (d.HasParseError() || !d.IsObject()) { err = "事件 JSON 解析失败"; return false; }
        auto it = d.FindMember("floor");
        if (it == d.MemberEnd() || !it->value.IsInt64()) continue;   // 没有 floor 的事件不进窗口
        const int64_t fl = it->value.GetInt64();
        if (fl < floorLo || fl >= floorHi) continue;
        outEvents.push_back(std::move(js));
    }
    return true;
}

size_t zstdEstimate(const std::vector<uint8_t>& raw) {
    if (raw.empty()) return 0;
    std::vector<uint8_t> z(ZSTD_compressBound(raw.size()));
    const size_t got = ZSTD_compress(z.data(), z.size(), raw.data(), raw.size(), 19);
    return ZSTD_isError(got) ? raw.size() : got;
}

}  // namespace adocao
}  // namespace adofai
