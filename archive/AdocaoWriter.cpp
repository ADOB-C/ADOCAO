#include "archive/AdocaoWriter.hpp"

#include <cstring>

#include "archive/AdocaoColumns.hpp"
#include "core/level/LevelData.hpp"

#include <zstd.h>   // 段级压缩（archive 的 PRIVATE 依赖；实测真实文件上 zstd 比 xz 更小）

namespace adofai {
namespace adocao {
namespace {

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
void putBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* q = (const uint8_t*)p;
    b.insert(b.end(), q, q + n);
}
void putF32(std::vector<uint8_t>& b, float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, 4);          // 位模式原样：float 列必须逐位无损
    putU32(b, x);
}

// 载荷列按类型稀疏：只写"该类型真的用到这一列"的事件，其余读侧取默认 0。
// 判据是**自描述的**：某类型在这一列出现过非默认值 → 该类型进掩码（不靠列名/语义硬编码）。
uint32_t sparseMask(const std::vector<uint32_t>& types, const std::vector<uint32_t>& col) {
    uint32_t mask = 0;
    for (size_t i = 0; i < types.size(); ++i)
        if (col[i] != 0u) mask |= (1u << (types[i] & 31u));
    return mask;
}

// 内容指纹：FNV-1a-64（算法写进 docs/adocao-format.md §2.1，别的实现可逐字节复现）。
// 覆盖**未压缩**的各段内容 + 段 id，所以与压缩器、与构建都无关（可跨实现比对）。
uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// 构建期注入的 git commit（40 位十六进制）→ 20 原始字节；没有/不合法就写全 0。
// 读取方**不看**这个字段，它只作溯源 —— 绝不能当兼容闸门。
void writerCommitBytes(uint8_t out[20]) {
    std::memset(out, 0, 20);
#ifdef ADOCAO_GIT_COMMIT
    const char* s = ADOCAO_GIT_COMMIT;
    if (std::strlen(s) != 40) return;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 20; ++i) {
        const int hi = hex(s[i * 2]);
        const int lo = hex(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) { std::memset(out, 0, 20); return; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
#endif
}

struct Section {
    uint8_t id = 0;
    uint64_t elements = 0;
    std::vector<uint8_t> blob;
};

// 字符串池：v[0] 必须是空串（strId = 0 的约定）。actionStrTable 的**原序**必须保留
// （strId 就是它的下标），settings 用到的字符串按固定顺序追加到末尾。
struct Pool {
    std::vector<std::string> v;
    uint16_t add(const std::string& s) {
        if (s.empty()) return 0;
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] == s) return (uint16_t)i;
        v.push_back(s);
        return (uint16_t)(v.size() - 1);
    }
};

// settings 的定长记录（字段顺序固定 → 打包确定性）。字符串走池下标；未知键与今天的行为一致
// （两条解析路径本来就只读已知字段），原样保留原始字节的 `preserved` 段留给后续版本。
void writeSettings(const LevelData::Settings& s, Pool& pool, std::vector<uint8_t>& b) {
    putU32(b, (uint32_t)s.version);
    putF32(b, s.bpm);
    putF32(b, s.offset);
    putU32(b, (uint32_t)s.countdownTicks);
    putF32(b, s.zoom);
    putF32(b, s.rotation);
    putU16(b, pool.add(s.relativeTo));
    putF32(b, s.position[0]);
    putF32(b, s.position[1]);
    putU16(b, pool.add(s.hitsound));
    putF32(b, s.hitsoundVolume);
    putU16(b, pool.add(s.trackColor));
    putU16(b, pool.add(s.secondaryTrackColor));
    putU16(b, pool.add(s.backgroundColor));
    putU8(b, s.stickToFloors ? 1 : 0);
    putU16(b, pool.add(s.planetEase));
    putU16(b, pool.add(s.trackDisappearAnimation));
    putU16(b, pool.add(s.trackAnimation));
    putF32(b, s.beatsBehind);
    putF32(b, s.beatsAhead);
}

}  // namespace

