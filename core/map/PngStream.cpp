#include "core/map/PngStream.hpp"

#include "core/util/Logger.hpp"

#include <cstring>

// miniz 的底层 deflate API（依赖里已经有了；audio 用它做 zip，core 用来做 PNG）
#include "miniz.h"



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

const uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

}  // namespace

bool PngStreamWriter::open(const std::string& path, uint32_t width, uint32_t height) {
    m_colorType = 6; m_bitDepth = 8; m_rowBytes = (size_t)width * 4;
    return openCommon(path, width, height);
}

bool PngStreamWriter::openIndexed1(const std::string& path, uint32_t width, uint32_t height,
                                   uint8_t r0, uint8_t g0, uint8_t b0,
                                   uint8_t r1, uint8_t g1, uint8_t b1) {
    m_colorType = 3; m_bitDepth = 1; m_rowBytes = ((size_t)width + 7) / 8;
    m_palette[0] = r0; m_palette[1] = g0; m_palette[2] = b0;
    m_palette[3] = r1; m_palette[4] = g1; m_palette[5] = b1;
    return openCommon(path, width, height);
}

bool PngStreamWriter::openCommon(const std::string& path, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    m_w = width; m_h = height; m_written = 0;
    m_f = std::fopen(path.c_str(), "wb");
    if (!m_f) { LOG_E("PngStream: 无法写入 %s", path.c_str()); return false; }
    if (std::fwrite(kSig, 1, sizeof kSig, m_f) != sizeof kSig) { m_failed = true; return false; }

    // IHDR：8bit / RGBA / 无压缩选项 / 无隔行
    std::vector<uint8_t> ihdr;
    put32(ihdr, m_w); put32(ihdr, m_h);
    ihdr.push_back((uint8_t)m_bitDepth); ihdr.push_back((uint8_t)m_colorType);
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    if (!writeChunk("IHDR", ihdr.data(), ihdr.size())) return false;
    if (m_colorType == 3 && !writeChunk("PLTE", m_palette, sizeof m_palette)) return false;

    // 用 miniz 的 zlib 兼容层做流式 deflate：IDAT 必须是**一条** zlib 流，
    // 所以不能一段一段地各自压缩。
    mz_stream* zs = (mz_stream*)std::calloc(1, sizeof(mz_stream));
    m_defl = zs;
    m_out.resize(1u << 20);
    if (mz_deflateInit(zs, MZ_DEFAULT_COMPRESSION) != MZ_OK) { m_failed = true; return false; }
    m_prev.assign(m_rowBytes, 0);
    m_line.assign(1 + m_rowBytes, 0);
    return true;
}

bool PngStreamWriter::writeChunk(const char* type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> hdr;
    put32(hdr, (uint32_t)len);
    if (std::fwrite(hdr.data(), 1, hdr.size(), m_f) != hdr.size()) { m_failed = true; return false; }
    if (std::fwrite(type, 1, 4, m_f) != 4) { m_failed = true; return false; }
    if (len && std::fwrite(data, 1, len, m_f) != len) { m_failed = true; return false; }
    uint8_t crcbuf[4];
    std::memcpy(crcbuf, type, 4);
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, crcbuf, 4);
    if (len) crc = mz_crc32(crc, data, len);
    uint8_t out[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    if (std::fwrite(out, 1, 4, m_f) != 4) { m_failed = true; return false; }
    return true;
}

bool PngStreamWriter::pump(bool finish) {
    mz_stream* zs = (mz_stream*)m_defl;
    for (;;) {
        zs->next_in = nullptr; zs->avail_in = 0;
        zs->next_out = m_out.data(); zs->avail_out = (unsigned)m_out.size();
        const int st = mz_deflate(zs, finish ? MZ_FINISH : MZ_NO_FLUSH);
        const size_t produced = m_out.size() - zs->avail_out;
        if (produced && !writeChunk("IDAT", m_out.data(), produced)) return false;
        if (st == MZ_STREAM_END) return true;
        if (st != MZ_OK) { m_failed = true; return false; }
        if (produced == 0) return true;
    }
}

bool PngStreamWriter::writeRow(const uint8_t* rgbaRow) {
    if (m_colorType != 6) return false;
    return writePackedRow(rgbaRow);
}

bool PngStreamWriter::writePackedRow(const uint8_t* packed) {
    if (m_failed || !m_f || m_written >= m_h) return false;
    // PNG "Up" 滤波（type 2）：稀疏图（大片纯背景）压缩率提升巨大
    m_line[0] = 2;
    const size_t n = m_rowBytes;
    for (size_t i = 0; i < n; ++i) m_line[1 + i] = (uint8_t)(packed[i] - m_prev[i]);
    std::memcpy(m_prev.data(), packed, n);
    mz_stream* zs = (mz_stream*)m_defl;
    zs->next_in = m_line.data(); zs->avail_in = (unsigned)m_line.size();
    while (zs->avail_in) {
        zs->next_out = m_out.data(); zs->avail_out = (unsigned)m_out.size();
        const int st = mz_deflate(zs, MZ_NO_FLUSH);
        const size_t produced = m_out.size() - zs->avail_out;
        if (produced && !writeChunk("IDAT", m_out.data(), produced)) return false;
        if (st != MZ_OK && st != MZ_BUF_ERROR) { m_failed = true; return false; }
    }
    ++m_written;
    return true;
}

bool PngStreamWriter::close() {
    if (!m_f) return true;
    bool ok = true;
    if (!m_failed && m_written == m_h) ok = pump(true);
    if (ok && !m_failed) ok = writeChunk("IEND", nullptr, 0);
    std::fclose(m_f);
    m_f = nullptr;
    if (m_defl) { mz_deflateEnd((mz_stream*)m_defl); std::free(m_defl); m_defl = nullptr; }
    return ok && !m_failed;
}
