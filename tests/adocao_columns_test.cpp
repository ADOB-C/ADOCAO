// `.adocao` 列编码器自测：逐位往返 + 编码选择 + **坏数据必须干净失败**（负向对照）。
//
// 为什么断言长这样：这条线的全部价值就是"逐位无损 + 绝不静默给出半截数据"。
// 所以除了正向往返，还有一组负向对照（截断、count 说谎、未知 codec、空输入），
// 它们必须失败 —— 否则坏文件会被当成好文件读进去（这正是本项目栽过两次的坑）。

#include "archive/AdocaoColumns.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace adofai;
using namespace adofai::adocao;   // ColumnStats / ColumnHeader / encodeXxx 都在这一层

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

bool sameBits(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(double)) != 0) return false;
    return true;
}

}  // namespace

int main() {
    // ① 先让校验函数本身可信：CRC32C("123456789") 的标准检验值是 0xE3069283
    check(adocao::crc32c("123456789", 9) == 0xE3069283u, "crc32c 已知检验值 0xE3069283");

    // ② 真实形状：100 万个角度里只有 52 个取值（MYC 实测 6,770,912 → 52）
    {
        std::vector<double> v(1000000);
        for (size_t i = 0; i < v.size(); ++i) {
            const size_t k = i % 52;
            v[i] = (k == 51) ? 999.0 : (double)k * 22.5;   // 含中旋哨兵 999
        }
        std::vector<uint8_t> enc;
        ColumnStats st;
        bool ok = adocao::encodeDoubleColumn(v, enc, &st);
        std::vector<double> back;
        ok = ok && adocao::decodeDoubleColumn(enc.data(), enc.size(), back);
        check(ok && sameBits(v, back), "字典列逐位往返（100 万值 / 52 个不同值）");
        check(std::strcmp(st.codec, "Dict") == 0, "52 个取值时选中 Dict");
        check(st.bits == 6, "字典下标 6 bit（52 项）");
        check(st.encodedBytes * 4 < st.rawBytes, "字典后体积降到 1/4 以下");
        std::printf("     字典列：%zu B = %.3f B/值（朴素 8 B/值），字典 %zu 项，%d bit\n",
                    st.encodedBytes, (double)st.encodedBytes / (double)v.size(),
                    st.dictEntries, st.bits);
    }

    // ③ 难缠的 double：±0.0 / 极值 / 次正规 / NaN / 音频谱那种长小数 + 伪随机位模式
    {
        std::vector<double> v = {
            0.0, -0.0, 1.0, -1.0, 999.0, 1e-300, -1e300, 4.547473508864641e-13,
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::denorm_min(),
            123456789.123456789,
        };
        uint64_t x = 0x123456789ABCDEFull;                 // 固定种子：跑动可复现
        for (int i = 0; i < 200000; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            double d;
            std::memcpy(&d, &x, sizeof(double));
            v.push_back(d);
        }
        std::vector<uint8_t> enc;
        ColumnStats st;
        bool ok = adocao::encodeDoubleColumn(v, enc, &st);
        std::vector<double> back;
        ok = ok && adocao::decodeDoubleColumn(enc.data(), enc.size(), back);
        check(ok && sameBits(v, back), "随机/极值/NaN 的 double 列逐位往返");
        std::printf("     %zu 值（含 NaN/次正规）→ %zu B（%s）\n", v.size(), enc.size(), st.codec);
    }

    // ④ int64 列：单调递增的 floor（差分 varint 的主场）
    {
        std::vector<int64_t> v(200000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (int64_t)i + (int64_t)(i % 7);
        std::vector<uint8_t> enc;
        ColumnStats st;
        bool ok = adocao::encodeIntColumn(v, enc, &st);
        std::vector<int64_t> back;
        ok = ok && adocao::decodeIntColumn(enc.data(), enc.size(), back);
        check(ok && back == v, "floor 列往返（单调 + 偶尔重复）");
        // 差分是 2,2,2,2,2,2,-5 的循环 → 六个相同差分构成游程 → DeltaRleVarint 胜出（合法）
        check(std::strcmp(st.codec, "DeltaRleVarint") == 0, "差分有游程时选中 DeltaRleVarint");
        check(st.encodedBytes < v.size(), "DeltaRle 后低于 1 B/值（差分有游程就有收益）");
        std::printf("     floor 列：%zu B = %.2f B/值（%s）\n", st.encodedBytes,
                    (double)st.encodedBytes / (double)v.size(), st.codec);
    }

    // ⑤ uint32 列：eventType 0..7（小枚举 → 位打包）
    {
        std::vector<uint32_t> v(300000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (uint32_t)(i % 8);
        std::vector<uint8_t> enc;
        ColumnStats st;
        bool ok = adocao::encodeU32Column(v, enc, &st);
        std::vector<uint32_t> back;
        ok = ok && adocao::decodeU32Column(enc.data(), enc.size(), back);
        check(ok && back == v, "枚举列往返（0..7）");
        check(std::strcmp(st.codec, "BitPack") == 0 && st.bits == 3, "0..7 选中 BitPack 且 3 bit");
        std::printf("     eventType 列：%zu B = %.3f B/值（%s, %d bit）\n", st.encodedBytes,
                    (double)st.encodedBytes / (double)v.size(), st.codec, st.bits);
    }

    // ⑥ 整列常量（实测 angleOffset / rotation / opacity 各只有 1 个取值）
    {
        std::vector<double> v(100000, 100.0);
        std::vector<uint8_t> enc;
        ColumnStats st;
        bool ok = adocao::encodeDoubleColumn(v, enc, &st);
        std::vector<double> back;
        ok = ok && adocao::decodeDoubleColumn(enc.data(), enc.size(), back);
        check(ok && sameBits(v, back) && std::strcmp(st.codec, "Const") == 0,
              "常量列往返且选中 Const");
        check(st.encodedBytes == sizeof(adocao::ColumnHeader) + 8, "常量列只占 28 B");
    }

    // ⑦ 空列（不能崩，且要能原样往返）
    {
        std::vector<double> v;
        std::vector<uint8_t> enc;
        ColumnStats st;
        std::vector<double> back;
        const bool ok = adocao::encodeDoubleColumn(v, enc, &st) &&
                        adocao::decodeDoubleColumn(enc.data(), enc.size(), back) && back.empty();
        check(ok, "空列往返");
    }

    // ⑧ 负向对照：坏数据必须**失败**，不能给出半截结果
    {
        std::vector<double> v(1000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (double)(i % 13);
        std::vector<uint8_t> enc;
        ColumnStats st;
        adocao::encodeDoubleColumn(v, enc, &st);
        std::vector<double> back;
        check(!enc.empty() && std::strcmp(st.codec, "Dict") == 0, "负向对照的样本列用 Dict 编成");

        check(!adocao::decodeDoubleColumn(enc.data(), enc.size() - 1, back),
              "截断 1 字节 → 必须失败");
        check(!adocao::decodeDoubleColumn(nullptr, 0, back), "空输入 → 必须失败（且不能崩）");

        std::vector<uint8_t> bad = enc;
        uint64_t c = 0;
        std::memcpy(&c, bad.data() + 4, 8);
        c += 1;                                            // 头里的 count 说谎
        std::memcpy(bad.data() + 4, &c, 8);
        check(!adocao::decodeDoubleColumn(bad.data(), bad.size(), back), "count 说谎 → 必须失败");

        bad = enc;
        bad[0] = 250;                                      // 未知 codec
        check(!adocao::decodeDoubleColumn(bad.data(), bad.size(), back), "未知 codec → 必须失败");

        bad = enc;
        bad[20 + 13 * 8] = 0xFF;                           // 下标流里塞越界下标（4 bit/值）
        bad[20 + 13 * 8 + 1] = 0xFF;
        check(!adocao::decodeDoubleColumn(bad.data(), bad.size(), back), "越界下标 → 必须失败");
    }

    // ⑨ DeltaVarint vs DeltaRleVarint 的分界：差分**互不相同**时没有游程 → 应选 DeltaVarint
    {
        std::vector<int64_t> v(50000);
        int64_t x = 0;
        for (size_t i = 0; i < v.size(); ++i) { x += (int64_t)(i % 7) + 1 + (int64_t)(i % 3) * 5; v[i] = x; }
        std::vector<uint8_t> enc;
        ColumnStats st;
        std::vector<int64_t> back;
        const bool ok = adocao::encodeIntColumn(v, enc, &st) &&
                        adocao::decodeIntColumn(enc.data(), enc.size(), back) && back == v;
        check(ok && std::strcmp(st.codec, "DeltaVarint") == 0, "差分无游程时选中 DeltaVarint");
    }

    // ⑩ Rle 专用用例：几段长游程（不是整列常量，所以不会退化成 Const）
    {
        std::vector<uint32_t> v;
        v.insert(v.end(), 100000, 3u);
        v.insert(v.end(), 200000, 5u);
        v.insert(v.end(), 50000, 9u);
        std::vector<uint8_t> enc;
        ColumnStats st;
        std::vector<uint32_t> back;
        const bool ok = adocao::encodeU32Column(v, enc, &st) &&
                        adocao::decodeU32Column(enc.data(), enc.size(), back) && back == v;
        check(ok && std::strcmp(st.codec, "Rle") == 0, "长游程的列选中 Rle");
        check(st.encodedBytes < 64, "长游程的列塌成几十字节");
        std::printf("     三段长游程（35 万值）→ %zu B（%s）\n", st.encodedBytes, st.codec);
    }

    // ⑪ 新编码的负向对照：RLE 载荷被改坏 / 截断，都必须干净失败
    {
        std::vector<uint32_t> v;
        v.insert(v.end(), 1000, 1u);
        v.insert(v.end(), 1000, 2u);
        std::vector<uint8_t> enc;
        ColumnStats st;
        adocao::encodeU32Column(v, enc, &st);
        std::vector<uint32_t> back;
        check(std::strcmp(st.codec, "Rle") == 0, "负向对照的样本列用 Rle 编成");
        check(!adocao::decodeU32Column(enc.data(), enc.size() - 1, back), "Rle 截断 → 必须失败");
        std::vector<uint8_t> bad = enc;
        // 把"游程总数"改大 → 展开后总数对不上 → 必须失败
        uint64_t rc = 0;
        const size_t runOff = adocao::ColumnHeader{}.count ? 0 : 0;   // 占位，下面按布局算
        (void)runOff;
        std::memcpy(&rc, bad.data() + 20 + 2 * 4, 8);                 // 20 列头 + 2 项 u32 字典
        rc += 1000;                                                   // 谎报游程数
        std::memcpy(bad.data() + 20 + 2 * 4, &rc, 8);
        check(!adocao::decodeU32Column(bad.data(), bad.size(), back), "Rle 游程数说谎 → 必须失败");
    }

    std::printf("\n%s：%s（%d 项失败）\n", g_fail ? "FAIL" : "ok",
                "adocao 列编码器自测", g_fail);
    return g_fail ? 1 : 0;
}
