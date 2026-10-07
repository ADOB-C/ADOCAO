#include "LevelArchive.hpp"
#include "LzmaMtCompat.hpp"

#include <lzma.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <new>


namespace adofai {

namespace {

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
    archive_detail::setMtMemlimit(mt);   // 字段名跨版本改过（见 LzmaMtCompat.hpp）
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

// ---------------------------------------------------------------- 流式解压（乒乓半窗）
// 三方状态（lzma_stream / ZSTD_DStream / 半窗缓冲）全部藏在这里：公共头因此零三方依赖，
// 只链 adocao_archive 的消费者不必配 lzma/zstd 的头文件路径（ADOCAO 的 app 就是这种）。
struct ArchiveStream::Impl {
    std::vector<char> store;
    char* buf[2] = {nullptr, nullptr};
    size_t half = 0, len = 0, carry = 0, cur = 0;
    bool eof = false, failed = false, stuck = false, started = false;
    bool finalDelivered = false;   // 只含 carry 的尾块是否已经交付过
    std::string error;
    const char* in = nullptr;
    size_t inLen = 0;
    LevelArchiveKind kind = LevelArchiveKind::Plain;
    lzma_stream strm = LZMA_STREAM_INIT;
    ZSTD_DStream* ds = nullptr;
    ZSTD_inBuffer zin{nullptr, 0, 0};
    bool frameDone = false;
};

ArchiveStream::ArchiveStream() : m_impl(new Impl()) {}

ArchiveStream::~ArchiveStream() { release(); }

void ArchiveStream::release() {
    if (m_impl->kind == LevelArchiveKind::Xz && m_impl->started) lzma_end(&m_impl->strm);
    if (m_impl->ds) { ZSTD_freeDStream(m_impl->ds); m_impl->ds = nullptr; }
    m_impl->started = false;
}

bool ArchiveStream::open(const char* data, size_t length, LevelArchiveKind kind, size_t halfSize) {
    release();
    if (kind == LevelArchiveKind::Plain || halfSize < 4096) {
        m_impl->error = "streaming requires a compressed container and halfSize >= 4 KiB";
        m_impl->failed = true;
        return false;
    }
    m_impl->kind = kind;
    m_impl->half = halfSize;
    m_impl->store.assign(m_impl->half * 2, 0);          // 一次分配，两块半窗地址固定
    m_impl->buf[0] = m_impl->store.data();
    m_impl->buf[1] = m_impl->store.data() + m_impl->half;
    m_impl->in = data;
    m_impl->inLen = length;
    m_impl->len = m_impl->carry = m_impl->cur = 0;
    m_impl->eof = m_impl->failed = m_impl->stuck = m_impl->started = false;
    m_impl->finalDelivered = false;

    if (kind == LevelArchiveKind::Xz) {
        lzma_ret r = xzDecoderInit(&m_impl->strm);
        if (r != LZMA_OK) { m_impl->error = lzmaReason(r); m_impl->failed = true; return false; }
        m_impl->strm.next_in = reinterpret_cast<const uint8_t*>(m_impl->in);
        m_impl->strm.avail_in = m_impl->inLen;
    } else {
        m_impl->ds = ZSTD_createDStream();
        if (!m_impl->ds) { m_impl->error = "out of memory"; m_impl->failed = true; return false; }
        size_t zr = ZSTD_initDStream(m_impl->ds);
        if (ZSTD_isError(zr)) { m_impl->error = ZSTD_getErrorName(zr); m_impl->failed = true; return false; }
        m_impl->zin = ZSTD_inBuffer{m_impl->in, m_impl->inLen, 0};
        m_impl->frameDone = false;
    }
    m_impl->started = true;
    return true;
}

bool ArchiveStream::pump(size_t carry) {
    const size_t space = m_impl->half - carry;
    if (space == 0) return false;
    char* dst = m_impl->buf[m_impl->cur] + carry;

    if (m_impl->kind == LevelArchiveKind::Xz) {
        m_impl->strm.next_out = reinterpret_cast<uint8_t*>(dst);
        m_impl->strm.avail_out = space;
        for (;;) {
            const size_t outBefore = m_impl->strm.avail_out;
            const size_t inBefore = m_impl->strm.avail_in;
            lzma_ret r = lzma_code(&m_impl->strm, LZMA_FINISH);
            if (r == LZMA_STREAM_END) { m_impl->eof = true; break; }
            if (r != LZMA_OK) { m_impl->error = lzmaReason(r); m_impl->failed = true; return false; }
            if (m_impl->strm.avail_out == 0) break;                 // 半窗满，剩下的下次再解
            // 输入吃完又没产出：流被截断了，别再空转
            if (m_impl->strm.avail_in == inBefore && m_impl->strm.avail_out == outBefore) {
                m_impl->error = "truncated xz data"; m_impl->failed = true; return false;
            }
        }
        m_impl->len = carry + (space - m_impl->strm.avail_out);
        return true;
    }

    ZSTD_outBuffer ob{dst, space, 0};
    for (;;) {
        // 上一帧已经收尾、输入也吃完了：不要再喂（再喂会去读下一帧的帧头，被误判截断）
        if (m_impl->frameDone && m_impl->zin.pos >= m_impl->zin.size) { m_impl->eof = true; break; }
        const size_t remaining = ZSTD_decompressStream(m_impl->ds, &ob, &m_impl->zin);
        if (ZSTD_isError(remaining)) { m_impl->error = ZSTD_getErrorName(remaining); m_impl->failed = true; return false; }
        m_impl->frameDone = (remaining == 0);                      // 0 = 当前帧结束（后面可能还有帧）
        if (ob.pos == ob.size) break;                        // 半窗满，剩下的下次再解
        if (m_impl->zin.pos >= m_impl->zin.size) {                       // 输入吃完
            if (!m_impl->frameDone) { m_impl->error = "truncated zstd data"; m_impl->failed = true; return false; }
            m_impl->eof = true;
            break;
        }
    }
    m_impl->len = carry + ob.pos;
    return true;
}

bool ArchiveStream::next() {
    if (m_impl->failed) return false;
    if (m_impl->failed || (m_impl->eof && m_impl->carry == 0)) return false;

    const size_t carry = m_impl->carry;
    // 残缺值本身就把半窗占满了：消费者一点都吃不进去，再给也没用
    if (carry >= m_impl->half) {
        if (!m_impl->eof) m_impl->stuck = true;
        return false;
    }
    const size_t prev = m_impl->cur;
    m_impl->cur = 1 - m_impl->cur;                      // 换到另一半
    if (carry > 0) {
        // 上一块末尾那 carry 个字节是残缺值，拷到新块开头，再接着解压
        if (carry > m_impl->len) { m_impl->error = "carry beyond window"; m_impl->failed = true; return false; }
        std::memmove(m_impl->buf[m_impl->cur], m_impl->buf[prev] + (m_impl->len - carry), carry);
    }
    m_impl->carry = 0;
    if (!pump(carry)) return false;
    // 一个新字节都没解出来：消费者上次连一个完整值都凑不齐 -> 单个值比半窗还大
    // （流刚好结束时也算：同样的字节再给一遍也还是吃不下，必须让调用方退回整份解压）
    if (m_impl->len == carry) {
        // 一个新字节都没解出来，而消费者上次连一个完整值都没凑齐。
        if (m_impl->eof) {
            // 流已经结束：这一块只含 carry。必须**交付一次**，否则这几个字节就永远丢了
            // （典型场景：帧结束时半窗正好填满，eof 要等下一次调用才发现）。
            if (m_impl->finalDelivered) return false;
            m_impl->finalDelivered = true;
            return true;
        }
        // 流还没结束 = 单个值比半窗还大 -> stuck，调用方退回整份解压
        m_impl->stuck = true;
        return false;
    }
    return true;
}

void ArchiveStream::consume(size_t completeBytes) {
    if (completeBytes > m_impl->len) completeBytes = m_impl->len;
    m_impl->carry = m_impl->len - completeBytes;
}

// ---------------- core 的后端钩子实现（依赖倒置的"实现"这一侧）----------------

// 访问器都在这里定义（Impl 在头里是不完整类型，没法内联）
const char* ArchiveStream::data() const { return m_impl->buf[m_impl->cur]; }
size_t ArchiveStream::size() const { return m_impl->len; }
bool ArchiveStream::failed() const { return m_impl->failed; }
bool ArchiveStream::stuck() const { return m_impl->stuck; }
bool ArchiveStream::eof() const { return m_impl->eof; }
size_t ArchiveStream::halfSize() const { return m_impl->half; }
const std::string& ArchiveStream::error() const { return m_impl->error; }

namespace archive {

namespace {

bool decodeWholeImpl(const char* data, size_t length, LevelArchiveKind kind,
                     std::string& out, std::string& reason,
                     const std::function<void(float)>& onProgress) {
    return decompressLevelArchive(data, length, kind, out, reason, onProgress);
}

WindowSource* makeWindowImpl() { return new ArchiveStream(); }

}  // namespace

ArchiveBackend backend() {
    ArchiveBackend b;
    b.decodeWhole = &decodeWholeImpl;
    b.makeWindow = &makeWindowImpl;
    return b;
}

void install() { setArchiveBackend(backend()); }

}  // namespace archive

}  // namespace adofai
