#include "LevelArchive.hpp"

#include <lzma.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>

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
    mt.memlimit_threading = UINT64_MAX;
    mt.memlimit_stop = UINT64_MAX;
#else
    mt.memlimit = UINT64_MAX;
#endif
    return lzma_stream_decoder_mt(strm, &mt);
#else
    return lzma_stream_decoder(strm, UINT64_MAX, LZMA_CONCATENATED);
#endif
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
    out.reserve(length * 4);

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
