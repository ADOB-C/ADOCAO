#pragma once

// `.adocao` v3 的**分块列容器**（`docs/adocao-format.md` §8.3 ⑥）：
//
//   一列按 `kBlockElems`（4096，与 §8.8 的 checkpoint 间隔**同一个数**）切块，
//   **每块独立编码** —— 自带列头、自带字典、首值是绝对值，于是任意一块都能单独解码
//   （不需要它前面的任何一块）。若干块打包进一个 **zstd 帧**，帧是"独立可解压"的最小
//   单位。于是：
//
//   * `检查点/数据块/窗口` 三者同界（4096 层）—— ③ 的窗口化直接消费这个边界；
//   * **压缩帧大小与 seek 粒度解耦**（§8.3）：2^31 层按 4096 切是 512,695 块，若每块一帧，
//     光帧头就要几十 MB、而且 32 KB 帧的压缩率明显更差。这里帧聚合 ~256 KB / ≤256 块，
//     帧表只有 frameCount 条（2^31 层约 1.7 万条 ≈ 0.8 MB）。
//
// 布局（全部小端、定宽；见 docs/adocao-format.md §8.11）：
//
//   u8 kind | u8 layout | u16 reserved | u64 totalElements
//   layout == Single :
//       u8 codec | u8[3] reserved | u64 rawSize | u64 storedSize | <storedSize 字节>
//   layout == Blocked:
//       u32 blockElems | u32 reserved | u64 frameCount
//       frameCount × FrameEntry (48 B) = { firstBlock, blockCount, frameOffset, compSize,
//                                          rawSize, codec, reserved }
//       frames 区（每帧 8 B 对齐）
//
//   帧解压后 = 若干个 { u32 blockLen, <blockLen 字节的自描述列 blob> }。
//
// 三条硬规矩（与列编码器一致，§8.0）：
//   1. **逐位无损** —— 每块用 `AdocaoColumns` 的编码器（它自带"编完解回来逐位比"的自检）；
//      容器自己再验一遍"整列解回来逐位相同"，不一致就退回单块布局。
//   2. **最小由实测决定** —— 单块与分块**都真编一遍**，按**实际存储字节**取最小，
//      所以"加分块"的最坏情况是零收益（不会让任何谱变大）。
//   3. **坏数据干净失败** —— 帧表越界 / 未知 codec / 块长越界 / kind 不符 → false，绝不猜。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "archive/AdocaoFormat.hpp"

