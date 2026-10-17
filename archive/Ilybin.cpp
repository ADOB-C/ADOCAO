#include "Ilybin.hpp"

#include <cstring>
#include <map>

namespace adofai {
namespace archive {

namespace {

constexpr uint8_t  kIlybinVersion = 1;
constexpr uint32_t kMagic         = 0x42594c49u;   // "ILYB"（小端 u32，即字节 I L Y B）
constexpr size_t   kHeaderSize    = 12;

// section id（他们的常量；1/2/5 必需，3/6/7/8 可选）
constexpr uint8_t kSecSettings = 1, kSecDirection = 2, kSecTwirl = 5;

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;

    bool need(size_t k) const { return off + k <= n; }
    bool u8(uint8_t& v) { if (!need(1)) return false; v = p[off++]; return true; }
    bool u16(uint16_t& v) {
        if (!need(2)) return false;
        v = (uint16_t)(p[off] | (p[off + 1] << 8));
        off += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        v = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16)
            | ((uint32_t)p[off + 3] << 24);
        off += 4;
        return true;
    }
    bool f64(double& v) {
        if (!need(8)) return false;
        uint64_t bits = 0;
        for (int i = 7; i >= 0; --i) bits = (bits << 8) | p[off + i];
        std::memcpy(&v, &bits, 8);
        off += 8;
        return true;
    }
    // 无符号 LEB128（他们接受 > 2^31 的值，按 7 bit 组编码）
    bool varint(uint64_t& v) {
        v = 0;
        for (int shift = 0;; shift += 7) {
            if (!need(1)) return false;
            const uint8_t c = p[off++];
            v |= (uint64_t)(c & 0x7f) << shift;
            if (!(c & 0x80)) return true;
            if (shift >= 63) return false;          // 越界编码
        }
    }
    // ZigZag 变长整数（有符号）
    bool svarint(int64_t& v) {
        uint64_t u = 0;
        if (!varint(u)) return false;
        v = (int64_t)((u >> 1) ^ (~(u & 1) + 1));   // (u>>1) ^ -(u&1)，避免实现定义行为
        return true;
    }
};

// FloatSection 的三种编码（kind 0 缩放整数差分 / 1 原始 f64 / 2 f32 定宽）
bool decodeFloatSection(const uint8_t* p, size_t n, uint32_t count, std::vector<double>& out,
                        std::string& reason) {
    Reader r{p, n};
    uint8_t kind = 0;
    if (!r.u8(kind)) { reason = "浮点段为空"; return false; }
    out.clear();
    out.reserve(count);
    if (kind == 1) {                                   // 原始 f64（位精确兜底）
        for (uint32_t i = 0; i < count; ++i) {
            double v = 0;
            if (!r.f64(v)) { reason = "浮点段（f64）数据不足"; return false; }
            out.push_back(v);
        }
        return true;
    }
    if (kind == 2) {                                   // f32 定宽（紧凑模式原生精度）
        if (r.n - r.off < (size_t)count * 4) { reason = "浮点段（f32）数据不足"; return false; }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t bits = (uint32_t)p[r.off] | ((uint32_t)p[r.off + 1] << 8)
                                | ((uint32_t)p[r.off + 2] << 16) | ((uint32_t)p[r.off + 3] << 24);
            float f = 0;
            std::memcpy(&f, &bits, 4);
            r.off += 4;
            out.push_back((double)f);
        }
        return true;
    }
    if (kind != 0) { reason = "未知浮点编码 kind=" + std::to_string((int)kind); return false; }
    uint16_t scale = 0;
    if (!r.u16(scale) || scale == 0) { reason = "浮点段（缩放差分）scale 非法"; return false; }
    int64_t acc = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int64_t d = 0;
        if (!r.svarint(d)) { reason = "浮点段（缩放差分）数据不足或 varint 越界"; return false; }
        acc += d;
        out.push_back((double)acc / (double)scale);
    }
    return true;
}

bool decodeTwirlCounted(const uint8_t* p, size_t n, uint32_t count, std::string& reason) {
    // twirl 段是**派生量**（累计 Twirl 计数），ADOCAO 的权威是 actions 里的 Twirl 事件。
    // 所以这里只校验它能不能完整解出来（段坏了 = 文件坏了，要报出来），值本身不落盘。
    Reader r{p, n};
    int64_t acc = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int64_t d = 0;
        if (!r.svarint(d)) { reason = "twirl 段数据不足或 varint 越界"; return false; }
        acc += d;
        if (acc < 0 || acc > 0x7fffffff) { reason = "twirl 累计值越界"; return false; }
    }
    return true;
}

}  // namespace

bool decodeIlybin(const uint8_t* data, size_t length, IlybinLevel& out, std::string& reason) {
    if (!data || length < kHeaderSize) { reason = "ilybin 太短（连 12 字节头都不够）"; return false; }
    Reader r{data, length};
    uint32_t magic = 0;
    r.u32(magic);
    if (magic != kMagic) { reason = "ilybin magic 不是 \"ILYB\""; return false; }
    uint8_t version = 0, tileMode = 0;
    uint16_t sectionCount = 0;
    uint32_t tileCount = 0;
    r.u8(version);
    r.u8(tileMode);
    r.u16(sectionCount);
    r.u32(tileCount);
    if (version != kIlybinVersion) {
        reason = "不支持的 ilybin 版本 " + std::to_string((int)version);
        return false;
    }
    if (tileMode > 1) { reason = "非法 tileMode=" + std::to_string((int)tileMode); return false; }
    // 目标上限 2^31−1 层；ilybin 的 tileCount 是 u32，按上限卡住免得分配被撑爆
    if (tileCount > 0x7fffffffu) { reason = "tileCount 超过 2^31−1"; return false; }

    std::map<uint8_t, const uint8_t*> secData;
    std::map<uint8_t, size_t> secLen;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        uint8_t id = 0;
        uint32_t len = 0;
        if (!r.u8(id) || !r.u32(len)) { reason = "section 头越界"; return false; }
        if (r.off + len > length) { reason = "section 数据越界"; return false; }
        secData[id] = data + r.off;
        secLen[id] = len;
        r.off += len;
    }

    if (!secData.count(kSecSettings)) { reason = "缺少 settings 段"; return false; }
    if (!secData.count(kSecDirection)) { reason = "缺少 direction 段"; return false; }
    if (!secData.count(kSecTwirl)) { reason = "缺少 twirl 段"; return false; }

    out = IlybinLevel{};
    out.tileMode = tileMode;
    out.tileCount = tileCount;
    out.settingsJson.assign((const char*)secData[kSecSettings], secLen[kSecSettings]);
    if (!decodeFloatSection(secData[kSecDirection], secLen[kSecDirection], tileCount, out.direction, reason))
        return false;
    if (!decodeTwirlCounted(secData[kSecTwirl], secLen[kSecTwirl], tileCount, reason)) return false;

    if (secData.count(6)) out.actionsJson.assign((const char*)secData[6], secLen[6]);
    if (secData.count(7)) out.decorationsJson.assign((const char*)secData[7], secLen[7]);
    return true;
}

}  // namespace archive
}  // namespace adofai
