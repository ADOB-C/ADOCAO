#include "archive/AdocaoReader.hpp"

#include <cstring>
#include <vector>

#include "archive/AdocaoColumns.hpp"
#include "archive/AdocaoFormat.hpp"
#include "archive/LevelArchive.hpp"   // 段级解压复用现成的 decompressLevelArchive
#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {
namespace {

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

struct Sec {
    uint8_t id = 0;
    const uint8_t* p = nullptr;
    size_t size = 0;
    uint64_t elems = 0;      // elemCount（元素个数）
    size_t elems0Raw = 0;    // 段表里的 rawSize（解压后的长度，用来核对）
};

const Sec* findSec(const std::vector<Sec>& v, SectionId id) {
    for (const Sec& s : v)
        if (s.id == (uint8_t)id) return &s;
    return nullptr;
}

// 与 AdocaoWriter::writeSettings 逐字段对称（顺序改一处就要改两处）
bool parseSettings(const std::vector<std::string>& pool, const uint8_t* p, size_t n,
                   LevelData::Settings& s) {
    size_t off = 0;
    auto u8 = [&](uint8_t& v) { if (off + 1 > n) return false; v = p[off++]; return true; };
    auto u16 = [&](uint16_t& v) { if (off + 2 > n) return false; v = rdU16(p + off); off += 2; return true; };
    auto u32 = [&](uint32_t& v) { if (off + 4 > n) return false; v = rdU32(p + off); off += 4; return true; };
    auto f32 = [&](float& v) {
        uint32_t x = 0;
        if (!u32(x)) return false;
        std::memcpy(&v, &x, 4);
        return true;
    };
    auto idx = [&](std::string& dst) {
        uint16_t i = 0;
        if (!u16(i)) return false;
        if (i >= pool.size()) return false;            // 越界下标 = 坏数据
        dst = pool[i];
        return true;
    };
    uint32_t vi = 0;
    if (!u32(vi)) return false;
    s.version = (int)vi;
    if (!f32(s.bpm) || !f32(s.offset)) return false;
    if (!u32(vi)) return false;
    s.countdownTicks = (int)vi;
    if (!f32(s.zoom) || !f32(s.rotation)) return false;
    if (!idx(s.relativeTo)) return false;
    if (!f32(s.position[0]) || !f32(s.position[1])) return false;
    if (!idx(s.hitsound)) return false;
    if (!f32(s.hitsoundVolume)) return false;
    if (!idx(s.trackColor) || !idx(s.secondaryTrackColor) || !idx(s.backgroundColor)) return false;
    uint8_t b = 0;
    if (!u8(b)) return false;
    s.stickToFloors = (b != 0);
    if (!idx(s.planetEase) || !idx(s.trackDisappearAnimation) || !idx(s.trackAnimation)) return false;
    if (!f32(s.beatsBehind) || !f32(s.beatsAhead)) return false;
    return true;
}

bool parsePool(const uint8_t* p, size_t n, std::vector<std::string>& out) {
    if (!have(n, 0, 4)) return false;
    const uint32_t count = rdU32(p);
    if (count == 0 || count > 1000000u) return false;   // v[0] 必须是空串
    size_t off = 4;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!have(n, off, 4)) return false;
        const uint32_t len = rdU32(p + off);
        off += 4;
        if (!have(n, off, len)) return false;
        out.emplace_back((const char*)p + off, len);
        off += len;
    }
    return true;
}

}  // namespace

