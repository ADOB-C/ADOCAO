#include "Ilytx.hpp"

#include "Ilybin.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

namespace adofai {
namespace archive {

namespace {

constexpr size_t kBlock = 512;

// tar 的数字字段是**八进制 ASCII**、以 NUL 或空格结尾；只认纯八进制数字。
// 空字段（有些写入方留全 NUL）当 0。
bool readOctal(const char* p, size_t n, size_t& out) {
    out = 0;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)p[i];
        if (c == 0 || c == ' ') {
            if (any) break;
            continue;                       // 前导 NUL/空格（有些写入方补在前面）
        }
        if (c < '0' || c > '7') return false;
        if (out > (~size_t{0} >> 3)) return false;
        out = out * 8 + (size_t)(c - '0');
        any = true;
    }
    return true;
}

struct TarMember {
    const char* name = nullptr;   // 指向 tar 缓冲区里的字段本身（不一定是 NUL 结尾）
    size_t nameLen = 0;
    const char* data = nullptr;
    size_t size = 0;
};

std::string memberName(const TarMember& m) {
    return std::string(m.name, m.nameLen);
}

// 在 tar 里按名字找成员（`level.ilybin`）。找到第一次出现即可。
bool findMember(const char* tar, size_t length, const char* want, const char*& data, size_t& size,
                std::string& reason) {
    size_t off = 0;
    bool first = true;
    while (off + kBlock <= length) {
        const char* hdr = tar + off;
        if (hdr[0] == '\0') break;                      // 结束标记（后面全是填充 0）
        if (first && std::memcmp(hdr + 257, "ustar", 5) != 0) {
            reason = "不是 ustar tar（偏移 257 没有 ustar magic）";
            return false;
        }
        first = false;

        TarMember m;
        m.name = hdr;
        m.nameLen = strnlen(hdr, 100);
        const char* prefix = hdr + 345;
        size_t prefixLen = strnlen(prefix, 155);
        const char typeflag = hdr[156];

        size_t memberSize = 0;
        if (!readOctal(hdr + 124, 12, memberSize)) { reason = "tar 头 size 字段不是八进制"; return false; }
        if (memberSize > length) { reason = "tar 成员 size 大得不可能（> 整个缓冲区）"; return false; }
        if (off + kBlock + memberSize > length) { reason = "tar 成员数据越界（文件被截断？）"; return false; }

        m.data = hdr + kBlock;
        m.size = memberSize;

        const std::string leaf = (prefixLen > 0) ? (std::string(prefix, prefixLen) + "/" + memberName(m))
                                                 : memberName(m);
        // 只认普通文件；目录（'5'）与 GNU 扩展（'L'/'K'）跳过/报错
        if (typeflag == '0' || typeflag == '\0') {
            const size_t wl = strlen(want);
            if (leaf == want || (leaf.size() > wl && leaf.compare(leaf.size() - wl, wl, want) == 0
                                 && leaf[leaf.size() - wl - 1] == '/')) {
                data = m.data;
                size = m.size;
                return true;
            }
        } else if (typeflag == 'L' || typeflag == 'K') {
            reason = "tar 里有 GNU 长名字扩展（本实现不支持；Re_ADOJAS 的写入方不会产生它）";
            return false;
        }

        off += kBlock + ((memberSize + kBlock - 1) / kBlock) * kBlock;
    }
    reason = std::string("tar 里没有 ") + want;
    return false;
}

// double → JSON 数字（**最短往返**表示，locale 无关；`%g` 会受 LC_NUMERIC 影响 ✗）
bool appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) return false;
    char buf[40];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec != std::errc{}) return false;
    out.append(buf, res.ptr);
    return true;
}

// settings 段里的成员（去掉 data 三段，避免与我们的 angleData/actions/decorations 重复）。
// 用 RapidJSON 走一遍是**必要**的：dup key 在多数解析器里行为不明，而这里的输入是别人的
// 导出物，不能假设它干净。settings 本身很小（不含 angleData），DOM 开销可以忽略。
// 没有冲突时**原样透传文本**（保留对方的字段顺序/排版）；有冲突才 DOM 重排。
bool appendSettingsMembers(const std::string& settingsJson, std::string& out, std::string& reason) {
    rapidjson::Document d;
    d.Parse<rapidjson::kParseTrailingCommasFlag>(settingsJson.c_str(), settingsJson.size());
    if (d.HasParseError() || !d.IsObject()) { reason = "settings 段不是 JSON 对象"; return false; }

    std::string inner;
    const bool hasDup = d.HasMember("angleData") || d.HasMember("actions")
                     || d.HasMember("decorations");
    if (!hasDup) {
        const size_t b = settingsJson.find('{');
        const size_t e = settingsJson.rfind('}');
        if (b == std::string::npos || e == std::string::npos || e <= b) {
            reason = "settings 段首尾不是对象括号";
            return false;
        }
        inner.assign(settingsJson, b + 1, e - b - 1);
    } else {
        d.RemoveMember("angleData");
        d.RemoveMember("actions");
        d.RemoveMember("decorations");
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        if (!d.Accept(w)) { reason = "settings 段重新序列化失败"; return false; }
        inner.assign(sb.GetString() + 1, sb.GetSize() - 2);       // 去掉外层 {}
    }
    // 全是空白 = 空对象：别拼出 `{,}` 这种非法 JSON
    bool blank = true;
    for (char c : inner) {
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) { blank = false; break; }
    }
    if (!blank) {
        out += ',';
        out += inner;
    }
    return true;
}

}  // namespace

bool ilytxToAdofaiJson(const char* tar, size_t length, std::string& out, std::string& reason) {
    if (!tar || length < kBlock) { reason = "载荷太短（连一个 tar 块都不够）"; return false; }

    const char* levelData = nullptr;
    size_t levelSize = 0;
    if (!findMember(tar, length, "level.ilybin", levelData, levelSize, reason)) return false;

    IlybinLevel ily;
    if (!decodeIlybin((const uint8_t*)levelData, levelSize, ily, reason)) return false;

    out.clear();
    out.reserve(ily.direction.size() * 12 + 1024);
    out += "{\"angleData\":[";
    for (size_t i = 0; i < ily.direction.size(); ++i) {
        if (i) out += ',';
        // direction 就是 angleData 的值：这里必须是**位精确**往返（f64 / 缩放差分都是 double，
        // f32 源已经是它自己的精度），to_chars 的最短往返正好满足
        if (!appendNumber(out, ily.direction[i])) {
            reason = "direction 里有非有限值（NaN/Inf）—— 不是合法谱面";
            return false;
        }
    }
    out += ']';

    if (!ily.actionsJson.empty()) {
        out += ",\"actions\":";
        out += ily.actionsJson;
    }
    if (!ily.decorationsJson.empty()) {
        out += ",\"decorations\":";
        out += ily.decorationsJson;
    }
    if (!appendSettingsMembers(ily.settingsJson, out, reason)) return false;
    out += '}';
    // 诊断钩子：把"翻译出来的明文"落盘，便于对照 RapidJSON 的报错偏移（不参与正常流程）
    if (const char* dump = std::getenv("ADOCAO_ILYTX_DUMP")) {
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
    }
    return true;
}

}  // namespace archive
}  // namespace adofai
