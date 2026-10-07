#pragma once

// `.adocao` 的**列编码器**：把"一列同类型的值"压成一段字节流。
//
// 设计原则（见 docs/adocao-format.md）：**列式 + 每列按其取值基数选编码** ——
// 字典 / 常量 / 差分 / 位打包 / 原始。与谱面来源无关；音频谱只是"取值集合恰好是 int16"
// 的一个特例，不需要任何音频专用机制。
//
// 三条硬规矩：
//   1. **逐位无损**：double 列按**位模式**建字典（所以 -0.0 / NaN 也原样往返），
//      解码出来的 double 位模式必须与输入完全相同。
//   2. **编码器自检**：每次编码都先算候选、再解回来逐位比对，取"能用且最小"的那个；
//      任何输入最坏只是退回 Raw，**不会编错**。
//   3. **坏数据必须干净失败**：所有读取都带边界检查（截断、越界下标、未知 codec → false），
//      绝不静默给出半截数据。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "archive/AdocaoFormat.hpp"

namespace adofai {
namespace adocao {

// 一列的编码统计（`adocao pack --codec-report` 用它打印"每列 B/层"，体积判据的来源）
struct ColumnStats {
    const char* codec = "?";   // 实际选中的编码（带 fallback 说明）
    size_t rawBytes = 0;       // 朴素表示的字节数（double/int64 = 8 B/项，u32 = 4 B/项）
    size_t encodedBytes = 0;   // 编码后（含 ColumnHeader）
    size_t dictEntries = 0;
    int    bits = 0;
};

bool encodeDoubleColumn(const std::vector<double>& v, std::vector<uint8_t>& out, ColumnStats* st = nullptr);
bool decodeDoubleColumn(const uint8_t* p, size_t n, std::vector<double>& out);

bool encodeIntColumn(const std::vector<int64_t>& v, std::vector<uint8_t>& out, ColumnStats* st = nullptr);
bool decodeIntColumn(const uint8_t* p, size_t n, std::vector<int64_t>& out);

bool encodeU32Column(const std::vector<uint32_t>& v, std::vector<uint8_t>& out, ColumnStats* st = nullptr);
bool decodeU32Column(const uint8_t* p, size_t n, std::vector<uint32_t>& out);

}  // namespace adocao
}  // namespace adofai
