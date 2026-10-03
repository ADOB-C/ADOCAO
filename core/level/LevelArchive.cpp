#include "LevelArchive.hpp"

#include <lzma.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr unsigned char kXzMagic[6]   = {0xFD, '7', 'z', 'X', 'Z', 0x00};
constexpr unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};

// 输出分块：1 MiB。解压后动辄上 GB（1.2 GB 谱面），一次一块、按需增长。
constexpr size_t kChunk = 1u << 20;

const char* lzmaReason(lzma_ret r) {
    switch (r) {
    case LZMA_MEM_ERROR:      return "out of memory";
    case LZMA_FORMAT_ERROR:   return "not an xz stream";
    case LZMA_OPTIONS_ERROR:  return "unsupported xz options";
    case LZMA_DATA_ERROR:     return "corrupt xz data";
    case LZMA_BUF_ERROR:      return "truncated xz data";
    case LZMA_UNSUPPORTED_CHECK: return "unsupported integrity check";
    case LZMA_MEMLIMIT_ERROR: return "xz memory limit hit";
    default:                  return "xz decode error";
    }
}

// 和 ../Song.adofai 的 xz.c 一样用多线程解码：它的谱面是多 block 压出来的，
// 单线程 lzma_stream_decoder 只有 ~0.5 GB/s，MT 能到 ~2.8 GB/s（实测 2.4 s -> 0.45 s）。
lzma_ret xzDecoderInit(lzma_stream* strm) {
#if LZMA_VERSION >= 50040000
    lzma_mt mt;
    std::memset(&mt, 0, sizeof mt);
    mt.flags = LZMA_CONCATENATED;
    mt.threads = lzma_cputhreads();
    if (mt.threads == 0) mt.threads = 1;
#if LZMA_VERSION >= 50060000
    // 线程缓冲上限。../Song.adofai 那边给的是 UINT64_MAX（只要最快），但那是 CLI，
    // 而 ADOCAO 解完还要在同一个进程里放下解析结构，峰值内存更值钱。同机 10 核、
    // 64 MiB 字典 / 19 blocks 实测（1.18 GB 输出）：不限 475 ms / 1.67 GB 峰值，
    // 1 GiB 上限 585 ms / 1.04 GB —— +110 ms 换掉 0.6 GB，值。
    mt.memlimit_threading = (uint64_t)1 << 30;
    mt.memlimit_stop = UINT64_MAX;
#else
    mt.memlimit = UINT64_MAX;
#endif
    return lzma_stream_decoder_mt(strm, &mt);
#else
    return lzma_stream_decoder(strm, UINT64_MAX, LZMA_CONCATENATED);
#endif
}

// 从流尾的 footer + index 反推出解压后的总大小（多流则逐流累加），算不出来返回 0。
// 有了它就能一次 reserve 到位：1.18 GB 的输出若靠 std::string 自己增长，新旧缓冲同时
// 存在会让峰值多出 ~2.4 GB（实测 1.67 GB -> 4.06 GB）。
uint64_t xzUncompressedSize(const char* data, size_t length) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(data);
    size_t pos = length;
    uint64_t total = 0;
    int streams = 0;
    while (pos >= 12) {
        const uint8_t* footer = base + pos - 12;
        lzma_stream_flags flags;
        // 解不出 footer 就说明后面没有更多流了（单流文件走完一圈后必然到这里），
        // 此时把已经累加到的值返回，而不是当成失败。
        if (lzma_stream_footer_decode(&flags, footer) != LZMA_OK) break;
        if (flags.backward_size > pos - 12) return 0;
        const uint8_t* indexPtr = footer - flags.backward_size;
        lzma_index* idx = nullptr;
        uint64_t memlimit = UINT64_MAX;
        size_t inPos = 0;
        lzma_ret r = lzma_index_buffer_decode(&idx, &memlimit, nullptr, indexPtr,
                                              &inPos, flags.backward_size);
        if (r != LZMA_OK) { if (idx) lzma_index_end(idx, nullptr); return 0; }
        total += lzma_index_uncompressed_size(idx);
        lzma_index_end(idx, nullptr);
        const size_t next = (size_t)(indexPtr - base);
        if (next >= pos) return 0;          // 没往前走，别死循环
        pos = next;
        if (++streams > 1024) return 0;
    }
    return total;   // 流数 >= 1 才有意义；0 表示这不是能识别的 xz 流
}

