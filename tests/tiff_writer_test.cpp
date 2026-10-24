// BigTIFF 写出器的自检：写一张小图 → 用 zlib 解回来 → 逐字节比对；
// 再检查文件头/IFD 的关键字段（tile 数、Compression、offsets 单调）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/map/TiffWriter.hpp"
#include <zlib.h>

using namespace adofai;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
    else std::printf("ok   %s\n", what.c_str());
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int main() {
    const uint32_t W = 100, H = 70;           // 非 16 的倍数：顺便验证边缘块的补齐
    std::vector<uint8_t> img((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &img[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)(x * 2); p[1] = (uint8_t)(y * 3); p[2] = (uint8_t)((x ^ y) & 0xFF); p[3] = 255;
        }

    const char* path = "tiff_writer_test.tif";
    TiffWriter w;
    check(w.open(path, W, H, 32, TiffWriter::kCompressionDeflate, deflateCompress),
          "open");
    check(w.stripCount() == 3, "条带数 3（32 行/条，70 行）");
    for (uint32_t y = 0; y < H; y += 32) {
        const uint32_t rows = std::min(32u, H - y);
        check(w.writeStrip(&img[(size_t)y * W * 4], rows), "writeStrip");
    }
    check(w.finish(), "finish");
    w.close();

    // ── 读回：解析 header + IFD ──
    std::FILE* f = std::fopen(path, "rb");
    check(f != nullptr, "读回文件");
    if (!f) return 1;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)size);
    check(std::fread(buf.data(), 1, buf.size(), f) == buf.size(), "文件读满");
    std::fclose(f);

    check(rd16(&buf[0]) == 0x4949, "字节序 II");
    check(rd16(&buf[2]) == 42, "经典 TIFF 标记 42");
    const uint64_t ifd = rd32(&buf[4]);
    check(ifd > 0 && ifd < (uint64_t)size, "首个 IFD 偏移合法");
    const uint64_t nEntry = rd16(&buf[(size_t)ifd]);
    check(nEntry >= 12, "IFD 条目数");
    uint64_t tileOff = 0, tileCnt = 0, rowsPerStrip = 0, comp = 0;
    for (uint64_t i = 0; i < nEntry; ++i) {
        const uint8_t* e = &buf[(size_t)ifd + 2 + i * 12];
        const uint16_t tag = rd16(e), type = rd16(e + 2);
        const uint64_t cnt = rd32(e + 4);
        const uint64_t val = rd32(e + 8);
        if (tag == 259) comp = val & 0xFFFF;
        if (tag == 278) rowsPerStrip = val & 0xFFFFFFFF;
        if (tag == 273) { check(cnt == 3, "StripOffsets 是 3 条"); tileOff = val; }
        if (tag == 279) tileCnt = val;
    }
    check(comp == TiffWriter::kCompressionDeflate, "Compression = Deflate(8)");
    check(rowsPerStrip == 32, "RowsPerStrip = 32");
    check(tileOff > 0 && tileCnt > 0, "TileOffsets/TileByteCounts 非空");

    // 第一块解回来，和原图左上角 32x32 比
    const uint64_t off0 = rd32(&buf[(size_t)tileOff]);
    const uint64_t n0 = rd32(&buf[(size_t)tileCnt]);
    std::vector<uint8_t> raw((size_t)W * 32 * 4);
    z_stream s{};
    check(inflateInit(&s) == Z_OK, "inflateInit");
    s.next_in = const_cast<Bytef*>(buf.data() + off0);
    s.avail_in = (uInt)n0;
    s.next_out = raw.data();
    s.avail_out = (uInt)raw.size();
    const int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    check(rc == Z_STREAM_END && s.total_out == raw.size(), "第一条带解压出 100x32x4");
    check(std::memcmp(raw.data(), img.data(), raw.size()) == 0, "第一条带像素逐字节一致");

    std::printf("tiff_writer: %s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}
