#include "core/map/TiffWriter.hpp"

#include "core/map/PngDeflate.hpp"
#include "core/util/Logger.hpp"

#include <algorithm>
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

bool TiffWriter::open(const std::string& path, uint32_t width, uint32_t height, Layout layout,
                      uint32_t tileW, uint32_t tileH, uint16_t compression, Compressor compressor,
                      bool bigTiff) {
    close();
    if (width == 0 || height == 0) return false;
    m_w = width;
    m_h = height;
    m_layout = layout;
    if (layout == Layout::Tiles) {
        m_blockW = (tileW + 15u) & ~15u;   // TIFF 规范：TileWidth/TileLength 必须是 16 的倍数
        m_blockH = (tileH + 15u) & ~15u;
        if (m_blockW == 0) m_blockW = 256;
        if (m_blockH == 0) m_blockH = 256;
    } else {
        m_blockW = width;                  // 条带：整宽
        m_blockH = tileH ? tileH : 256;
        if (m_blockH > height) m_blockH = height;
    }
    m_blocksX = (width + m_blockW - 1) / m_blockW;
    m_blocksY = (height + m_blockH - 1) / m_blockH;
    m_compression = compression;
    m_compressor = std::move(compressor);
    m_big = bigTiff;
    m_nextBlock = 0;
    m_offsets.assign((size_t)m_blocksX * m_blocksY, 0);
    m_bytes.assign((size_t)m_blocksX * m_blocksY, 0);
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

bool TiffWriter::writeBlock(const uint8_t* rgba) {
    if (!m_f || m_failed) return false;
    if (m_nextBlock >= m_offsets.size()) return false;
    const size_t expect = (size_t)m_blockW * m_blockH * 4;
    m_tmp.clear();
    bool ok = false;
    if (m_compressor) ok = m_compressor(rgba, expect, m_tmp);
    if (!ok) m_tmp.assign(rgba, rgba + expect);   // 压缩器说不行 → 存原始块
    m_offsets[m_nextBlock] = m_pos;
    m_bytes[m_nextBlock] = (uint64_t)m_tmp.size();
    ++m_nextBlock;
    return writeRaw(m_tmp.data(), m_tmp.size());
}

bool TiffWriter::finish() {
    if (!m_f) return false;
    if (m_finished) return !m_failed;
    m_finished = true;
    const uint32_t blockCount = m_blocksX * m_blocksY;
    if (m_nextBlock != blockCount)
        LOG_W("TIFF: 只写了 %zu / %u 块（缺失的块写 0）", m_nextBlock, blockCount);
    for (size_t i = m_nextBlock; i < blockCount; ++i) {   // 没写的块补 0
        m_offsets[i] = m_pos;
        std::vector<uint8_t> zero((size_t)m_blockW * m_blockH * 4, 0);
        if (m_compressor) m_compressor(zero.data(), zero.size(), m_tmp);
        else m_tmp = std::move(zero);
        m_bytes[i] = (uint64_t)m_tmp.size();
        if (!writeRaw(m_tmp.data(), m_tmp.size())) return false;
    }

    struct E { uint16_t tag, type; uint64_t count, value; };
    std::vector<E> entries;
    auto add = [&](uint16_t tag, uint16_t type, uint64_t count, uint64_t value) {
        entries.push_back({tag, type, count, value});
    };

    if (m_big) {
        const uint64_t offsetsArray = m_pos;
        if (!writeRaw(m_offsets.data(), m_offsets.size() * 8)) return false;
        const uint64_t bytesArray = m_pos;
        if (!writeRaw(m_bytes.data(), m_bytes.size() * 8)) return false;
        const uint64_t ifdPos = m_pos;
        add(256, kTypeLong, 1, m_w);
        add(257, kTypeLong, 1, m_h);
        add(258, kTypeShort, 4, 0x0008000800080008ull);
        add(259, kTypeShort, 1, m_compression);
        add(262, kTypeShort, 1, 2);
        if (m_layout == Layout::Tiles) {
            add(322, kTypeLong, 1, m_blockW);
            add(323, kTypeLong, 1, m_blockH);
            add(324, kTypeLong8, blockCount, offsetsArray);
            add(325, kTypeLong8, blockCount, bytesArray);
        } else {
            add(273, kTypeLong8, blockCount, offsetsArray);
            add(278, kTypeLong, 1, m_blockH);
            add(279, kTypeLong8, blockCount, bytesArray);
        }
        add(277, kTypeShort, 1, 4);
        add(284, kTypeShort, 1, 1);
        add(338, kTypeShort, 1, 1);
        add(339, kTypeShort, 4, 0x0001000100010001ull);
        std::sort(entries.begin(), entries.end(), [](const E& a, const E& b) { return a.tag < b.tag; });
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
        std::vector<uint32_t> offs(blockCount), sizes(blockCount);
        for (uint32_t i = 0; i < blockCount; ++i) {
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
        add(256, kTypeLong, 1, m_w);
        add(257, kTypeLong, 1, m_h);
        add(258, kTypeShort, 4, bitsArray);
        add(259, kTypeShort, 1, m_compression);
        add(262, kTypeShort, 1, 2);
        if (m_layout == Layout::Tiles) {
            add(322, kTypeLong, 1, m_blockW);
            add(323, kTypeLong, 1, m_blockH);
            add(324, kTypeLong, blockCount, offsetsArray);
            add(325, kTypeLong, blockCount, bytesArray);
        } else {
            add(273, kTypeLong, blockCount, offsetsArray);
            add(278, kTypeLong, 1, m_blockH);
            add(279, kTypeLong, blockCount, bytesArray);
        }
        add(277, kTypeShort, 1, 4);
        add(284, kTypeShort, 1, 1);
        add(338, kTypeShort, 1, 1);
        add(339, kTypeShort, 4, fmtArray);
        std::sort(entries.begin(), entries.end(), [](const E& a, const E& b) { return a.tag < b.tag; });
        put16((uint16_t)entries.size());
        for (const E& en : entries) {
            uint8_t buf[12] = {0};
            auto w16 = [&](int off, uint16_t v) { buf[off] = (uint8_t)(v & 0xFF); buf[off + 1] = (uint8_t)(v >> 8); };
            auto w32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) buf[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF); };
            w16(0, en.tag); w16(2, en.type); w32(4, (uint32_t)en.count); w32(8, (uint32_t)en.value);
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
