#include "archive/AdocaoColumns.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace adofai {
namespace adocao {
namespace {

// ---------------- 定宽小端读写（读取一律带边界检查） ----------------

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
uint16_t rdU16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t rdU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
inline bool have(size_t n, size_t off, size_t k) { return off <= n && k <= n - off; }

// ---------------- varint / zigzag ----------------

void putVarint(std::vector<uint8_t>& b, uint64_t v) {
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7F);
        v >>= 7;
        b.push_back((uint8_t)(x | (v ? 0x80 : 0)));
        if (!v) return;
    }
}
bool getVarint(const uint8_t* p, size_t n, size_t& off, uint64_t& out) {
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (off >= n || shift > 63) return false;
        const uint8_t x = p[off++];
        v |= (uint64_t)(x & 0x7F) << shift;
        if (!(x & 0x80)) { out = v; return true; }
        shift += 7;
    }
}
inline uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
inline int64_t unzigzag(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

// ---------------- 位打包（1..32 位/值，低位在前） ----------------

void bitPack(const std::vector<uint64_t>& vals, int bits, std::vector<uint8_t>& out) {
    out.clear();
    if (bits <= 0 || bits > 32) return;
    const uint64_t mask = (bits == 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
    uint64_t acc = 0;
    int nb = 0;
    for (uint64_t v : vals) {
        acc |= (v & mask) << nb;
        nb += bits;
        while (nb >= 8) {
            out.push_back((uint8_t)(acc & 0xFF));
            acc >>= 8;
            nb -= 8;
        }
    }
    if (nb) out.push_back((uint8_t)(acc & 0xFF));
}

bool bitUnpack(const uint8_t* p, size_t n, size_t count, int bits, std::vector<uint64_t>& out) {
    if (bits <= 0 || bits > 32) return false;
    const uint64_t mask = (bits == 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
    out.assign(count, 0);
    uint64_t acc = 0;
    int nb = 0;
    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        while (nb < bits) {
            if (off >= n) return false;            // 截断：干净失败
            acc |= (uint64_t)p[off++] << nb;
            nb += 8;
        }
        out[i] = acc & mask;
        acc >>= bits;
        nb -= bits;
    }
    return true;
}

// ---------------- 列头 ----------------

void writeColumnHeader(std::vector<uint8_t>& b, Codec c, int bits, uint64_t count, uint64_t aux) {
    putU8(b, (uint8_t)c);
    putU8(b, (uint8_t)bits);
    putU16(b, 0);
    putU64(b, count);
    putU64(b, aux);
}

bool readColumnHeader(const uint8_t* p, size_t n, ColumnHeader& h) {
    if (!have(n, 0, sizeof(ColumnHeader))) return false;
    h.codec = p[0];
    h.bits = p[1];
    h.flags = rdU16(p + 2);
    h.count = rdU64(p + 4);
    h.aux = rdU64(p + 12);
    return true;
}

// 挑最小候选：每个候选都是完整字节流（含列头），比大小即可 —— 这就是"每列按基数选编码"。
struct Choice {
    std::vector<uint8_t> bytes;
    Codec codec = Codec::Raw;
    int bits = 0;
    size_t dict = 0;
    bool valid = false;
};

void consider(Choice& best, Codec c, int bits, size_t dict, std::vector<uint8_t>&& bytes) {
    if (!best.valid || bytes.size() < best.bytes.size()) {
        best.bytes = std::move(bytes);
        best.codec = c;
        best.bits = bits;
        best.dict = dict;
        best.valid = true;
    }
}

const char* codecName(Codec c) {
    switch (c) {
        case Codec::Raw: return "Raw";
        case Codec::Dict: return "Dict";
        case Codec::DeltaVarint: return "DeltaVarint";
        case Codec::Const: return "Const";
        case Codec::BitPack: return "BitPack";
        case Codec::JsonPassthrough: return "JsonPassthrough";
    }
    return "?";
}

int bitsForCount(uint64_t count) {
    int bits = 1;
    while (bits < 32 && (uint64_t(1) << bits) < count) ++bits;
    return bits;
}

}  // namespace

// ---------------- double 列 ----------------

bool encodeDoubleColumn(const std::vector<double>& v, std::vector<uint8_t>& out, ColumnStats* st) {
    const size_t n = v.size();
    std::vector<uint64_t> bits(n);
    for (size_t i = 0; i < n; ++i) std::memcpy(&bits[i], &v[i], sizeof(double));

    Choice best;

    // Const：整列一个值（实测 angleOffset / rotation / opacity 各只有 1 个取值）
    if (n > 0) {
        bool same = true;
        for (size_t i = 1; i < n; ++i)
            if (bits[i] != bits[0]) { same = false; break; }
        if (same) {
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Const, 64, n, 0);
            putU64(b, bits[0]);
            consider(best, Codec::Const, 64, 1, std::move(b));
        }
    }

    // Dict：精确值字典 + 位打包下标（主编码：MYC 6.77M 个角度只有 52 个取值）
    if (n > 1) {
        std::vector<uint64_t> dict(bits);
        std::sort(dict.begin(), dict.end());
        dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
        if (dict.size() < n) {                     // 等于 n 时字典纯亏，不必算
            const int dbits = bitsForCount((uint64_t)dict.size());
            std::vector<uint64_t> idx(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = (uint64_t)(std::lower_bound(dict.begin(), dict.end(), bits[i]) - dict.begin());
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Dict, dbits, n, (uint64_t)dict.size());
            for (uint64_t d : dict) putU64(b, d);
            std::vector<uint8_t> packed;
            bitPack(idx, dbits, packed);
            b.insert(b.end(), packed.begin(), packed.end());
            consider(best, Codec::Dict, dbits, dict.size(), std::move(b));
        }
    }

    // Raw 兜底：定宽 8 B
    {
        std::vector<uint8_t> b;
        writeColumnHeader(b, Codec::Raw, 64, n, 0);
        const size_t off = b.size();
        b.resize(off + n * 8);
        if (n) std::memcpy(b.data() + off, bits.data(), n * 8);
        consider(best, Codec::Raw, 64, 0, std::move(b));
    }

    // 自检：编出来的必须能**逐位**解回原值，否则退回 Raw（宁可不省，不能编错）
    bool ok = false;
    std::vector<double> back;
    if (best.valid && decodeDoubleColumn(best.bytes.data(), best.bytes.size(), back)) {
        ok = (back.size() == n);
        for (size_t i = 0; ok && i < n; ++i)
            if (std::memcmp(&back[i], &v[i], sizeof(double)) != 0) ok = false;
    }
    if (!ok) {
        best.bytes.clear();
        writeColumnHeader(best.bytes, Codec::Raw, 64, n, 0);
        const size_t off = best.bytes.size();
        best.bytes.resize(off + n * 8);
        if (n) std::memcpy(best.bytes.data() + off, bits.data(), n * 8);
        best.codec = Codec::Raw;
        best.bits = 64;
        best.dict = 0;
    }

    out = std::move(best.bytes);
    if (st) {
        st->codec = ok ? codecName(best.codec) : "Raw(fallback)";
        st->rawBytes = n * 8;
        st->encodedBytes = out.size();
        st->dictEntries = best.dict;
        st->bits = best.bits;
    }
    return true;
}

