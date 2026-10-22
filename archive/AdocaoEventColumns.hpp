#pragma once
// `.adocao` v7 的**事件列**：把"一堆异构 JSON 事件"按 schema 拆成"每字段一列"。
//
// 布局（同样适用于 actions 与 decorations，靠 schema 的 source 区分）：
//   * `typeIds[event]`           每个事件属于哪个类型（按原顺序，不排序、不搬家）；
//   * 每字段一列                present 位图（该类型的第 k 个事件写没写）+ null 位图（出现才写）
//                               + payload（只有"写了且非 null"的值，按类型内顺序）；
//   * 全局字符串字典             Enum/Tag/String 共用（下标进 payload 的 u32 列）。
//
// 值的物理编码复用 `AdocaoColumns` 的三支编码器（u32/int64/double），所以字典/差分/位打包/
// 自检/坏数据失败这些既有保证直接继承。Pair 的分量按 double 存（逐位无损）+ 分量 null 位；
// Nested/RawJSON 存原始 JSON 文本（长度前缀）。
#include <cstdint>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "archive/AdocaoKinds.hpp"
#include "archive/AdocaoSchema.hpp"

namespace adofai {
namespace adocao {

struct ColumnBlob {
    std::vector<uint8_t> present;   // 位图：类型内第 k 个事件是否写了该字段
    std::vector<uint8_t> nulls;     // 位图：写了但为 null（空 = 没有 null）
    std::vector<uint8_t> payload;   // 编码后的值列（只含"写了且非 null"的值）
    uint32_t valueCount = 0;
    uint8_t  subFlags = 0;          // Pair 用：bit0 分量0有 null、bit1 分量1有 null；
                                    // Color 用：bit2 = 8 位（含 alpha）、bit3 = 带 '#'
    std::vector<uint8_t> nulls0, nulls1;   // Pair 的分量 null 位图（出现才写）
    std::vector<std::string> dictStrings;  // 该列若是字符串类（Enum/Tag/String）则带字典
};

struct EventColumns {
    std::vector<uint16_t> typeIds;      // 与输入事件一一对应（原顺序）
    std::vector<ColumnBlob> fields;     // 与 schema.fields 平行
};

// 建列。`events` 与 `sources` 一一对应（同一批事件的来源）。
bool buildEventColumns(const Schema& schema,
                       const std::vector<std::string>& eventsJson,
                       const std::vector<uint8_t>& source,   // EventSource
                       EventColumns& out, std::string& err);

// 解列：还原成每个事件的 JSON 文本（字段顺序 = schema 顺序，即"首次出现顺序"）。
bool decodeEventColumns(const Schema& schema, const EventColumns& cols, size_t eventCount,
                        std::vector<std::string>& eventsJson, std::string& err);

}  // namespace adocao
}  // namespace adofai
