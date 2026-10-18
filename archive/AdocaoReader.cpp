#include "archive/AdocaoReader.hpp"

#include <cstring>
#include <vector>

#include "archive/AdocaoBlocked.hpp"
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
    uint8_t codec = 0;       // Codec（Raw / SectionZstd / SectionXz）
    uint16_t flags = 0;
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t elems0Raw = 0;    // 段表里的 rawSize（解压后的长度，用来核对）
    bool materialized = false;
};

// 容器的"段表视图"：头/表/CRC 一次校验完，**段载荷按需解压**。
// 为什么按需：③ 的窗口化加载一次只要"一个段里的一块"，不该顺手把整份谱的所有段都摊开
// （那也是 O(n) 的瞬时内存，正是 D1 要消掉的东西）。
struct Container {
    uint16_t version = 0;
    std::vector<Sec> secs;
    std::vector<std::string> plain;      // 段级解压的后备（decompressLevelArchive 的出参是 string）

    size_t indexOf(uint8_t id) const {
        for (size_t i = 0; i < secs.size(); ++i) if (secs[i].id == id) return i;
        return (size_t)-1;
    }
    bool materialize(size_t i, std::string& err) {
        if (i >= secs.size()) { err = "段下标越界"; return false; }
        Sec& s = secs[i];
        if (s.materialized) return true;
        if (s.codec == (uint8_t)Codec::Raw) { s.materialized = true; return true; }
        LevelArchiveKind kind;
        if (s.codec == (uint8_t)Codec::SectionZstd) kind = LevelArchiveKind::Zstd;
        else if (s.codec == (uint8_t)Codec::SectionXz) kind = LevelArchiveKind::Xz;
        else {
            err = "段 " + std::to_string(s.id) + " 的编码 " + std::to_string(s.codec) + " 不认识";
            return false;
        }
        std::string& dst = plain[i];
        std::string why;
        if (!decompressLevelArchive((const char*)s.p, s.size, kind, dst, why)) {
            err = "段 " + std::to_string(s.id) + " 解压失败: " + why;
            return false;
        }
        if (dst.size() != s.elems0Raw) {   // 解压出来的长度必须与段表里写的一致
            err = "段 " + std::to_string(s.id) + " 解压后长度与段表不符（文件损坏）";
            return false;
        }
        s.p = (const uint8_t*)dst.data();
        s.size = dst.size();
        s.materialized = true;
        return true;
    }
    bool materializeAll(std::string& err) {
        for (size_t i = 0; i < secs.size(); ++i)
            if (!materialize(i, err)) return false;
        return true;
    }
};

bool openContainer(const uint8_t* data, size_t length, Container& out, std::string& err) {
    out = Container{};
    if (!data || !have(length, 0, sizeof(Header))) {
        err = "文件太短（连文件头都不完整）";
        return false;
    }
    if (std::memcmp(data, kMagic, 4) != 0) {
        err = "magic 不符（不是 .adocao）";
        return false;
    }
    const uint16_t version = rdU16(data + 4);
    // 双读：v3 是当前的写入版本，v2 仍然照读（行为不变）。更早的版本从来没有实现过，
    // 未知版本**明确失败**（不猜、不退化）—— §8.9：version 是唯一的兼容闸门。
    if (version < kMinReadVersion || version > kVersion) {
        err = "版本不支持：文件是 v" + std::to_string(version) + "，本程序认 v" +
              std::to_string(kMinReadVersion) + "~v" + std::to_string(kVersion);
        return false;
    }
    out.version = version;
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
    out.secs.reserve(sectionCount);
    out.plain.assign(sectionCount, std::string());
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const uint8_t* e = data + sizeof(Header) + (size_t)i * sizeof(SectionEntry);
        Sec s;
        s.id = e[0];
        s.codec = e[1];
        s.flags = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
        const uint64_t off = rdU64(e + 4);
        const uint64_t size = rdU64(e + 12);
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
        s.materialized = (s.codec == (uint8_t)Codec::Raw);
        out.secs.push_back(s);
    }
    return true;
}

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
    // 2026-10-17 追加的字段（记录末尾）。**旧文件没有这两个字节** → 保留调用方给的默认值，
    // 不是坏数据（这也是不 bump 版本号的原因：两个方向都兼容）。
    if (off + 2 <= n) {
        if (!idx(s.songFilename)) return false;
    }
    // 再往后是运镜用的两个字段；旧文件没有 → 保留默认值（同 songFilename 的规则）。
    if (off + 4 <= n) {
        if (!f32(s.pitch)) return false;
    }
    if (off + 1 <= n) {
        if (!u8(b)) return false;
        s.legacyCamRelativeTo = (b != 0);
    }
    // 砖色（2026-10-17 追加在末尾）：旧文件读不到就保留默认值。
    if (off + 2 <= n) { if (!idx(s.trackStyle)) return false; }
    if (off + 2 <= n) { if (!idx(s.trackColorType)) return false; }
    if (off + 2 <= n) { if (!idx(s.trackColorPulse)) return false; }
    if (off + 4 <= n) { if (!f32(s.trackColorAnimDuration)) return false; }
    if (off + 4 <= n) { if (!f32(s.trackPulseLength)) return false; }
    if (off + 4 <= n) { if (!f32(s.trackGlowIntensity)) return false; }
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