bool decodeDoubleColumn(const uint8_t* p, size_t n, std::vector<double>& out) {
    ColumnHeader h{};
    if (!readColumnHeader(p, n, h)) return false;
    if (h.count > (uint64_t)1 << 40) return false;             // 荒谬的 count：直接拒绝（防溢出）
    const size_t count = (size_t)h.count;
    const uint8_t* body = p + sizeof(ColumnHeader);
    const size_t bodyN = n - sizeof(ColumnHeader);

    if (h.codec == (uint8_t)Codec::Raw) {
        if (h.bits != 64) return false;
        if (!have(bodyN, 0, count * 8)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint64_t b = rdU64(body + i * 8);
            std::memcpy(&out[i], &b, sizeof(double));
        }
        return true;
    }
    if (h.codec == (uint8_t)Codec::Const) {
        if (!have(bodyN, 0, 8)) return false;
        const uint64_t b = rdU64(body);
        double d;
        std::memcpy(&d, &b, sizeof(double));
        out.assign(count, d);
        return true;
    }
    if (h.codec == (uint8_t)Codec::Dict) {
        const size_t dictN = (size_t)h.aux;
        if (dictN == 0 || dictN > count) return false;
        if (!have(bodyN, 0, dictN * 8)) return false;
        std::vector<double> dict(dictN);
        for (size_t i = 0; i < dictN; ++i) {
            const uint64_t b = rdU64(body + i * 8);
            std::memcpy(&dict[i], &b, sizeof(double));
        }
        std::vector<uint64_t> idx;
        if (!bitUnpack(body + dictN * 8, bodyN - dictN * 8, count, h.bits, idx)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (idx[i] >= dictN) return false;                 // 越界下标 = 坏数据
            out[i] = dict[(size_t)idx[i]];
        }
        return true;
    }
    return false;                                              // 未知/不该出现在这里的 codec
}

