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
#include <deque>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "archive/AdocaoKinds.hpp"
#include "archive/AdocaoSchema.hpp"
#include "core/level/LevelData.hpp"   // EventFieldValue（v7 → LevelData 直灌）

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
    // 分块容器用：字典串已进**段级共享池**，这里只留"本地下标 → 全局池下标"。
    // 置位时 serialization 不再写字符串本身（块间重复的图名/tag 只存一次）。
    std::vector<uint32_t> dictGlobalIds;
    static constexpr uint8_t kDictGlobal = 0x01;   // subFlags 位

};

struct EventColumns {
    std::vector<uint16_t> typeIds;      // 与输入事件一一对应（原顺序）
    std::vector<ColumnBlob> fields;     // 与 schema.fields 平行
    // 全局字符串池：序列化时跨列去重（同一张图/同一个 tag 只存一次），反序列化时填回。
    std::vector<std::string> globalStrings;
};

// 建列。`events` 与 `sources` 一一对应（同一批事件的来源）。
bool buildEventColumns(const Schema& schema,
                       const std::vector<std::string>& eventsJson,
                       const std::vector<uint8_t>& source,   // EventSource
                       EventColumns& out, std::string& err);

// 解列：还原成每个事件的 JSON 文本（字段顺序 = schema 顺序，即"首次出现顺序"）。
// 结构化字段的**扁平批**（offsets[e]..offsets[e+1] 是第 e 个事件的字段）。
// 用扁平数组而不是"每事件一个 vector"：11k 事件就是 11k 次小分配，实测占掉可观时间。
struct FieldBatch {
    std::vector<LevelData::EventFieldValue> values;
    std::vector<uint32_t> offsets;   // size = eventCount + 1
    // 颜色等"现算出来的字符串"必须存在这里：`EventFieldValue::str` 只有裸指针，
    // 早先用 thread_local 缓冲复用 → 后面的事件把前一个的颜色覆盖掉（语料回归抓到的）。
    std::deque<std::string> strings;
};

// `outBatch` 非空时**同时**产出结构化字段（v7 → LevelData 直灌用，不再拼 JSON 文本）；
// `wantJson=false` 且给了 `outBatch` 时连 JSON 都不生成。
bool decodeEventColumns(const Schema& schema, const EventColumns& cols, size_t eventCount,
                        std::vector<std::string>& eventsJson, std::string& err,
                        FieldBatch* outBatch = nullptr, bool wantJson = true);

// 列集的字节化（容器段用；也是"体积报告"的口径）。不含 schema —— schema 单独一段。
std::vector<uint8_t> serializeEventColumns(const Schema& schema, const EventColumns& cols);
bool deserializeEventColumns(const Schema& schema, const uint8_t* p, size_t n, EventColumns& out,
                             std::string& err,
                             const std::vector<std::string>* pool = nullptr);

// **按事件分块**（供窗口化 seek）：每 `blockEvents` 个事件一块，块内自包含（各自带字典/位图），
// 因此解一个窗口只需要解命中的那几块。段头存块表（offsets 相对段起点）。
std::vector<uint8_t> serializeEventColumnsBlocked(const Schema& schema,
                                                  const std::vector<std::string>& eventsJson,
                                                  const std::vector<uint8_t>& source,
                                                  uint32_t blockEvents, size_t* outBlocks = nullptr);
// 解出 `[firstEvent, lastEvent)` 的事件（只需解命中的块）。`outFirst` = 第一块的起始事件下标。
bool decodeEventColumnsRange(const Schema& schema, const uint8_t* p, size_t n,
                             uint32_t firstEvent, uint32_t lastEvent,
                             std::vector<std::string>& outEvents, std::string& err,
                             uint32_t* outBlocks = nullptr, std::vector<uint16_t>* outTypeIds = nullptr);
// 解全部（等价于解所有块再拼接）。
// 结构化版本（逐块回调，**不缓存全部事件**）：每解完一个事件就回调一次，
// 这样整个加载只需要一块的 `FieldBatch` 内存，也不会给每个事件建 vector。
using EventFieldsFn = bool (*)(void* ctx, const LevelData::EventFieldValue* v, size_t n,
                               uint16_t typeId, std::string& err);
bool decodeEventFieldsBlockedEach(const Schema& schema, const uint8_t* p, size_t n,
                                  EventFieldsFn fn, void* ctx, std::string& err,
                                  uint32_t firstEvent = 0, uint32_t lastEvent = 0xFFFFFFFFu);
bool decodeEventColumnsBlockedAll(const Schema& schema, const uint8_t* p, size_t n,
                                  std::vector<std::string>& outEvents, std::string& err,
                                  std::vector<uint16_t>* outTypeIds = nullptr);

// **按层号窗口解事件**（2e9 的 seek 落点）：用 `LevelData::eventBlockStart`（EventIndex）把
// `floor ∈ [floorLo, floorHi)` 映射成事件下标区间，只解覆盖的块；块边界是近似的，
// 所以解出来后再按每条事件的 `floor` 精确过滤。索引缺失/非升序时退回"全解再过滤"。
bool decodeEventsInFloorRange(const LevelData& lv, int64_t floorLo, int64_t floorHi,
                              std::vector<std::string>& outEvents, std::string& err);

// 诊断：整段 zstd-19 之后的字节数（报告用；真正写文件时由段级压缩决定）。
size_t zstdEstimate(const std::vector<uint8_t>& raw);

}  // namespace adocao
}  // namespace adofai