bool getSectionView(const uint8_t* data, size_t length, uint8_t sectionId,
                    SectionView& out, std::string& err) {
    err.clear();
    out = SectionView{};
    Container c;
    if (!openContainer(data, length, c, err)) return false;
    const size_t idx = c.indexOf(sectionId);
    if (idx == (size_t)-1) { err = "没有段 " + std::to_string(sectionId); return false; }
    if (!c.materialize(idx, err)) return false;
    const Sec& s = c.secs[idx];
    out.flags = s.flags;
    out.size = s.size;
    if (s.codec == (uint8_t)Codec::Raw) {
        out.p = s.p;                       // 直接指向文件字节（调用方必须让 data 活着）
    } else {
        out.storage = std::move(c.plain[idx]);
        out.p = (const uint8_t*)out.storage.data();
    }
    return true;
}

bool unpackLevel(const uint8_t* data, size_t length, LevelData& out, std::string& err,
                 DerivedInfo* derived) {
    err.clear();
    Container c;
    if (!openContainer(data, length, c, err)) return false;
    if (!c.materializeAll(err)) return false;
    const uint16_t version = c.version;
    const std::vector<Sec>& secs = c.secs;

    // 列容器在 v3 换了布局（分块）。这三个小包装把"v2 裸列 / v3 容器"两套列解码收在一处：
    // legacy = 段表 flags 置了 kFlagLegacyColumns（写入方在"分块没赚到"时逐字节落回 v2）。
    auto decD = [&](const uint8_t* p, size_t n, std::vector<double>& o, bool legacy) {
        return (version >= 3 && !legacy) ? decodeColumnV3(p, n, o)
                                         : decodeDoubleColumn(p, n, o);
    };
    auto decI = [&](const uint8_t* p, size_t n, std::vector<int64_t>& o, bool legacy) {
        return (version >= 3 && !legacy) ? decodeColumnV3(p, n, o)
                                         : decodeIntColumn(p, n, o);
    };
    auto decU = [&](const uint8_t* p, size_t n, std::vector<uint32_t>& o, bool legacy) {
        return (version >= 3 && !legacy) ? decodeColumnV3(p, n, o)
                                         : decodeU32Column(p, n, o);
    };

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
    const bool angLegacy = angSec && (angSec->flags & kFlagLegacyColumns) != 0;
    if (angSec && angSec->size) {
        if (!decD(angSec->p, angSec->size, out.angleData, angLegacy)) {
            err = "angleData 列解码失败";
            return false;
        }
    }

    // ④ actions（列式：floor / type / strId / flag / val1 / val2）
    const Sec* actSec = findSec(secs, SectionId::Actions);
    const bool actLegacy = actSec && (actSec->flags & kFlagLegacyColumns) != 0;
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
        auto readSparse = [&](uint32_t& mask, std::vector<uint32_t>& vals,
                              const uint8_t* c, size_t csz) {
            if (!have(csz, 0, 12)) return false;
            mask = rdU32(c);
            const uint64_t used = rdU64(c + 4);
            if (!decU(c + 12, csz - 12, vals, actLegacy)) return false;
            return vals.size() == used;
        };
        std::vector<uint32_t> strs, v1, v2;
        uint32_t mStr = 0, mV1 = 0, mV2 = 0;
        if (!decI(cp[0], cn[0], floors, actLegacy) || !decU(cp[1], cn[1], types, actLegacy) ||
            !readSparse(mStr, strs, cp[2], cn[2]) || !decU(cp[3], cn[3], flags, actLegacy) ||
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
    // ⑥ 相机 / 重复事件（可选段：没有 = 这张谱没有运镜，空 vector 就是正确结果）
    if (const Sec* camSec = findSec(secs, SectionId::Camera)) {
        constexpr size_t kRec = 38;   // floor(4) id(4) angleOffset/duration/x/y/rotation/zoom(24)
                                      // ease/relativeTo/flags/disabled(4) tag(2)
        if (camSec->size % kRec != 0) { err = "camera 段长度不是记录整数倍"; return false; }
        const size_t cnt = camSec->size / kRec;
        out.cameraEvents.clear();
        out.cameraEvents.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* q = camSec->p + i * kRec;
            LevelData::CameraEvent e;
            e.floor = (int32_t)rdU32(q);
            e.id = rdU32(q + 4);
            std::memcpy(&e.angleOffset, q + 8, 4);
            std::memcpy(&e.duration, q + 12, 4);
            std::memcpy(&e.x, q + 16, 4);
            std::memcpy(&e.y, q + 20, 4);
            std::memcpy(&e.rotation, q + 24, 4);
            std::memcpy(&e.zoom, q + 28, 4);
            e.ease = q[32];
            e.relativeTo = q[33];
            e.flags = q[34];
            e.disabled = q[35];
            const uint16_t ti = rdU16(q + 36);
            if (ti >= out.actionStrTable.size()) { err = "camera 段 tag 下标越界"; return false; }
            e.tag = (ti == 0) ? std::string() : out.actionStrTable[ti];
            out.cameraEvents.push_back(std::move(e));
        }
    }
    if (const Sec* repSec = findSec(secs, SectionId::Repeat)) {
        constexpr size_t kRec = 19;   // floor(4) repetitions/interval/gapLength(12) flags(1) tag(2)
        if (repSec->size % kRec != 0) { err = "repeat 段长度不是记录整数倍"; return false; }
        const size_t cnt = repSec->size / kRec;
        out.repeatEvents.clear();
        out.repeatEvents.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* q = repSec->p + i * kRec;
            LevelData::RepeatEvent r;
            r.floor = (int32_t)rdU32(q);
            std::memcpy(&r.repetitions, q + 4, 4);
            std::memcpy(&r.interval, q + 8, 4);
            std::memcpy(&r.gapLength, q + 12, 4);
            r.flags = q[16];
            const uint16_t ti = rdU16(q + 17);
            if (ti >= out.actionStrTable.size()) { err = "repeat 段 tag 下标越界"; return false; }
            r.tag = (ti == 0) ? std::string() : out.actionStrTable[ti];
            out.repeatEvents.push_back(std::move(r));
        }
    }
    if (const Sec* mvSec = findSec(secs, SectionId::MoveTrack)) {
        const size_t kRec = (version >= 5) ? 54 : 52;   // v5 追加 eventTag(u16)
        if (mvSec->size % kRec != 0) { err = "movetrack 段长度不是记录整数倍"; return false; }
        const size_t cnt = mvSec->size / kRec;
        out.moveTrackEvents.clear();
        out.moveTrackEvents.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* q = mvSec->p + i * kRec;
            LevelData::MoveTrackEvent e;
            e.floor = (int32_t)rdU32(q);
            std::memcpy(&e.angleOffset, q + 4, 4);
            std::memcpy(&e.durBeats, q + 8, 4);
            e.gap = (int32_t)rdU32(q + 12);
            e.startOff = (int32_t)rdU32(q + 16);
            e.endOff = (int32_t)rdU32(q + 20);
            e.startRel = (int8_t)q[24];
            e.endRel = (int8_t)q[25];
            e.ease = q[26];
            e.flags = q[27];
            std::memcpy(&e.offX, q + 28, 4);
            std::memcpy(&e.offY, q + 32, 4);
            std::memcpy(&e.rotDeg, q + 36, 4);
            std::memcpy(&e.scaleX, q + 40, 4);
            std::memcpy(&e.scaleY, q + 44, 4);
            std::memcpy(&e.opacity, q + 48, 4);
            if (version >= 5) {
                const uint16_t ti = (uint16_t)(q[52] | (q[53] << 8));
                if (ti >= out.actionStrTable.size()) { err = "movetrack 段 tag 下标越界"; return false; }
                if (ti != 0) e.tag = out.actionStrTable[ti];
            }
            out.moveTrackEvents.push_back(e);
        }
    }
    if (const Sec* flSec = findSec(secs, SectionId::Flash)) {
        constexpr size_t kRec = 32;
        if (flSec->size % kRec != 0) { err = "flash 段长度不是记录整数倍"; return false; }
        const size_t cnt = flSec->size / kRec;
        out.flashEvents.clear();
        out.flashEvents.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* q = flSec->p + i * kRec;
            LevelData::FlashEvent e;
            e.floor = (int32_t)rdU32(q);
            std::memcpy(&e.duration, q + 4, 4);
            std::memcpy(&e.angleOffset, q + 8, 4);
            e.plane = q[12]; e.style = q[13]; e.ease = q[14];
            e.startColor[0] = q[16]; e.startColor[1] = q[17]; e.startColor[2] = q[18];
            e.endColor[0] = q[20];   e.endColor[1] = q[21];   e.endColor[2] = q[22];
            std::memcpy(&e.startOpacity, q + 24, 4);
            std::memcpy(&e.endOpacity, q + 28, 4);
            out.flashEvents.push_back(e);
        }
    }
    if (const Sec* colSec = findSec(secs, SectionId::Colour)) {
        // v3 = 32 B（floor + 5 池下标 + 4 float + ease/flags）
        // v4 = 50 B（再追加 startOff/endOff/startRel/endRel/gapLength/angleOffset）
        const size_t kRec = (version >= 4) ? 50 : 32;
        if (colSec->size % kRec != 0) { err = "colour 段长度不是记录整数倍"; return false; }
        const size_t cnt = colSec->size / kRec;
        out.colorEvents.clear();
        out.colorEvents.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            const uint8_t* q = colSec->p + i * kRec;
            LevelData::ColorEvent e;
            e.floor = (int32_t)rdU32(q);
            const uint16_t it = rdU16(q + 4), ic = rdU16(q + 6), is = rdU16(q + 8),
                           ip = rdU16(q + 10), ist = rdU16(q + 12);
            if (it >= out.actionStrTable.size() || ic >= out.actionStrTable.size() ||
                is >= out.actionStrTable.size() || ip >= out.actionStrTable.size() ||
                ist >= out.actionStrTable.size()) { err = "colour 段池下标越界"; return false; }
            e.cfg.colorType = (it == 0) ? std::string("Single") : out.actionStrTable[it];
            e.cfg.color = (ic == 0) ? std::string("debb7b") : out.actionStrTable[ic];
            e.cfg.secondary = (is == 0) ? std::string("ffffff") : out.actionStrTable[is];
            e.cfg.pulse = (ip == 0) ? std::string("None") : out.actionStrTable[ip];
            e.cfg.style = (ist == 0) ? std::string("Standard") : out.actionStrTable[ist];
            std::memcpy(&e.cfg.animDuration, q + 14, 4);
            std::memcpy(&e.cfg.pulseLength, q + 18, 4);
            std::memcpy(&e.cfg.glowIntensity, q + 22, 4);
            std::memcpy(&e.duration, q + 26, 4);
            e.ease = q[30];
            e.flags = q[31];
            if (version >= 4) {
                e.startOff = (int32_t)rdU32(q + 32);
                e.endOff = (int32_t)rdU32(q + 36);
                e.startRel = (int8_t)q[40];
                e.endRel = (int8_t)q[41];
                e.gapLength = (int32_t)rdU32(q + 42);
                std::memcpy(&e.angleOffset, q + 46, 4);
            }
            e.cfg.startFloor = e.floor;
            e.cfg.startTime = 0.0;
            e.order = (uint32_t)i;
            out.colorEvents.push_back(std::move(e));
        }
    }

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

    // ⑥ Derived（可选）：checkpoint 表。坏数据**明确失败**（它是 ③ 的寻址依据，不许半信半疑）
    if (derived) {
        derived->stride = 0;
        derived->checkpoints.clear();
        derived->actionStride = 0;
        derived->firstAction.clear();
        const Sec* aiSec = findSec(secs, SectionId::ActionIndex);
        if (aiSec && aiSec->size) {
            if (!decodeActionIndex(aiSec->p, aiSec->size, derived->actionStride,
                                   derived->firstAction)) {
                err = "ActionIndex 段的按层索引解析失败（文件损坏）";
                return false;
            }
        }
        const Sec* dSec = findSec(secs, SectionId::Derived);
        if (dSec && dSec->size) {
            if (!decodeCheckpoints(dSec->p, dSec->size, derived->stride, derived->checkpoints)) {
                err = "Derived 段的 checkpoint 表解析失败（文件损坏）";
                return false;
            }
        }
    }

    // 未知段 id 一律跳过（前向兼容）—— 这就是"只读自己认识的段"的实现方式
    return true;
}

}  // namespace adocao
}  // namespace adofai
