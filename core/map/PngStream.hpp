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
    bool writeRow(const uint8_t* rgbaRow);      // width*4 字节，自上而下依次调用
    bool close();
    bool failed() const { return m_failed; }
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }

private:
    bool writeChunk(const char* type, const uint8_t* data, size_t len);
    bool pump(bool finish);

    std::FILE* m_f = nullptr;
    uint32_t m_w = 0, m_h = 0, m_written = 0;
    void* m_defl = nullptr;                     // tdefl_compressor*
    std::vector<uint8_t> m_out;                 // deflate 输出缓冲
    std::vector<uint8_t> m_prev;                // 上一行，用于 PNG "Up" 滤波
    std::vector<uint8_t> m_line;                // 滤波后的行（1 + w*4）
    bool m_failed = false;
};
