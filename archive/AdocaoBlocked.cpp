#include "archive/AdocaoBlocked.hpp"

#include <cstdlib>
#include <cstring>
#include <limits>

#include "archive/AdocaoColumns.hpp"
#include "core/util/Logger.hpp"

#include <zstd.h>   // 帧级压缩（archive 的 PRIVATE 依赖；这个文件属于 adocao_archive，用得上）

namespace adofai {
namespace adocao {
namespace {

using Bytes = std::vector<uint8_t>;

void putU8(Bytes& b, uint8_t v) { b.push_back(v); }
void putU32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putU64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putBytes(Bytes& b, const void* p, size_t n) {
    const uint8_t* q = (const uint8_t*)p;
    b.insert(b.end(), q, q + n);
}

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

// zstd-19 一帧；压不小就返回 false（调用方存原样）。
bool compressFrame(const Bytes& raw, Bytes& out) {
    out.clear();
    if (raw.size() < 64) return false;
    Bytes z(ZSTD_compressBound(raw.size()));
    const size_t got = ZSTD_compress(z.data(), z.size(), raw.data(), raw.size(), 19);
    if (ZSTD_isError(got) || got >= raw.size()) return false;
    z.resize(got);
    out = std::move(z);
    return true;
}

bool decompressFrame(const uint8_t* p, size_t n, size_t rawSize, Bytes& out) {
    out.assign(rawSize, 0);
    if (rawSize == 0) return n == 0;
    const size_t got = ZSTD_decompress(out.data(), rawSize, p, n);
    if (ZSTD_isError(got) || got != rawSize) return false;
    return true;
}

// 三种列类型各自转发到 AdocaoColumns 的编码器/解码器（那里已有"编完解回来逐位比"的自检）
bool encodeBlob(ColKind, const std::vector<double>& v, Bytes& o, ColumnStats* s) {
    return encodeDoubleColumn(v, o, s);
}
bool encodeBlob(ColKind, const std::vector<int64_t>& v, Bytes& o, ColumnStats* s) {
    return encodeIntColumn(v, o, s);
}
bool encodeBlob(ColKind, const std::vector<uint32_t>& v, Bytes& o, ColumnStats* s) {
    return encodeU32Column(v, o, s);
}
bool decodeBlob(ColKind, const uint8_t* p, size_t n, std::vector<double>& o) {
    return decodeDoubleColumn(p, n, o);
}
bool decodeBlob(ColKind, const uint8_t* p, size_t n, std::vector<int64_t>& o) {
    return decodeIntColumn(p, n, o);
}
bool decodeBlob(ColKind, const uint8_t* p, size_t n, std::vector<uint32_t>& o) {
    return decodeU32Column(p, n, o);
}

// 单块布局的容器头（12 B）+ 单块头（16 B）
void writeContainerHeader(Bytes& b, ColKind kind, uint8_t layout, uint64_t total) {
    putU8(b, (uint8_t)kind);
    putU8(b, layout);
    b.push_back(0); b.push_back(0);          // reserved u16
    putU64(b, total);
}

struct FramePlan {
    uint64_t firstBlock = 0;
    uint64_t blockCount = 0;
    uint64_t frameOffset = 0;   // 相对 frames 区起点
    Bytes    stored;            // 实际写出的字节（zstd 或原样）
    uint64_t rawSize = 0;
    uint8_t  codec = 0;
};

// ---- 分块布局的编码：切块 → 每块独立编码 → 聚合成帧 ----
template <typename T>
bool buildBlocked(const std::vector<T>& v, ColKind kind, Bytes& out, ColumnLayoutStats* st) {
    const size_t n = v.size();
    const size_t nBlocks = (n + kBlockElems - 1) / kBlockElems;

    // ① 每块独立编码（自带列头/字典/首值；这是"任意块可单独解码"的根据）
    std::vector<std::pair<uint64_t, Bytes>> blocks;   // (firstElement, blob)
    blocks.reserve(nBlocks);
    for (size_t bi = 0; bi < nBlocks; ++bi) {
        const size_t from = bi * kBlockElems;
        const size_t to = (from + kBlockElems < n) ? from + kBlockElems : n;
        std::vector<T> slice(v.begin() + from, v.begin() + to);
        Bytes blob;
        ColumnStats cs;
        if (!encodeBlob(kind, slice, blob, &cs)) return false;
        blocks.emplace_back((uint64_t)from, std::move(blob));
    }

    // ② 聚合成帧：解压后目标 ~256 KB / 最多 256 块（§8.3：帧大小与 seek 粒度解耦）
    std::vector<FramePlan> frames;
    size_t codecBytes = 0;
    for (const auto& b : blocks) codecBytes += b.second.size();
    size_t bi = 0;
    while (bi < blocks.size()) {
        Bytes raw;
        FramePlan fp;
        fp.firstBlock = bi;          // **块号**（不是元素号）：locateBlock/自洽校验都按块号二分的
        while (bi < blocks.size() && fp.blockCount < kMaxBlocksPerFrame) {
            const Bytes& blob = blocks[bi].second;
            if (fp.blockCount > 0 && raw.size() + blob.size() + 4 > kFrameTargetRaw) break;
            putU32(raw, (uint32_t)blob.size());
            putBytes(raw, blob.data(), blob.size());
            ++fp.blockCount;
            ++bi;
            if (raw.size() >= kFrameTargetRaw) break;
        }
        fp.rawSize = raw.size();
        Bytes z;
        if (compressFrame(raw, z)) { fp.stored = std::move(z); fp.codec = (uint8_t)Codec::SectionZstd; }
        else { fp.stored = std::move(raw); fp.codec = (uint8_t)Codec::Raw; }
        frames.push_back(std::move(fp));
    }

    // ③ 组装容器：头 + 帧表 + 帧区（每帧 8 B 对齐）
    const size_t tableBytes = frames.size() * kFrameEntry;
    const size_t framesBase = kContainerHeader + 16 + tableBytes;
    out.clear();
    writeContainerHeader(out, kind, kLayoutBlocked, n);
    putU32(out, kBlockElems);
    putU32(out, 0);
    putU64(out, frames.size());

    Bytes frameArea;
    for (FramePlan& fp : frames) {
        while (frameArea.size() % 8 != 0) frameArea.push_back(0);
        fp.frameOffset = frameArea.size();
        putBytes(frameArea, fp.stored.data(), fp.stored.size());
    }
    for (const FramePlan& fp : frames) {
        putU64(out, fp.firstBlock);
        putU64(out, fp.blockCount);
        putU64(out, fp.frameOffset);
        putU64(out, fp.stored.size());
        putU64(out, fp.rawSize);
        putU32(out, fp.codec);
        putU32(out, 0);
    }
    putBytes(out, frameArea.data(), frameArea.size());

    if (st) {
        st->blocked = true;
        st->blocks = nBlocks;
        st->frames = frames.size();
        st->blockedBytes = out.size();
        st->blockedCodecBytes = codecBytes;
    }
    return out.size() == framesBase + frameArea.size();
}

}  // namespace

// ---------------- 公开 API ----------------

namespace {

size_t g_mustBlockBytes = kMustBlockBytes;   // 见 AdocaoBlocked.hpp：强制分块的阈值

// 单块布局的容器：12 B 容器头 + 20 B 单块头 + 载荷
void writeSingle(Bytes& dst, ColKind kind, uint64_t total, const Bytes& blob, bool compress) {
    Bytes stored;
    uint8_t codec = (uint8_t)Codec::Raw;
    if (compress) {
        if (compressFrame(blob, stored)) codec = (uint8_t)Codec::SectionZstd;
        else stored = blob;
    } else {
        stored = blob;
    }
    dst.clear();
    writeContainerHeader(dst, kind, kLayoutSingle, total);
    putU8(dst, codec);
    dst.push_back(0); dst.push_back(0); dst.push_back(0);
    putU64(dst, blob.size());
    putU64(dst, stored.size());
    putBytes(dst, stored.data(), stored.size());
}

template <typename T>
bool encodeTyped(ColKind kind, const std::vector<T>& v, ColumnStorage storage,
                 Bytes& out, ColumnLayoutStats* st) {
    const size_t n = v.size();
    ColumnLayoutStats s;

    // 列 blob：AdocaoColumns 的编码器**自带**"编完解回来逐位比"的自检
    Bytes blob; ColumnStats cs;
    if (!encodeBlob(kind, v, blob, &cs)) return false;
    s.singleCodecBytes = blob.size();

    // ① LegacyV2：**裸列字节**（连容器头都不加），交给段级 zstd —— 与 v2 文件逐字节等价
    if (storage == ColumnStorage::LegacyV2) {
        out = blob;
        s.blocked = false;
        s.singleBytes = out.size();
        if (st) *st = s;
        return true;
    }

    // ② SingleZstd：载荷自己压一帧（段级不再压）
    Bytes zs;
    writeSingle(zs, kind, n, blob, /*compress=*/true);
    s.singleBytes = zs.size();

    // ③ Blocked：每块独立编码 + 帧级 zstd（列太短时 1 块 == 单块，纯亏头部，不做）
    Bytes blocked;
    const bool tried = (n > kBlockElems) && buildBlocked<T>(v, kind, blocked, &s);
    if (!tried) { s.blockedBytes = 0; s.blocks = 0; s.frames = 0; }

    // 强制分块：裸数据过阈值（且单块不是"什么都没有可切"的常量级）→ 分块是要求，不是优化。
    // 测试钩子 ADOCAO_FORCE_BLOCK_KB=<n>：把阈值改成 n KB，好在**真实规模**上造出分块容器
    // （真谱按体积本来会选单块）—— 窗口化(③)的验收必须跑在那个布局上。
    size_t mustBlock = g_mustBlockBytes;
    if (const char* env = std::getenv("ADOCAO_FORCE_BLOCK_KB")) {
        const long kb = std::atol(env);
        if (kb > 0) mustBlock = (size_t)kb * 1024;
    }
    const bool forced = tried && (storage == ColumnStorage::BestSelf) &&
                        (n * sizeof(T) >= mustBlock) && zs.size() > 64;
    const bool useBlocked = forced || ((storage == ColumnStorage::BestSelf) && tried &&
                                       s.blockedBytes < zs.size());
    out = useBlocked ? std::move(blocked) : zs;
    s.blocked = useBlocked;
    s.forced = forced;

    // 容器级自检：整列解回来必须**逐位**相同，否则退回"单块+自压"
    // （列编码器已经自检过一次，这里查的是**容器框架**本身没把字节搬错）
    std::vector<T> back;
    bool ok = decodeColumnV3(out.data(), out.size(), back) && back.size() == n &&
              (n == 0 || std::memcmp(back.data(), v.data(), n * sizeof(T)) == 0);
    if (!ok) {
        // **不许静默**：容器级自检失败说明"分块布局"这条路径自己坏了（不是数据的问题），
        // 退回去只是兜底，必须留下痕迹 —— 否则表现就是"文件小一点、寻址悄悄没了"。
        LOG_E("adocao 分块列容器自检失败（kind=%d, n=%zu）—— 退回单块布局", (int)kind, n);
        out = std::move(zs);
        s.blocked = false;
        std::vector<T> back2;
        if (!decodeColumnV3(out.data(), out.size(), back2) || back2.size() != n ||
            (n != 0 && std::memcmp(back2.data(), v.data(), n * sizeof(T)) != 0)) {
            return false;       // 连单块都回不来：内部一致性已经破了，别写出坏文件
        }
    }
    if (st) *st = s;
    return true;
}

}  // namespace

void setMustBlockBytesForTest(size_t n) { g_mustBlockBytes = n; }

bool encodeColumnV3(ColKind kind, const void* data, size_t count, ColumnStorage storage,
                    std::vector<uint8_t>& out, ColumnLayoutStats* st) {
    out.clear();
    static const uint8_t kEmpty[8] = {0};
    if (!data) {
        if (count) return false;
        data = kEmpty;                 // count == 0：别造 nullptr 的指针算术
    }
    switch (kind) {
    case ColKind::Double:
        return encodeTyped<double>(kind, std::vector<double>((const double*)data, (const double*)data + count),
                                   storage, out, st);
    case ColKind::Int64:
        return encodeTyped<int64_t>(kind, std::vector<int64_t>((const int64_t*)data, (const int64_t*)data + count),
                                    storage, out, st);
    case ColKind::U32:
        return encodeTyped<uint32_t>(kind, std::vector<uint32_t>((const uint32_t*)data, (const uint32_t*)data + count),
                                     storage, out, st);
    default:
        return false;
    }
}

uint64_t ColumnView::frameFirstBlock(uint64_t f) const {
    if (f >= frameCount) return 0;
    return rdU64(frameTable + f * kFrameEntry);
}
uint64_t ColumnView::frameBlockCount(uint64_t f) const {
    if (f >= frameCount) return 0;
    return rdU64(frameTable + f * kFrameEntry + 8);
}

bool ColumnView::locateBlock(uint64_t blockIndex, uint64_t& frame, uint64_t& blockInFrame) const {
    if (frameCount == 0) return false;
    // 帧表按 firstBlock 升序且不重叠 → 二分
    uint64_t lo = 0, hi = frameCount;
    while (lo + 1 < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (frameFirstBlock(mid) <= blockIndex) lo = mid; else hi = mid;
    }
    if (frameFirstBlock(lo) > blockIndex) return false;
    const uint64_t inFrame = blockIndex - frameFirstBlock(lo);
    if (inFrame >= frameBlockCount(lo)) return false;
    frame = lo;
    blockInFrame = inFrame;
    return true;
}

bool openColumnV3(const uint8_t* p, size_t n, ColumnView& out) {
    out = ColumnView{};
    if (!p || !have(n, 0, kContainerHeader)) return false;
    const uint8_t kindB = p[0];
    if (kindB > (uint8_t)ColKind::U32) return false;
    const uint8_t layout = p[1];
    out.kind = (ColKind)kindB;
    out.container = p;
    out.containerSize = n;
    out.total = rdU64(p + 4);

    if (layout == kLayoutSingle) {
        if (!have(n, kContainerHeader, kSingleHeader)) return false;
        const uint8_t codec = p[kContainerHeader];
        if (codec != (uint8_t)Codec::Raw && codec != (uint8_t)Codec::SectionZstd) return false;
        const uint64_t rawSize = rdU64(p + kContainerHeader + 4);
        const uint64_t storedSize = rdU64(p + kContainerHeader + 12);
        if (!have(n, kContainerHeader + kSingleHeader, (size_t)storedSize)) return false;
        out.blocked = false;
        out.blockElems = kBlockElems;
        (void)rawSize;
        return true;
    }
    if (layout != kLayoutBlocked) return false;
    if (!have(n, kContainerHeader, 16)) return false;
    const uint32_t blockElems = rdU32(p + kContainerHeader);
    const uint64_t frameCount = rdU64(p + kContainerHeader + 8);
    if (blockElems == 0) return false;
    const size_t tableBytes = (size_t)frameCount * kFrameEntry;
    const size_t tableOff = kContainerHeader + 16;
    if (!have(n, tableOff, tableBytes)) return false;
    out.blocked = true;
    out.blockElems = blockElems;
    out.frameCount = frameCount;
    out.frameTable = p + tableOff;
    out.frames = p + tableOff + tableBytes;
    out.framesSize = n - (tableOff + tableBytes);
    // 帧表必须自洽：firstBlock 升序且连续、块数非 0、字节范围与 rawSize 都在合理范围内
    uint64_t expectFirst = 0;
    for (uint64_t f = 0; f < frameCount; ++f) {
        const uint8_t* e = out.frameTable + f * kFrameEntry;
        const uint64_t first = rdU64(e);
        const uint64_t cnt = rdU64(e + 8);
        const uint64_t off = rdU64(e + 16);
        const uint64_t compSize = rdU64(e + 24);
        const uint64_t rawSize = rdU64(e + 32);
        const uint32_t codec = rdU32(e + 40);
        if (cnt == 0 || first != expectFirst) return false;
        if (codec != (uint8_t)Codec::Raw && codec != (uint8_t)Codec::SectionZstd) return false;
        // rawSize 是攻击面可控的分配量：限死（正常帧 ≤ kFrameTargetRaw + 一个块）
        if (rawSize > (4u << 20)) return false;
        expectFirst = first + cnt;
        if (!have(out.framesSize, (size_t)off, (size_t)compSize)) return false;
        if (rawSize == 0 && compSize == 0) return false;
    }
    // 帧表必须正好盖住全部块（块数 = ceil(total / blockElems)）
    const uint64_t expectBlocks = (out.total + blockElems - 1) / blockElems;
    if (expectFirst != expectBlocks) return false;
    return true;
}

namespace {

bool framePayload(const ColumnView& v, uint64_t frame, Bytes& raw) {
    raw.clear();
    if (frame >= v.frameCount) return false;
    const uint8_t* e = v.frameTable + frame * kFrameEntry;
    const uint64_t off = rdU64(e + 16);
    const uint64_t compSize = rdU64(e + 24);
    const uint64_t rawSize = rdU64(e + 32);
    const uint32_t codec = rdU32(e + 40);
    if (!have(v.framesSize, (size_t)off, (size_t)compSize)) return false;
    if (codec == (uint8_t)Codec::Raw) {
        if (compSize != rawSize) return false;
        raw.assign(v.frames + off, v.frames + off + compSize);
        return true;
    }
    return decompressFrame(v.frames + off, (size_t)compSize, (size_t)rawSize, raw);
}

// 在一帧的解压结果里走到第 blockInFrame 块，返回它那块 blob 的区间
bool seekBlockInFrame(const Bytes& raw, uint64_t blockInFrame, const uint8_t** blob, size_t* blobN) {
    size_t off = 0;
    for (uint64_t i = 0;; ++i) {
        if (!have(raw.size(), off, 4)) return false;
        const uint32_t len = rdU32(raw.data() + off);
        off += 4;
        if (!have(raw.size(), off, len)) return false;
        if (i == blockInFrame) {
            *blob = raw.data() + off;
            *blobN = len;
            return true;
        }
        off += len;
    }
}

template <typename T>
bool decodeTyped(const uint8_t* p, size_t n, ColKind expect, std::vector<T>& out) {
    ColumnView v;
    if (!openColumnV3(p, n, v)) return false;
    if (v.kind != expect) return false;                      // 用错类型解释 = 坏数据
    out.clear();

    if (!v.blocked) {
        const uint8_t* s = p + kContainerHeader;
        const uint8_t codec = s[0];
        const uint64_t rawSize = rdU64(s + 4);
        const uint64_t storedSize = rdU64(s + 12);
        if (!have(n, kContainerHeader + kSingleHeader, (size_t)storedSize)) return false;
        Bytes raw;
        if (codec == (uint8_t)Codec::Raw) {
            if (storedSize != rawSize) return false;
            raw.assign(p + kContainerHeader + kSingleHeader,
                       p + kContainerHeader + kSingleHeader + storedSize);
        } else if (!decompressFrame(p + kContainerHeader + kSingleHeader, (size_t)storedSize,
                                    (size_t)rawSize, raw)) {
            return false;
        }
        return decodeBlob(expect, raw.data(), raw.size(), out) && out.size() == (size_t)v.total;
    }

    out.reserve((size_t)v.total);
    uint64_t expectBlock = 0;
    for (uint64_t f = 0; f < v.frameCount; ++f) {
        Bytes raw;
        if (!framePayload(v, f, raw)) return false;
        if (v.frameFirstBlock(f) != expectBlock) return false;
        const uint64_t cnt = v.frameBlockCount(f);
        size_t off = 0;
        for (uint64_t i = 0; i < cnt; ++i) {
            if (!have(raw.size(), off, 4)) return false;
            const uint32_t len = rdU32(raw.data() + off);
            off += 4;
            if (!have(raw.size(), off, len)) return false;
            std::vector<T> block;
            if (!decodeBlob(expect, raw.data() + off, len, block)) return false;
            if (block.empty() && v.total != 0) return false;
            out.insert(out.end(), block.begin(), block.end());
            if (out.size() > (size_t)v.total) return false;
            off += len;
            ++expectBlock;
        }
    }
    return out.size() == (size_t)v.total;
}

}  // namespace

bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<double>& out) {
    return decodeTyped<double>(p, n, ColKind::Double, out);
}
bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<int64_t>& out) {
    return decodeTyped<int64_t>(p, n, ColKind::Int64, out);
}
bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<uint32_t>& out) {
    return decodeTyped<uint32_t>(p, n, ColKind::U32, out);
}

namespace {

template <typename T>
bool decodeBlockTyped(const ColumnView& v, uint64_t blockIndex, ColKind expect, std::vector<T>& out) {
    out.clear();
    if (v.kind != expect) return false;
    if (!v.blocked) {
        if (blockIndex != 0) return false;
        return decodeColumnV3(v.container, v.containerSize, out);
    }
    uint64_t frame = 0, inFrame = 0;
    if (!v.locateBlock(blockIndex, frame, inFrame)) return false;
    Bytes raw;
    if (!framePayload(v, frame, raw)) return false;
    const uint8_t* blob = nullptr;
    size_t blobN = 0;
    if (!seekBlockInFrame(raw, inFrame, &blob, &blobN)) return false;
    return decodeBlob(expect, blob, blobN, out);
}

}  // namespace

bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<double>& out) {
    return decodeBlockTyped<double>(v, blockIndex, ColKind::Double, out);
}
bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<int64_t>& out) {
    return decodeBlockTyped<int64_t>(v, blockIndex, ColKind::Int64, out);
}
bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<uint32_t>& out) {
    return decodeBlockTyped<uint32_t>(v, blockIndex, ColKind::U32, out);
}

}  // namespace adocao
}  // namespace adofai