namespace adofai {
namespace adocao {

// 列的元素类型（自描述地写进容器头：解码方拿它挡住"用 u32 解释 double 列"这类错误）
enum class ColKind : uint8_t { Double = 0, Int64 = 1, U32 = 2 };

inline constexpr uint32_t kBlockElems = 4096;         // = checkpoint 间隔（§8.8）
inline constexpr uint32_t kMaxBlocksPerFrame = 256;   // 一块帧最多装多少块
inline constexpr size_t   kFrameTargetRaw = 256u << 10;  // 帧解压后的目标大小（§8.3 的 64 KB~1 MB）

// **强制分块**的阈值：单列裸数据到 128 MB（f64 = 1600 万个值 ≈ 1600 万层）时，分块是
// **要求**而不是优化 —— 再大就不可能整份 materialize，③ 的窗口化没有它无从谈起。
//
// 为什么阈值要定得这么高（实测，2026-10-15）：分块把一列切成 k 个**独立可解压**的单元，
// 而独立单元必然输给"一整条实心流"（zstd 的熵表被摊薄、每块/每帧各付一次头部与字典）：
//   * The Moon angleData  1394 B → 2970 B（0.47×）
//   * MYC      angleData  3055 B → 10594 B（0.29×）
//   * Unity    actions.val1  21.07 MB → 31.68 MB（0.67×，强制分块会让容器 +51%）
// 而 §8.3 的"分段型 1.66~3.29×"是**编码级**口径（不含 zstd，见 §8.5 的注），
// 段级 zstd 会把那份收益吃掉（实测同族合成列：编码级 3.87/2.19/1.70×，存储级 1.00/0.97/0.96×）。
// 所以：**阈值以下按实编取最小（零回归）**，阈值以上强制分块（拿回可寻址性）。
// 想把窗口化下探到 MYC/Unity 规模（1e7 层）之前，得先做 §8.3 的选项②（字典段跨块共享）。
inline constexpr size_t kMustBlockBytes = 128u << 20;

// 测试钩子：把上面的阈值改小，好在单测里覆盖"强制分块"这条规则（生产不调用 = 用默认值）
void setMustBlockBytesForTest(size_t n);
inline constexpr uint8_t  kLayoutSingle  = 0;
inline constexpr uint8_t  kLayoutBlocked = 1;
inline constexpr size_t   kContainerHeader = 12;      // kind/layout/reserved/total
inline constexpr size_t   kFrameEntry = 48;
inline constexpr size_t   kSingleHeader = 20;         // codec(1)+reserved(3)+rawSize(8)+storedSize(8)

// 一次编码的体积账（`adocao pack --codec-report` 用它量"分块到底赚了多少"）
struct ColumnLayoutStats {
    bool   blocked = false;      // 最终选了哪种布局
    bool   forced = false;       // true = 不是"分块更小"，而是触发了**强制分块**（见 kMustBlockBytes）
    size_t blocks = 0;           // 分块布局的块数（未分块为 0）
    size_t frames = 0;
    size_t singleBytes = 0;      // "单块 + 自压"的实际存储字节（分块的**存储级**比较基线）
    size_t blockedBytes = 0;     // 分块布局的实际存储字节（容器头 + 帧表 + 各帧）
    // **编码级**体积（都不含 zstd）：§8.3/§8.5 量"分段型收益 1.66~3.29×"用的就是这一档口径
    // （"合成列、无段级压缩"）。生产路径按存储级取最小，所以两个口径都要能看见。
    size_t singleCodecBytes = 0; // 整列一个 blob 的字节数
    size_t blockedCodecBytes = 0;// 各块 blob 之和（不含块长前缀/帧表/帧头）
};

// 一列怎么存（§8.0-2 的"实编取最小"在**段级**做，见 AdocaoWriter）。
// **段级口径很重要**：v2 是把整个段（Actions 的 6 列一起）压成一帧 zstd，
// 那比"每列各压一帧"更省（跨列共享熵表、少 6 个帧头。实测 The Moon：3544 B vs 3992 B）。所以：
//   LegacyV2   —— **裸列字节、不带 v3 容器头**，段级 zstd 照旧。逐字节等于 v2 文件（回归兜底）；
//                  任何输入只要分块没赚到就落回这里，所以 **v3 的文件永远 ≤ v2**。
//                  例外：裸数据 ≥ kMustBlockBytes 的列**强制分块**（窗口化是要求，不是优化）。
//   SingleZstd —— v3 容器，载荷自己压一帧（段级不再压）。
//   BestSelf   —— v3 容器，在 SingleZstd 与 Blocked 里取实际存储更小的那个（段级不再压）。
enum class ColumnStorage : uint8_t { LegacyV2 = 0, SingleZstd = 1, BestSelf = 2 };

// 编码一列容器。
bool encodeColumnV3(ColKind kind, const void* data, size_t count, ColumnStorage storage,
                    std::vector<uint8_t>& out, ColumnLayoutStats* st = nullptr);

// 解码整列（全量路径）。kind 必须与容器头里写的相符，否则失败。
bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<double>& out);
bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<int64_t>& out);
bool decodeColumnV3(const uint8_t* p, size_t n, std::vector<uint32_t>& out);

// —— 按块随机访问（③"每砖结构窗口化"要消费的地基）——
//
// openColumnV3 只解析容器头与帧表（O(frameCount)，**不碰任何帧内容**）。
// decodeColumnBlock 只解压目标块所在的那一帧 —— 别的帧不会被碰。
struct ColumnView {
    ColKind kind = ColKind::Double;
    bool blocked = false;
    uint64_t total = 0;

    const uint8_t* container = nullptr;   // 容器起点（单块布局解码用）
    size_t containerSize = 0;

    uint32_t blockElems = 0;
    uint64_t frameCount = 0;
    const uint8_t* frameTable = nullptr;  // frameCount × kFrameEntry
    const uint8_t* frames = nullptr;      // 帧区起点
    size_t framesSize = 0;

    // 帧表访问（都带边界检查，越界返回 0 / false）
    uint64_t frameFirstBlock(uint64_t f) const;
    uint64_t frameBlockCount(uint64_t f) const;
    // 第 blockIndex 块落在哪一帧、帧内第几块
    bool locateBlock(uint64_t blockIndex, uint64_t& frame, uint64_t& blockInFrame) const;
};

bool openColumnV3(const uint8_t* p, size_t n, ColumnView& out);

bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<double>& out);
bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<int64_t>& out);
bool decodeColumnBlock(const ColumnView& v, uint64_t blockIndex, std::vector<uint32_t>& out);

}  // namespace adocao
}  // namespace adofai