// ---------------- int64 列 ----------------

bool encodeIntColumn(const std::vector<int64_t>& v, std::vector<uint8_t>& out, ColumnStats* st) {
    const size_t n = v.size();
    Choice best;

    if (n > 0) {
        bool same = true;
        for (size_t i = 1; i < n; ++i)
            if (v[i] != v[0]) { same = false; break; }
        if (same) {
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Const, 64, n, 0);
            putU64(b, (uint64_t)v[0]);
            consider(best, Codec::Const, 64, 1, std::move(b));
        }
    }

    // DeltaVarint：首值 zigzag varint，其后是 zigzag(相邻差)。单调列（floor）几乎全是小正差。
    if (n > 0) {
        std::vector<uint8_t> b;
        writeColumnHeader(b, Codec::DeltaVarint, 0, n, 0);
        putVarint(b, zigzag(v[0]));
        for (size_t i = 1; i < n; ++i) putVarint(b, zigzag(v[i] - v[i - 1]));
        consider(best, Codec::DeltaVarint, 0, 0, std::move(b));
    }

    // Dict：取值重复度高时更划算（例如 floor 只在少数几层上出现）
    if (n > 1) {
        std::vector<int64_t> dict(v);
        std::sort(dict.begin(), dict.end());
        dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
        if (dict.size() < n) {
            const int dbits = bitsForCount((uint64_t)dict.size());
            std::vector<uint64_t> idx(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = (uint64_t)(std::lower_bound(dict.begin(), dict.end(), v[i]) - dict.begin());
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Dict, dbits, n, (uint64_t)dict.size());
            for (int64_t d : dict) putU64(b, (uint64_t)d);
            std::vector<uint8_t> packed;
            bitPack(idx, dbits, packed);
            b.insert(b.end(), packed.begin(), packed.end());
            consider(best, Codec::Dict, dbits, dict.size(), std::move(b));
        }
    }

    // Raw：定宽 8 B
    {
        std::vector<uint8_t> b;
        writeColumnHeader(b, Codec::Raw, 64, n, 0);
        const size_t off = b.size();
        b.resize(off + n * 8);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t u = (uint64_t)v[i];
            for (int k = 0; k < 8; ++k) b[off + i * 8 + (size_t)k] = (uint8_t)((u >> (8 * k)) & 0xFF);
        }
        consider(best, Codec::Raw, 64, 0, std::move(b));
    }

    bool ok = false;
    std::vector<int64_t> back;
    if (best.valid && decodeIntColumn(best.bytes.data(), best.bytes.size(), back)) {
        ok = (back.size() == n) && (n == 0 || std::memcmp(back.data(), v.data(), n * sizeof(int64_t)) == 0);
    }
    if (!ok) {
        best.bytes.clear();
        writeColumnHeader(best.bytes, Codec::Raw, 64, n, 0);
        const size_t off = best.bytes.size();
        best.bytes.resize(off + n * 8);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t u = (uint64_t)v[i];
            for (int k = 0; k < 8; ++k) best.bytes[off + i * 8 + (size_t)k] = (uint8_t)((u >> (8 * k)) & 0xFF);
        }
        best.codec = Codec::Raw;
        best.bits = 64;
        best.dict = 0;
    }

    out = std::move(best.bytes);
    if (st) {
        st->codec = ok ? codecName(best.codec) : "Raw(fallback)";
        st->rawBytes = n * 8;
        st->encodedBytes = out.size();
        st->dictEntries = best.dict;
        st->bits = best.bits;
    }
    return true;
}

