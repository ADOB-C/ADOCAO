#include "core/map/TiffWriter.hpp"

#include "core/util/Logger.hpp"
#include "core/map/PngDeflate.hpp"

#include <cstring>

namespace adofai {

namespace {
constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong  = 4;
constexpr uint16_t kTypeLong8 = 16;
}  // namespace

void TiffWriter::put16(uint16_t v) {
    const uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
    writeRaw(b, 2);
}
void TiffWriter::put32(uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                          (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
    writeRaw(b, 4);
}
void TiffWriter::put64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    writeRaw(b, 8);
}

bool TiffWriter::writeRaw(const void* p, size_t n) {
    if (!m_f || m_failed) return false;
    if (std::fwrite(p, 1, n, m_f) != n) { m_failed = true; return false; }
    m_pos += n;
    return true;
}

bool TiffWriter::open(const std::string& path, uint32_t width, uint32_t height,
                      uint32_t rowsPerStrip, uint16_t compression, Compressor compressor,
                      bool bigTiff) {
    close();
    if (width == 0 || height == 0) return false;
    m_w = width;
    m_h = height;
    m_rowsPerStrip = rowsPerStrip ? rowsPerStrip : 256;
    if (m_rowsPerStrip > height) m_rowsPerStrip = height;
    m_compression = compression;
    m_compressor = std::move(compressor);
    m_big = bigTiff;
    m_strips = (height + m_rowsPerStrip - 1) / m_rowsPerStrip;
    m_rowsWritten = 0;
    m_offsets.assign(m_strips, 0);
    m_bytes.assign(m_strips, 0);
    m_pos = 0;
    m_failed = false;
    m_finished = false;
    m_f = std::fopen(path.c_str(), "wb");
    if (!m_f) return false;
    if (m_big) {
        put16(0x4949); put16(43); put16(8); put16(0); put64(0);   // BigTIFF header
    } else {
        put16(0x4949); put16(42); put32(0);                        // 经典 TIFF header
    }
    return !m_failed;
}

bool TiffWriter::writeStrip(const uint8_t* rgba, uint32_t rows) {
    if (!m_f || m_failed) return false;
    if (m_strips == 0 || m_rowsWritten >= m_h) return false;
    if (rows == 0 || rows > m_rowsPerStrip || m_rowsWritten + rows > m_h) return false;
    const size_t bytes = (size_t)m_w * rows * 4;
    m_tmp.clear();
    bool ok = false;
    if (m_compressor) ok = m_compressor(rgba, bytes, m_tmp);
    if (!ok) m_tmp.assign(rgba, rgba + bytes);
    const uint32_t idx = m_rowsWritten / m_rowsPerStrip;
    if (idx >= m_strips) return false;
    m_offsets[idx] = m_pos;
    m_bytes[idx] = (uint64_t)m_tmp.size();
    m_rowsWritten += rows;
    return writeRaw(m_tmp.data(), m_tmp.size());
}

bool TiffWriter::finish() {
    if (!m_f) return false;
    if (m_finished) return !m_failed;
    m_finished = true;
    if (m_rowsWritten != m_h) {
        LOG_W("TIFF: 只写了 %u / %u 行（末尾补白）", m_rowsWritten, m_h);
    }
    // 末尾段之外没写的条带：给一个空的 deflate 段（有效但全黑）
    for (uint32_t i = 0; i < m_strips; ++i) {
        if (m_offsets[i] == 0 && m_bytes[i] == 0) {
            m_offsets[i] = m_pos;
            std::vector<uint8_t> empty((size_t)m_w * m_rowsPerStrip * 4, 0);
            if (m_compressor) m_compressor(empty.data(), empty.size(), m_tmp);
            else m_tmp = std::move(empty);
            m_bytes[i] = (uint64_t)m_tmp.size();
            if (!writeRaw(m_tmp.data(), m_tmp.size())) return false;
        }
    }

    if (m_big) {
        const uint64_t offsetsArray = m_pos;
        if (!writeRaw(m_offsets.data(), m_offsets.size() * 8)) return false;
        const uint64_t bytesArray = m_pos;
        if (!writeRaw(m_bytes.data(), m_bytes.size() * 8)) return false;
        const uint64_t ifdPos = m_pos;
        struct E { uint16_t tag, type; uint64_t count, value; };
        const std::vector<E> entries = {
            {256, kTypeLong, 1, m_w}, {257, kTypeLong, 1, m_h},
            {258, kTypeShort, 4, 0x0008000800080008ull},
            {259, kTypeShort, 1, m_compression},
            {262, kTypeShort, 1, 2},
            {273, kTypeLong8, m_strips, offsetsArray},
            {277, kTypeShort, 1, 4},
            {278, kTypeLong, 1, m_rowsPerStrip},
            {279, kTypeLong8, m_strips, bytesArray},
            {284, kTypeShort, 1, 1},
            {338, kTypeShort, 1, 1},
            {339, kTypeShort, 4, 0x0001000100010001ull},
        };
        uint8_t cnt[8];
        for (int i = 0; i < 8; ++i) cnt[i] = (uint8_t)((uint64_t)entries.size() >> (8 * i));
        if (!writeRaw(cnt, 8)) return false;
        for (const E& en : entries) {
            uint8_t buf[20] = {0};
            auto w16 = [&](int off, uint16_t v) { buf[off] = (uint8_t)(v & 0xFF); buf[off + 1] = (uint8_t)(v >> 8); };
            auto w64 = [&](int off, uint64_t v) { for (int i = 0; i < 8; ++i) buf[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF); };
            w16(0, en.tag); w16(2, en.type); w64(4, en.count); w64(12, en.value);
            if (!writeRaw(buf, 20)) return false;
        }
        put64(0);
        const uint64_t endPos = m_pos;
        if (std::fseek(m_f, 8, SEEK_SET) != 0) { m_failed = true; return false; }
        uint8_t off8[8];
        for (int i = 0; i < 8; ++i) off8[i] = (uint8_t)(ifdPos >> (8 * i));
        if (std::fwrite(off8, 1, 8, m_f) != 8) { m_failed = true; return false; }
        std::fseek(m_f, (long)endPos, SEEK_SET);
    } else {
        std::vector<uint32_t> offs(m_strips), sizes(m_strips);
        for (uint32_t i = 0; i < m_strips; ++i) {
            if (m_offsets[i] > 0xFFFFFFFFull) {
                LOG_E("经典 TIFF: 偏移超过 4 GB，需要 BigTIFF");
                m_failed = true;
                return false;
            }
            offs[i] = (uint32_t)m_offsets[i];
            sizes[i] = (uint32_t)m_bytes[i];
        }
        const uint64_t bitsArray = m_pos;
        const uint16_t bits[4] = {8, 8, 8, 8};
        if (!writeRaw(bits, 8)) return false;
        const uint64_t fmtArray = m_pos;
        const uint16_t fmt[4] = {1, 1, 1, 1};
        if (!writeRaw(fmt, 8)) return false;
        const uint64_t offsetsArray = m_pos;
        if (!writeRaw(offs.data(), offs.size() * 4)) return false;
        const uint64_t bytesArray = m_pos;
        if (!writeRaw(sizes.data(), sizes.size() * 4)) return false;
        const uint64_t ifdPos = m_pos;
        struct E { uint16_t tag, type; uint32_t count, value; };
        const std::vector<E> entries = {
            {256, kTypeLong, 1, m_w}, {257, kTypeLong, 1, m_h},
            {258, kTypeShort, 4, (uint32_t)bitsArray},       // 4×SHORT = 8 B → 必须外置
            {259, kTypeShort, 1, m_compression},
            {262, kTypeShort, 1, 2},
            {273, kTypeLong, m_strips, (uint32_t)offsetsArray},
            {277, kTypeShort, 1, 4},
            {278, kTypeLong, 1, m_rowsPerStrip},
            {279, kTypeLong, m_strips, (uint32_t)bytesArray},
            {284, kTypeShort, 1, 1},
            {338, kTypeShort, 1, 1},
            {339, kTypeShort, 4, (uint32_t)fmtArray},
        };
        put16((uint16_t)entries.size());
        for (const E& en : entries) {
            uint8_t buf[12] = {0};
            auto w16 = [&](int off, uint16_t v) { buf[off] = (uint8_t)(v & 0xFF); buf[off + 1] = (uint8_t)(v >> 8); };
            auto w32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) buf[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF); };
            w16(0, en.tag); w16(2, en.type); w32(4, en.count); w32(8, en.value);
            if (!writeRaw(buf, 12)) return false;
        }
        put32(0);
        const uint64_t endPos = m_pos;
        if (std::fseek(m_f, 4, SEEK_SET) != 0) { m_failed = true; return false; }
        uint8_t off4[4];
        for (int i = 0; i < 4; ++i) off4[i] = (uint8_t)(ifdPos >> (8 * i));
        if (std::fwrite(off4, 1, 4, m_f) != 4) { m_failed = true; return false; }
        std::fseek(m_f, (long)endPos, SEEK_SET);
    }
    std::fflush(m_f);
    return !m_failed;
}