bool decodeXz(const char* data, size_t length, std::string& out, std::string& reason,
              const std::function<void(float)>& onProgress) {
    lzma_stream strm = LZMA_STREAM_INIT;
    // LZMA_CONCATENATED: 多个 xz 流首尾相接也要读完（xz 工具会这么切块）
    lzma_ret r = xzDecoderInit(&strm);
    if (r != LZMA_OK) { reason = lzmaReason(r); return false; }

    strm.next_in = reinterpret_cast<const uint8_t*>(data);
    strm.avail_in = length;
    out.clear();
    const uint64_t exact = xzUncompressedSize(data, length);
    try {
        // 多留一个 chunk：循环最后一次 resize 会按整块要，留够就不会再触发一次
        // 全长拷贝（那一下会让峰值凭空多出一个 1.18 GB）
        out.reserve(exact > 0 ? (size_t)exact + kChunk : length * 4);
    } catch (const std::bad_alloc&) {
        out.reserve(length * 4);            // 预留失败就退回按需增长
    }

    bool ok = false;
    for (;;) {
        const size_t base = out.size();
        out.resize(base + kChunk);
        strm.next_out = reinterpret_cast<uint8_t*>(&out[base]);
        strm.avail_out = kChunk;
        const size_t consumedBefore = strm.avail_in;

        r = lzma_code(&strm, LZMA_FINISH);
        out.resize(base + (kChunk - strm.avail_out));

        if (onProgress && length > 0)
            onProgress(1.0f - (float)strm.avail_in / (float)length);

        if (r == LZMA_STREAM_END) { ok = true; break; }
        if (r != LZMA_OK) { reason = lzmaReason(r); break; }
        // 既没结束、又没吃进输入也没产出 -> 提前收手，避免死循环
        if (strm.avail_in == consumedBefore && strm.avail_out == kChunk) {
            reason = "truncated xz data";
            break;
        }
    }
    lzma_end(&strm);
    if (ok && onProgress) onProgress(1.0f);
    return ok;
}

bool decodeZstd(const char* data, size_t length, std::string& out, std::string& reason,
                const std::function<void(float)>& onProgress) {
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) { reason = "out of memory"; return false; }
    size_t zr = ZSTD_initDStream(ds);
    if (ZSTD_isError(zr)) { reason = ZSTD_getErrorName(zr); ZSTD_freeDStream(ds); return false; }

    ZSTD_inBuffer in{data, length, 0};
    out.clear();
    const unsigned long long exact = ZSTD_getFrameContentSize(data, length);
    if (exact != ZSTD_CONTENTSIZE_UNKNOWN && exact != ZSTD_CONTENTSIZE_ERROR && exact > 0)
        out.reserve((size_t)exact);
    else
        out.reserve(length * 4);

    bool ok = false;
    size_t remaining = 1;   // 0 == 当前帧结束
    while (true) {
        const size_t base = out.size();
        out.resize(base + kChunk);
        ZSTD_outBuffer ob{&out[base], kChunk, 0};
        remaining = ZSTD_decompressStream(ds, &ob, &in);
        out.resize(base + ob.pos);

        if (onProgress && length > 0) onProgress((float)in.pos / (float)length);
        if (ZSTD_isError(remaining)) { reason = ZSTD_getErrorName(remaining); break; }
        if (in.pos >= in.size) {
            // 输入吃完：remaining == 0 表示正好收在一帧边界上（正常结束）
            ok = (remaining == 0);
            if (!ok) reason = "truncated zstd data";
            break;
        }
        if (ob.pos == 0 && remaining == 0) continue;   // 下一帧开始
    }
    ZSTD_freeDStream(ds);
    if (ok && onProgress) onProgress(1.0f);
    return ok;
}

}  // namespace

LevelArchiveKind sniffLevelArchive(const char* data, size_t length) {
    if (!data || length == 0) return LevelArchiveKind::Plain;
    if (length >= sizeof(kXzMagic) && std::memcmp(data, kXzMagic, sizeof(kXzMagic)) == 0)
        return LevelArchiveKind::Xz;
    if (length >= sizeof(kZstdMagic) && std::memcmp(data, kZstdMagic, sizeof(kZstdMagic)) == 0)
        return LevelArchiveKind::Zstd;
    return LevelArchiveKind::Plain;
}

bool decompressLevelArchive(const char* data, size_t length, LevelArchiveKind kind,
                            std::string& out, std::string& reason,
                            const std::function<void(float)>& onProgress) {
    switch (kind) {
    case LevelArchiveKind::Xz:   return decodeXz(data, length, out, reason, onProgress);
    case LevelArchiveKind::Zstd: return decodeZstd(data, length, out, reason, onProgress);
    case LevelArchiveKind::Plain:
        out.assign(data, length);
        return true;
    }
    reason = "unknown container";
    return false;
}