bool decodeIntColumn(const uint8_t* p, size_t n, std::vector<int64_t>& out) {
    ColumnHeader h{};
    if (!readColumnHeader(p, n, h)) return false;
    if (h.count > (uint64_t)1 << 40) return false;
    const size_t count = (size_t)h.count;
    const uint8_t* body = p + sizeof(ColumnHeader);
    const size_t bodyN = n - sizeof(ColumnHeader);

    if (h.codec == (uint8_t)Codec::Raw) {
        if (h.bits != 64) return false;
        if (!have(bodyN, 0, count * 8)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = (int64_t)rdU64(body + i * 8);
        return true;
    }
    if (h.codec == (uint8_t)Codec::Const) {
        if (!have(bodyN, 0, 8)) return false;
        out.assign(count, (int64_t)rdU64(body));
        return true;
    }
    if (h.codec == (uint8_t)Codec::DeltaVarint) {
        out.resize(count);
        size_t off = 0;
        uint64_t u = 0;
        if (count && !getVarint(body, bodyN, off, u)) return false;
        int64_t cur = count ? unzigzag(u) : 0;
        for (size_t i = 0; i < count; ++i) {
            if (i) {
                if (!getVarint(body, bodyN, off, u)) return false;
                cur += unzigzag(u);
            }
            out[i] = cur;
        }
        return true;
    }
    if (h.codec == (uint8_t)Codec::Dict) {
        const size_t dictN = (size_t)h.aux;
        if (dictN == 0 || dictN > count) return false;
        if (!have(bodyN, 0, dictN * 8)) return false;
        std::vector<int64_t> dict(dictN);
        for (size_t i = 0; i < dictN; ++i) dict[i] = (int64_t)rdU64(body + i * 8);
        std::vector<uint64_t> idx;
        if (!bitUnpack(body + dictN * 8, bodyN - dictN * 8, count, h.bits, idx)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (idx[i] >= dictN) return false;
            out[i] = dict[(size_t)idx[i]];
        }
        return true;
    }
    return false;
}

// ---------------- uint32 列 ----------------

bool encodeU32Column(const std::vector<uint32_t>& v, std::vector<uint8_t>& out, ColumnStats* st) {
    const size_t n = v.size();
    Choice best;

    if (n > 0) {
        bool same = true;
        uint32_t mx = 0;
        for (size_t i = 0; i < n; ++i) {
            if (v[i] != v[0]) same = false;
            if (v[i] > mx) mx = v[i];
        }
        if (same) {
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Const, 32, n, 0);
            putU32(b, v[0]);
            consider(best, Codec::Const, 32, 1, std::move(b));
        }
        // BitPack：小枚举（eventType 0..7 / 位标志）—— 位宽 = ceil(log2(max+1))
        int bits = 0;
        while (bits < 32 && (uint64_t(1) << bits) <= (uint64_t)mx) ++bits;
        if (bits > 0 && (size_t)bits * n + 8 < n * 4) {
            std::vector<uint64_t> vals(n);
            for (size_t i = 0; i < n; ++i) vals[i] = v[i];
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::BitPack, bits, n, 0);
            std::vector<uint8_t> packed;
            bitPack(vals, bits, packed);
            b.insert(b.end(), packed.begin(), packed.end());
            consider(best, Codec::BitPack, bits, 0, std::move(b));
        }
    }

    if (n > 1) {
        std::vector<uint32_t> dict(v);
        std::sort(dict.begin(), dict.end());
        dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
        if (dict.size() < n) {
            const int dbits = bitsForCount((uint64_t)dict.size());
            std::vector<uint64_t> idx(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = (uint64_t)(std::lower_bound(dict.begin(), dict.end(), v[i]) - dict.begin());
            std::vector<uint8_t> b;
            writeColumnHeader(b, Codec::Dict, dbits, n, (uint64_t)dict.size());
            for (uint32_t d : dict) putU32(b, d);
            std::vector<uint8_t> packed;
            bitPack(idx, dbits, packed);
            b.insert(b.end(), packed.begin(), packed.end());
            consider(best, Codec::Dict, dbits, dict.size(), std::move(b));
        }
    }

    {
        std::vector<uint8_t> b;
        writeColumnHeader(b, Codec::Raw, 32, n, 0);
        for (size_t i = 0; i < n; ++i) putU32(b, v[i]);
        consider(best, Codec::Raw, 32, 0, std::move(b));
    }

    bool ok = false;
    std::vector<uint32_t> back;
    if (best.valid && decodeU32Column(best.bytes.data(), best.bytes.size(), back)) {
        ok = (back.size() == n) && (n == 0 || std::memcmp(back.data(), v.data(), n * sizeof(uint32_t)) == 0);
    }
    if (!ok) {
        best.bytes.clear();
        writeColumnHeader(best.bytes, Codec::Raw, 32, n, 0);
        for (size_t i = 0; i < n; ++i) putU32(best.bytes, v[i]);
        best.codec = Codec::Raw;
        best.bits = 32;
        best.dict = 0;
    }

    out = std::move(best.bytes);
    if (st) {
        st->codec = ok ? codecName(best.codec) : "Raw(fallback)";
        st->rawBytes = n * 4;
        st->encodedBytes = out.size();
        st->dictEntries = best.dict;
        st->bits = best.bits;
    }
    return true;
}