bool TiffWriter::close() {
    if (m_f) {
        if (!m_finished) finish();
        std::fclose(m_f);
        m_f = nullptr;
    }
    return !m_failed;
}

bool deflateCompress(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    mz_stream s{};
    if (mz_deflateInit(&s, 6) != MZ_OK) return false;
    // **输出缓冲要留够**：不可压的数据 deflate 后会略大于输入（zlib 建议 deflateBound）。
    // 之前按 n/2 给，装不下就返回 false → 上层退回"原始段"，而 Compression 标签仍写着
    // Deflate(8) → 文件结构上合法、像素全错（真踩过：第一条带 12,800 B 原样存了进去）。
    out.resize(n + n / 8 + 512);
    s.next_in = const_cast<Bytef*>(data);
    s.avail_in = (uInt)n;
    s.next_out = out.data();
    s.avail_out = (uInt)out.size();
    int rc = mz_deflate(&s, MZ_FINISH);
    while (rc == MZ_OK && s.avail_out == 0) {
        const size_t used = s.total_out;
        out.resize(out.size() * 2);
        s.next_out = out.data() + used;
        s.avail_out = (uInt)(out.size() - used);
        rc = mz_deflate(&s, MZ_FINISH);
    }
    if (rc != MZ_STREAM_END) { mz_deflateEnd(&s); return false; }
    out.resize(s.total_out);
    mz_deflateEnd(&s);
    return true;
}

}  // namespace adofai
