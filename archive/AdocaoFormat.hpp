#pragma once

// `.adocao` 二进制容器的格式常量与定长结构（v1）。
//
// 设计原则（详见 docs/adocao-format.md）：**列式 + 每列按其取值基数选编码**。
// 与谱面来源无关：字典/常量/差分/位打包对任何谱都适用；音频谱只是"取值集合恰好是
// int16"的一个特例，不需要任何音频专用机制（早期草案里的 PCM 机制已删除）。
//
// 三条硬规矩：
//   1. **逐位无损**：字典里存原始 8 字节位模式，解码即原值，不做任何数值变换
//      （所以 §精度规则 不受影响：我们只改"表示"，不改"值"）。
//   2. **失败必须响亮**：crc 坏 / 截断 / version 不符 / 未知 codec 一律明确报错，
//      绝不静默回退（这条是被 2026-10 的两次"静默回退让测试全绿"教训逼出来的）。
//   3. **hash 只作溯源**：writerCommit / inputHash 用于排查与可复现性断言，
//      **绝不作兼容闸门** —— 版本门禁只认 `version`。无 git 环境写全 0 并标 unknown，
//      不影响可读性。

#include <cstddef>
#include <cstdint>

#include "core/level/ByteSource.hpp"   // kAdocaoMagic（magic 的唯一来源）

namespace adofai {
namespace adocao {

// 文件 magic：与 core/level/ByteSource.cpp 的 sniffLevelArchive 一致（按 magic 判断，
// 不看扩展名，所以改过名/抹掉扩展名照样能读）。
// magic 的唯一来源在 core（sniffLevelArchive 也要用它）；这里逐字节拷贝并由 static_assert 钉住。
inline constexpr char kMagic[4] = {kAdocaoMagic[0], kAdocaoMagic[1], kAdocaoMagic[2], kAdocaoMagic[3]};
static_assert(kAdocaoMagic[0] == 'A' && kAdocaoMagic[1] == 'D' && kAdocaoMagic[2] == 'O' &&
              kAdocaoMagic[3] == '1', "core 的 kAdocaoMagic 变了，archive 这边要一起改");
inline constexpr uint16_t kVersion  = 2;   // v2：Actions 段改成"稀疏载荷列"，新增 Rle/DeltaRleVarint 与段级压缩

// 段 id。v1 只用 1..5；6/7 预留（preserved / derived），未知 id 一律跳过（前向兼容）。
enum class SectionId : uint8_t {
    Settings   = 1,
    AngleData  = 2,
    Actions    = 3,
    StringPool = 4,
    PathData   = 5,
    Preserved  = 6,   // 未识别成员的原始字节（decorations、未知 eventType）—— 供 ADOCAO-E 无损回写
    Derived    = 7,   // 预计算/检查点 —— v1 不写
};

// 列/段的编码方式。`ColumnHeader::codec` 与 `SectionEntry::codec` 共用这一套。
enum class Codec : uint8_t {
    Raw           = 0,   // 原始定宽（兜底）
    Dict          = 1,   // 精确值字典 + 位打包下标（主编码：MYC 6.77M 个角度只有 52 个取值）
    DeltaVarint   = 2,   // 差分 + zigzag varint（单调列，如 floor）
    Const         = 3,   // 整列同一个值（angleOffset / rotation / opacity 实测各只有 1 个取值）
    BitPack       = 4,   // 小枚举/位标志
    JsonPassthrough = 5, // 兜底：原样文本（诊断/未知段用）
    // —— 列级（写在 ColumnHeader.codec 里）——
    Rle = 6,            // 字典 + (下标, 游程长度)：抓"长游程"（直线型赫兹谱的 angleData、type/flag/val1/val2）
    DeltaRleVarint = 7, // 差分 + 对差分再取游程：抓"单调且差分恒定"（floor 恒 +1 → 十几字节）
    // —— 段级（写在 SectionEntry.codec 里；与列级同字段但取值区间分开）——
    SectionZstd = 16,   // 段内容是一整帧 zstd
    SectionXz   = 17,   // 段内容是一整帧 xz（体积敏感时用，实测真实文件上 zstd 更小）
};

#pragma pack(push, 1)

// 段目录项（40 B）
struct SectionEntry {
    uint8_t  id       = 0;
    uint8_t  codec    = 0;
    uint16_t flags    = 0;
    uint64_t offset   = 0;   // 相对文件头，8 B 对齐（为 mmap 直读）
    uint64_t compSize = 0;   // 段在文件里的字节数
    uint64_t rawSize  = 0;   // 解压/解码后的字节数
    uint64_t elemCount = 0;  // 这一段的元素个数（u64：从格式层消除 2^31 边界）
    uint32_t crc32c   = 0;   // compSize 字节的 CRC32C
};
static_assert(sizeof(SectionEntry) == 40, "SectionEntry 必须保持 40 B 定长");

// 文件头（固定 76 B，之后紧跟 sectionCount 个 SectionEntry）
struct Header {
    char     magic[4]      = {'A', 'D', 'O', '1'};
    uint16_t version       = kVersion;
    uint16_t flags         = 0;
    uint16_t sectionCount  = 0;
    uint16_t reserved      = 0;
    uint64_t fileSize      = 0;
    uint32_t headerCrc     = 0;   // 覆盖 magic..fileSize 这段（不含自身）
    uint8_t  writerCommit[20] = {};  // 写入方 git commit（SHA-1 原始字节）；全 0 = unknown
    uint8_t  inputHash[32]    = {};  // 源谱规范化字节的 SHA-256；全 0 = unknown
};
static_assert(sizeof(Header) == 76, "Header 必须保持 76 B 定长");

// 每个列段的列头（20 B），后跟该列的载荷
struct ColumnHeader {
    uint8_t  codec = 0;   // Codec
    uint8_t  bits  = 0;   // 位打包用的位宽（Dict/BitPack 用；其余编码为 0）
    uint16_t flags = 0;
    uint64_t count = 0;   // 元素个数（u64：从格式层消除 2^31 边界）
    uint64_t aux   = 0;   // 字典项数（Dict 用；其余编码为 0）
};
static_assert(sizeof(ColumnHeader) == 20, "ColumnHeader 必须保持 20 B 定长");

#pragma pack(pop)

// CRC32C（Castagnoli，与 zstd/xz 的校验风格一致）：段与头都要能校验出坏数据。
// 不用 zlib 的 crc32（那是 CRC32/IEEE，多项式不同，别混）。
uint32_t crc32c(const void* data, size_t len, uint32_t seed = 0);

}  // namespace adocao
}  // namespace adofai