bool decodeU32Column(const uint8_t* p, size_t n, std::vector<uint32_t>& out) {
    ColumnHeader h{};
    if (!readColumnHeader(p, n, h)) return false;
    if (h.count > (uint64_t)1 << 40) return false;
    const size_t count = (size_t)h.count;
    const uint8_t* body = p + sizeof(ColumnHeader);
    const size_t bodyN = n - sizeof(ColumnHeader);

    if (h.codec == (uint8_t)Codec::Raw) {
        if (h.bits != 32) return false;
        if (!have(bodyN, 0, count * 4)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = rdU32(body + i * 4);
        return true;
    }
    if (h.codec == (uint8_t)Codec::Const) {
        if (!have(bodyN, 0, 4)) return false;
        out.assign(count, rdU32(body));
        return true;
    }
    if (h.codec == (uint8_t)Codec::BitPack) {
        std::vector<uint64_t> vals;
        if (!bitUnpack(body, bodyN, count, h.bits, vals)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = (uint32_t)vals[i];
        return true;
    }
    if (h.codec == (uint8_t)Codec::Dict) {
        const size_t dictN = (size_t)h.aux;
        if (dictN == 0 || dictN > count) return false;
        if (!have(bodyN, 0, dictN * 4)) return false;
        std::vector<uint32_t> dict(dictN);
        for (size_t i = 0; i < dictN; ++i) dict[i] = rdU32(body + i * 4);
        std::vector<uint64_t> idx;
        if (!bitUnpack(body + dictN * 4, bodyN - dictN * 4, count, h.bits, idx)) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (idx[i] >= dictN) return false;
            out[i] = dict[(size_t)idx[i]];
        }
        return true;
    }
    return false;
}

// ---------------- CRC32C ----------------

uint32_t crc32c(const void* data, size_t len, uint32_t seed) {
    static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace adocao
}  // namespace adofai
