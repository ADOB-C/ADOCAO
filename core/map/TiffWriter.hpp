#pragma once
// **流式 TIFF 写出器**（按**条带**独立压缩）：给"整张谱面全景"那种巨大画布用。
//
// 为什么不用 libtiff：它把 zlib/jpeg/lzma/webp 全拖进来，而我们要的东西很少 ——
// 条带（RowsPerStrip）、每段独立压缩、可选 BigTIFF。自己写 ~300 行没有依赖，
// 压缩器还可以由调用方注入（core 不依赖 zstd；app 用 zstd，测试用 deflate）。
//
// ── 为什么是**条带**而不是**分块（Tile）** ──────────────────────────────────
// 2026-10-23 实测：macOS ImageIO **不认 tiled TIFF**（`sips -g pixelWidth` 返回 nil，
// 连 ImageMagick 生成的 tiled TIFF 也一样），但同尺寸的 strip 版本秒开。
// 条带同样能"凑够一段就压一段"（每段 = RowsPerStrip 行 × 全宽），
// 内存 = 一条带 + 每段 8 B 索引，增量压缩/getseek 能力完全够用。
//
// 布局（Little-endian）：
//   Header → 每段压缩数据（顺序写，记 offset/size）→ 偏移数组 → IFD（末尾）
//
// **两种容器**：默认**经典 TIFF**（32 位偏移；Preview / PIL / ImageMagick 都认）；
// 画布大到 4 GB 偏移不够时才写 BigTIFF（`bigTiff=true`）。
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace adofai {

class TiffWriter {
public:
    // 压缩器：输入一段 RGBA8（w*rows*4），输出压缩后的字节。返回 false = 存原始段。
    using Compressor = std::function<bool(const uint8_t*, size_t, std::vector<uint8_t>&)>;
    static constexpr uint16_t kCompressionNone    = 1;
    static constexpr uint16_t kCompressionDeflate = 8;

    ~TiffWriter() { close(); }
    TiffWriter() = default;
    TiffWriter(const TiffWriter&) = delete;
    TiffWriter& operator=(const TiffWriter&) = delete;

    // rowsPerStrip 建议 64~512（越大压缩率略好、内存越大）。
    bool open(const std::string& path, uint32_t width, uint32_t height, uint32_t rowsPerStrip,
              uint16_t compression, Compressor compressor, bool bigTiff = false);
    // 按**行优先**顺序喂：每段恰好 rowsPerStrip 行（最后一段可以少），每行 width*4 字节。
    bool writeStrip(const uint8_t* rgba, uint32_t rows);
    bool finish();
    bool close();

    uint32_t rowsPerStrip() const { return m_rowsPerStrip; }
    uint32_t stripCount() const { return m_strips; }
    uint64_t bytesWritten() const { return m_pos; }

private:
    bool writeRaw(const void* p, size_t n);
    void put16(uint16_t v);
    void put32(uint32_t v);
    void put64(uint64_t v);

    std::FILE* m_f = nullptr;
    uint32_t m_w = 0, m_h = 0;
    uint32_t m_rowsPerStrip = 256;
    uint32_t m_strips = 0;
    uint32_t m_rowsWritten = 0;
    uint16_t m_compression = kCompressionNone;
    Compressor m_compressor;
    uint64_t m_pos = 0;
    std::vector<uint64_t> m_offsets;
    std::vector<uint64_t> m_bytes;
    std::vector<uint8_t> m_tmp;
    bool m_big = false;
    bool m_failed = false;
    bool m_finished = false;
};

// 简单的 Deflate 压缩器（zlib）：给测试与没有 zstd 的宿主用。
bool deflateCompress(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

}  // namespace adofai
