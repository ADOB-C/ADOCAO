#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 流式 PNG 写出（RGBA8，8bit，非隔行）：一行一行喂，内存只占一行 + deflate 状态。
// 用依赖里已有的 miniz tdefl 做 zlib 压缩，所以不需要 libpng / zlib / ffmpeg。
// 这是为"1 像素 = 1 层"那种 1.9M x 0.78M 的画布准备的：整张图 5.7 TB，攒缓冲是不可能的。
class PngStreamWriter {
public:
    ~PngStreamWriter() { close(); }
    PngStreamWriter(const PngStreamWriter&) = delete;
    PngStreamWriter& operator=(const PngStreamWriter&) = delete;
    PngStreamWriter() = default;

    bool open(const std::string& path, uint32_t width, uint32_t height);
    // 索引色 1 bit（两种颜色）：整张 1.94M x 785k 的图因此只有 243 KB/行，
    // 而且颜色与瓦片完全一致（黑白 + 轨道色）。packed 行按 MSB-first。
    bool openIndexed1(const std::string& path, uint32_t width, uint32_t height,
                      uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1);
    bool writeRow(const uint8_t* rgbaRow);       // width*4 字节（RGBA8 模式）
    bool writePackedRow(const uint8_t* packed);  // (width+7)/8 字节（索引 1 bit 模式）
    bool close();
    bool failed() const { return m_failed; }
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }

private:
    bool openCommon(const std::string& path, uint32_t width, uint32_t height);
    bool writeChunk(const char* type, const uint8_t* data, size_t len);
    bool pump(bool finish);

    std::FILE* m_f = nullptr;
    uint32_t m_w = 0, m_h = 0, m_written = 0;
    void* m_defl = nullptr;                     // tdefl_compressor*
    std::vector<uint8_t> m_out;                 // deflate 输出缓冲
    std::vector<uint8_t> m_prev;                // 上一行，用于 PNG "Up" 滤波
    std::vector<uint8_t> m_line;                // 滤波后的行（1 + 行字节数）
    size_t m_rowBytes = 0;                      // 每行未滤波的字节数
    int m_colorType = 6;                        // 6 = RGBA8, 3 = 索引
    int m_bitDepth = 8;                         // 8 或 1
    uint8_t m_palette[6] = {0, 0, 0, 0, 0, 0};  // 索引 0 / 1 的 RGB
    bool m_failed = false;
};
