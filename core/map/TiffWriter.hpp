#pragma once
// **流式 TIFF 写出器**（每块独立压缩）：给"整张谱面全景"那种巨大画布用。
//
// 为什么不用 libtiff：它把 zlib/jpeg/lzma/webp 全拖进来，而我们要的东西很少 ——
// 分块（Tile）或条带（Strip）、每块独立压缩、可选 BigTIFF。自己写 ~350 行没有依赖，
// 压缩器还可以由调用方注入（core 不依赖 zstd；app 用 zstd，测试用 deflate）。
//
// ── 两种布局（2026-10-23 实测）────────────────────────────────────────────────
// * **Tile**：随机访问最好（看右下角只解几块 256x256），但 **macOS ImageIO 不认 tiled TIFF**
//   （`sips` 返回 nil，连 ImageMagick 生成的 tiled 文件也一样）；
// * **Strip**：全宽 × N 行，兼容所有读者，但看一个角落也要解一整条（整宽）。
//
// 默认由调用方决定（CLI `--tiff-layout=strip|tile`，默认 tile）。压缩同理
// （`--tiff-compress=zstd|deflate`，默认 zstd —— 但 zstd 的 Compression=50000 同样
// 不被 macOS ImageIO 支持）。
//
// 布局（Little-endian）：
//   Header → 每块压缩数据（顺序写，记 offset/size）→ 辅助数组 → IFD（末尾）
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace adofai {

class TiffWriter {
public:
    // 压缩器：输入一块 RGBA8（blockW*blockH*4），输出压缩后的字节。返回 false = 存原始块。
    using Compressor = std::function<bool(const uint8_t*, size_t, std::vector<uint8_t>&)>;
    static constexpr uint16_t kCompressionNone    = 1;
    static constexpr uint16_t kCompressionDeflate = 8;
    static constexpr uint16_t kCompressionZstd    = 50000;

    enum class Layout { Strips, Tiles };

    ~TiffWriter() { close(); }
    TiffWriter() = default;
    TiffWriter(const TiffWriter&) = delete;
    TiffWriter& operator=(const TiffWriter&) = delete;

    // Tiles：tileW/tileH 会向上取整到 16 的倍数（TIFF 规范要求）；
    // Strips：tileW 强制 = width，tileH = 条带行数。
    bool open(const std::string& path, uint32_t width, uint32_t height, Layout layout,
              uint32_t tileW, uint32_t tileH, uint16_t compression, Compressor compressor,
              bool bigTiff = false);
    // 按"块行优先"顺序喂：每块恰好 blockW*blockH*4 字节（最后一块由调用方补 0 到整块）。
    bool writeBlock(const uint8_t* rgba);
    bool finish();
    bool close();

    uint32_t blockW() const { return m_blockW; }
    uint32_t blockH() const { return m_blockH; }
    uint32_t blocksX() const { return m_blocksX; }
    uint32_t blocksY() const { return m_blocksY; }
    Layout layout() const { return m_layout; }
    uint64_t bytesWritten() const { return m_pos; }

private:
    bool writeRaw(const void* p, size_t n);
    void put16(uint16_t v);
    void put32(uint32_t v);
    void put64(uint64_t v);

    std::FILE* m_f = nullptr;
    uint32_t m_w = 0, m_h = 0;
    uint32_t m_blockW = 0, m_blockH = 0;
    uint32_t m_blocksX = 0, m_blocksY = 0;
    size_t m_nextBlock = 0;
    Layout m_layout = Layout::Tiles;
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