bool packLevel(const LevelData& level, std::vector<uint8_t>& out, std::string& err,
               PackResult* result) {
    out.clear();
    err.clear();
    PackResult rep;

    // ---- ① settings（先做：它会往字符串池里追加 settings 的字符串）----
    Pool pool;
    pool.v = level.actionStrTable;
    if (pool.v.empty()) pool.v.emplace_back();
    std::vector<uint8_t> settingsBlob;
    writeSettings(level.settings, pool, settingsBlob);

    // ---- ② 字符串池（原序保留 actionStrTable，末尾是 settings 追加的）----
    std::vector<uint8_t> poolBlob;
    putU32(poolBlob, (uint32_t)pool.v.size());
    for (const std::string& s : pool.v) {
        putU32(poolBlob, (uint32_t)s.size());
        putBytes(poolBlob, s.data(), s.size());
    }

    // ---- ③ angleData：一列 double（字典是主编码）----
    std::vector<uint8_t> angleBlob;
    {
        ColumnStats st;
        encodeDoubleColumn(level.angleData, angleBlob, &st);
        rep.columns.push_back({"angleData", st.codec, level.angleData.size(), st.rawBytes,
                               st.encodedBytes, st.dictEntries, st.bits});
    }

    // ---- ④ actions：列式（floor 差分、type/strId/flag 位打包、val1/val2 按 float 位模式）----
    std::vector<uint8_t> actionsBlob;
    {
        const size_t n = level.actions.size();
        std::vector<int64_t> floors;
        std::vector<uint32_t> types, strIds, flags, v1, v2;
        floors.reserve(n); types.reserve(n); strIds.reserve(n); flags.reserve(n); v1.reserve(n); v2.reserve(n);
        for (const LevelData::FastAction& a : level.actions) {
            floors.push_back((int64_t)a.floor);
            types.push_back((uint32_t)a.type);
            strIds.push_back((uint32_t)a.strId);
            flags.push_back(a.flag ? 1u : 0u);
            uint32_t x = 0;
            std::memcpy(&x, &a.val1, 4); v1.push_back(x);
            std::memcpy(&x, &a.val2, 4); v2.push_back(x);
        }
        putU32(actionsBlob, 6);                       // 列数
        auto pushColumn = [&](const char* name, const std::vector<uint8_t>& enc, const ColumnStats& st,
                              size_t elements, size_t rawBytes) {
            putU64(actionsBlob, enc.size());           // u64：允许单列 > 4 GB
            putBytes(actionsBlob, enc.data(), enc.size());
            rep.columns.push_back({name, st.codec, elements, rawBytes, enc.size(), st.dictEntries, st.bits});
        };
        auto addI = [&](const char* name, const std::vector<int64_t>& col) {
            std::vector<uint8_t> enc; ColumnStats st;
            encodeIntColumn(col, enc, &st);
            pushColumn(name, enc, st, col.size(), col.size() * 8);
        };
        auto addU = [&](const char* name, const std::vector<uint32_t>& col) {
            std::vector<uint8_t> enc; ColumnStats st;
            encodeU32Column(col, enc, &st);
            pushColumn(name, enc, st, col.size(), col.size() * 4);
        };
        // 稀疏列：u32 掩码 + u64 实际条数 + 一列（只含掩码内的类型）
        auto addSparseU = [&](const char* name, const std::vector<uint32_t>& col) {
            const uint32_t mask = sparseMask(types, col);
            std::vector<uint32_t> used;
            used.reserve(col.size() / 4 + 16);
            for (size_t i = 0; i < col.size(); ++i)
                if (mask & (1u << (types[i] & 31u))) used.push_back(col[i]);
            std::vector<uint8_t> enc; ColumnStats st;
            encodeU32Column(used, enc, &st);
            std::vector<uint8_t> blob;
            putU32(blob, mask);
            putU64(blob, used.size());
            putBytes(blob, enc.data(), enc.size());
            putU64(actionsBlob, blob.size());
            putBytes(actionsBlob, blob.data(), blob.size());
            rep.columns.push_back({name, st.codec, used.size(), col.size() * 4, blob.size(),
                                   st.dictEntries, st.bits});
        };
        addI("actions.floor", floors);
        addU("actions.type", types);
        addSparseU("actions.strId", strIds);
        addU("actions.flag", flags);
        addSparseU("actions.val1", v1);
        addSparseU("actions.val2", v2);
        (void)n;
    }

    // ---- 组装段 ----
    std::vector<Section> secs;
    {
        Section s; s.id = (uint8_t)SectionId::Settings; s.blob = std::move(settingsBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::StringPool; s.elements = pool.v.size();
        s.blob = std::move(poolBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::AngleData; s.elements = level.angleData.size();
        s.blob = std::move(angleBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::Actions; s.elements = level.actions.size();
        s.blob = std::move(actionsBlob); secs.push_back(std::move(s));
    }
    if (!level.pathData.empty()) {
        Section s; s.id = (uint8_t)SectionId::PathData;
        putU64(s.blob, level.pathData.size());
        putBytes(s.blob, level.pathData.data(), level.pathData.size());
        secs.push_back(std::move(s));
    }

    // ---- 段目录（偏移 8 B 对齐，为 mmap 直读）----
    const uint64_t tableBytes = sizeof(Header) + (uint64_t)secs.size() * sizeof(SectionEntry);
    uint64_t off = (tableBytes + 7u) & ~7ull;
    std::vector<SectionEntry> table;
    table.reserve(secs.size());
    // 段级压缩：zstd 19（实测真实文件上比 xz 更小、解码更快、且 archive 本来就链着它）。
    // 太小的段不值得压；压不小就存原样。crc32c 覆盖**实际存储的字节**（读侧先校验再解压）。
    std::vector<std::vector<uint8_t>> stored(secs.size());
    for (size_t i = 0; i < secs.size(); ++i) {
        const std::vector<uint8_t>& blob = secs[i].blob;
        stored[i] = blob;
        if (blob.size() >= 256) {
            std::vector<uint8_t> z(ZSTD_compressBound(blob.size()));
            const size_t got = ZSTD_compress(z.data(), z.size(), blob.data(), blob.size(), 19);
            if (!ZSTD_isError(got) && got < blob.size()) {
                z.resize(got);
                stored[i] = std::move(z);
            }
        }
    }
    for (size_t i = 0; i < secs.size(); ++i) {
        const Section& s = secs[i];
        const std::vector<uint8_t>& st = stored[i];
        SectionEntry e{};
        e.id = s.id;
        e.codec = (st.size() < s.blob.size()) ? (uint8_t)Codec::SectionZstd : (uint8_t)Codec::Raw;
        e.flags = 0;
        e.offset = off;
        e.compSize = st.size();
        e.rawSize = s.blob.size();
        e.elemCount = s.elements;
        e.crc32c = crc32c(st.data(), st.size());
        off = (off + st.size() + 7u) & ~7ull;
        table.push_back(e);
    }
    const uint64_t fileSize = off;

    // ---- 头 + 表 ----
    std::vector<uint8_t> hb;
    putBytes(hb, kMagic, 4);
    putU16(hb, kVersion);
    putU16(hb, 0);                                  // flags
    putU16(hb, (uint16_t)secs.size());
    putU16(hb, 0);                                  // reserved
    putU64(hb, fileSize);
    putU32(hb, 0);                                  // headerCrc（待回填）
    {
        uint8_t commit[20];
        writerCommitBytes(commit);
        putBytes(hb, commit, 20);
    }
    {
        uint64_t h = 1469598103934665603ull;        // FNV-1a-64 offset basis
        for (const Section& s : secs) {
            h = fnv1a64(&s.id, 1, h);
            h = fnv1a64(s.blob.data(), s.blob.size(), h);
        }
        putU64(hb, h);                              // 前 8 字节 = 内容指纹
        for (int i = 0; i < 24; ++i) hb.push_back(0);
    }
    for (const SectionEntry& e : table) {
        putU8(hb, e.id);
        putU8(hb, e.codec);
        putU16(hb, e.flags);
        putU64(hb, e.offset);
        putU64(hb, e.compSize);
        putU64(hb, e.rawSize);
        putU64(hb, e.elemCount);
        putU32(hb, e.crc32c);
    }
    // headerCrc 覆盖"头（本字段按 0 参与）+ 整张段表"：段表小，索性一起校验，读侧无从漏检
    const uint32_t hc = crc32c(hb.data(), hb.size());
    std::memcpy(hb.data() + 20, &hc, 4);

    out.assign((size_t)fileSize, 0);
    std::memcpy(out.data(), hb.data(), hb.size());
    for (size_t i = 0; i < secs.size(); ++i)
        if (!stored[i].empty())
            std::memcpy(out.data() + table[i].offset, stored[i].data(), stored[i].size());

    if (hb.size() != (size_t)tableBytes) {
        err = "内部错误：头 + 段表的长度与预期不符";
        return false;
    }
    rep.fileBytes = fileSize;
    if (result) *result = std::move(rep);
    return true;
}

}  // namespace adocao
}  // namespace adofai