bool unpackLevel(const uint8_t* data, size_t length, LevelData& out, std::string& err) {
    err.clear();
    if (!data || !have(length, 0, sizeof(Header))) {
        err = "文件太短（连文件头都不完整）";
        return false;
    }
    if (std::memcmp(data, kMagic, 4) != 0) {
        err = "magic 不符（不是 .adocao）";
        return false;
    }
    const uint16_t version = rdU16(data + 4);
    if (version != kVersion) {
        err = "版本不支持：文件是 v" + std::to_string(version) + "，本程序只认 v" +
              std::to_string(kVersion);
        return false;
    }
    const uint16_t sectionCount = rdU16(data + 8);
    const uint64_t fileSize = rdU64(data + 12);
    if (fileSize > length) {
        err = "文件被截断（头里写着 " + std::to_string(fileSize) + " 字节，实际只有 " +
              std::to_string(length) + "）";
        return false;
    }
    const size_t tableBytes = sizeof(Header) + (size_t)sectionCount * sizeof(SectionEntry);
    if (!have(length, 0, tableBytes)) {
        err = "段表越界";
        return false;
    }
    // headerCrc 覆盖"头（该字段按 0 参与）+ 整张段表"
    {
        std::vector<uint8_t> hb(data, data + tableBytes);
        std::memset(hb.data() + 20, 0, 4);
        if (crc32c(hb.data(), hb.size()) != rdU32(data + 20)) {
            err = "headerCrc 校验失败（文件损坏）";
            return false;
        }
    }

    std::vector<Sec> secs;
    secs.reserve(sectionCount);
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const uint8_t* e = data + sizeof(Header) + (size_t)i * sizeof(SectionEntry);
        Sec s;
        s.id = e[0];
        const uint64_t off = rdU64(e + 4);
        const uint64_t size = rdU64(e + 12);
        s.elems = rdU64(e + 28);
        s.elems0Raw = (size_t)rdU64(e + 20);   // rawSize
        if (!have(length, (size_t)off, (size_t)size)) {
            err = "段 " + std::to_string(s.id) + " 越界（偏移 " + std::to_string(off) + " 长度 " +
                  std::to_string(size) + "）";
            return false;
        }
        s.p = data + off;
        s.size = (size_t)size;
        if (crc32c(s.p, s.size) != rdU32(e + 36)) {
            err = "段 " + std::to_string(s.id) + " 的 crc32c 校验失败（文件损坏）";
            return false;
        }
        secs.push_back(s);
    }

    // 段级解压：crc32c 已在上面校验过**存储字节**，这里再解开。未知段编码 → 明确失败（不猜）。
    std::vector<std::string> plain(secs.size());   // decompressLevelArchive 的出参是 string
    for (size_t i = 0; i < secs.size(); ++i) {
        Sec& s = secs[i];
        const uint8_t sc = data[sizeof(Header) + i * sizeof(SectionEntry) + 1];
        if (sc == (uint8_t)Codec::Raw) continue;
        LevelArchiveKind kind;
        if (sc == (uint8_t)Codec::SectionZstd) kind = LevelArchiveKind::Zstd;
        else if (sc == (uint8_t)Codec::SectionXz) kind = LevelArchiveKind::Xz;
        else {
            err = "段 " + std::to_string(s.id) + " 的编码 " + std::to_string(sc) + " 不认识";
            return false;
        }
        std::string why;
        if (!decompressLevelArchive((const char*)s.p, s.size, kind, plain[i], why)) {
            err = "段 " + std::to_string(s.id) + " 解压失败: " + why;
            return false;
        }
        if (plain[i].size() != s.elems0Raw) {   // 解压出来的长度必须与段表里写的一致
            err = "段 " + std::to_string(s.id) + " 解压后长度与段表不符（文件损坏）";
            return false;
        }
        s.p = (const uint8_t*)plain[i].data();
        s.size = plain[i].size();
    }

    // 清干净：调用方可能已经用过这个对象
    out.angleData.clear();
    out.actions.clear();
    out.pathData.clear();

    // ① 字符串池必须在最前（settings 的字符串下标、actions 的 strId 都指向它）
    const Sec* poolSec = findSec(secs, SectionId::StringPool);
    if (!poolSec) {
        err = "缺少字符串池段";
        return false;
    }
    if (!parsePool(poolSec->p, poolSec->size, out.actionStrTable)) {
        err = "字符串池解析失败";
        return false;
    }

    // ② settings
    const Sec* setSec = findSec(secs, SectionId::Settings);
    if (!setSec) {
        err = "缺少 settings 段";
        return false;
    }
    {
        LevelData::Settings s = out.settings;      // 与两条 JSON 路径一致：只覆盖文件里出现的字段
        if (!parseSettings(out.actionStrTable, setSec->p, setSec->size, s)) {
            err = "settings 记录解析失败";
            return false;
        }
        out.settings = s;
    }

    // ③ angleData（一列 double）
    const Sec* angSec = findSec(secs, SectionId::AngleData);
    if (angSec && angSec->size) {
        if (!decodeDoubleColumn(angSec->p, angSec->size, out.angleData)) {
            err = "angleData 列解码失败";
            return false;
        }
    }

    // ④ actions（列式：floor / type / strId / flag / val1 / val2）
    const Sec* actSec = findSec(secs, SectionId::Actions);
    if (actSec && actSec->size) {
        const uint8_t* p = actSec->p;
        const size_t n = actSec->size;
        if (!have(n, 0, 4)) {
            err = "actions 段太短";
            return false;
        }
        const uint32_t cols = rdU32(p);
        if (cols != 6) {
            err = "actions 列数不是 6（是 " + std::to_string(cols) + "）";
            return false;
        }
        std::vector<const uint8_t*> cp(6, nullptr);
        std::vector<size_t> cn(6, 0);
        size_t off = 4;
        for (uint32_t i = 0; i < cols; ++i) {
            if (!have(n, off, 8)) {
                err = "actions 列长度字段越界";
                return false;
            }
            const uint64_t len = rdU64(p + off);
            off += 8;
            if (!have(n, off, (size_t)len)) {
                err = "actions 第 " + std::to_string(i) + " 列越界";
                return false;
            }
            cp[i] = p + off;
            cn[i] = (size_t)len;
            off += (size_t)len;
        }
        std::vector<int64_t> floors;
        std::vector<uint32_t> types, flags;
        // 稀疏列：u32 类型掩码 + u64 实际条数 + 一列（只含掩码内的类型）
        auto readSparse = [](uint32_t& mask, std::vector<uint32_t>& vals, const uint8_t* c, size_t csz) {
            if (!have(csz, 0, 12)) return false;
            mask = rdU32(c);
            const uint64_t used = rdU64(c + 4);
            if (!decodeU32Column(c + 12, csz - 12, vals)) return false;
            return vals.size() == used;
        };
        std::vector<uint32_t> strs, v1, v2;
        uint32_t mStr = 0, mV1 = 0, mV2 = 0;
        if (!decodeIntColumn(cp[0], cn[0], floors) || !decodeU32Column(cp[1], cn[1], types) ||
            !readSparse(mStr, strs, cp[2], cn[2]) || !decodeU32Column(cp[3], cn[3], flags) ||
            !readSparse(mV1, v1, cp[4], cn[4]) || !readSparse(mV2, v2, cp[5], cn[5])) {
            err = "actions 某列解码失败";
            return false;
        }
        const size_t m = floors.size();
        if (types.size() != m || flags.size() != m) {
            err = "actions 各列长度不一致（文件损坏）";
            return false;
        }
        out.actions.resize(m);
        size_t ks = 0, k1 = 0, k2 = 0;
        for (size_t i = 0; i < m; ++i) {
            LevelData::FastAction& a = out.actions[i];
            const uint32_t t = types[i];
            const uint32_t bit = 1u << (t & 31u);
            a.floor = (int)floors[i];
            a.type = (LevelData::FastAction::Type)(uint8_t)t;
            a.strId = (mStr & bit) ? (uint16_t)strs[ks++] : 0;
            a.flag = (flags[i] != 0);
            const uint32_t x1 = (mV1 & bit) ? v1[k1++] : 0u;
            const uint32_t x2 = (mV2 & bit) ? v2[k2++] : 0u;
            std::memcpy(&a.val1, &x1, 4);
            std::memcpy(&a.val2, &x2, 4);
        }
        if (ks != strs.size() || k1 != v1.size() || k2 != v2.size()) {
            err = "actions 稀疏列的条数与掩码不符（文件损坏）";
            return false;
        }
    }

    // ⑤ pathData
    const Sec* pathSec = findSec(secs, SectionId::PathData);
    if (pathSec && pathSec->size) {
        if (!have(pathSec->size, 0, 8)) {
            err = "pathData 段太短";
            return false;
        }
        const uint64_t len = rdU64(pathSec->p);
        if (!have(pathSec->size, 8, (size_t)len)) {
            err = "pathData 越界";
            return false;
        }
        out.pathData.assign((const char*)pathSec->p + 8, (size_t)len);
    }

    // 未知段 id 一律跳过（前向兼容）—— 这就是"只读自己认识的段"的实现方式
    return true;
}

}  // namespace adocao
}  // namespace adofai
