#pragma once
// `.adocao` v7 的**自描述事件 schema**：类型表 + 字段表。
//
// 设计目标（docs/adocao-format.md §9）：
//   * 打包时从**实际 JSON** 自动发现类型与字段，不给 33 类事件手写登记表；
//   * 新增事件类型/字段不需要改容器代码、不需要 bump version；
//   * kind（值的种类）由整列统计决定（Enum/Color/Int/GridFloat/Pair…），编码器按 kind 选变换；
//   * 三态：presence（写了没）+ null（写了但为 null）+ 值，三者分开存。
//
// 本模块只做"发现 + 描述 + 序列化"，不碰列数据（列在 AdocaoEventColumns）。
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "archive/AdocaoKinds.hpp"

#include <rapidjson/document.h>   // pack 期才用到；保持值类型的完整定义，避免前置声明的坑

namespace adofai {
struct LevelData;   // 容器侧只读原始成员，不拥有它
namespace adocao {

// 事件来源：同一个类型名可能既出现在 `actions` 也出现在 `decorations`
// （2021 的 AddDecoration 在 actions、2025 的在 decorations —— 必须分开记，否则回写会搬家）。
enum class EventSource : uint8_t { Actions = 0, Decorations = 1 };

struct FieldDesc {
    uint16_t nameId = 0;      // schema 名字表下标
    Kind     kind = Kind::RawJSON;
    uint8_t  flags = 0;       // bit0: 该列出现过 null；bit1: 该列有缺失；bit2: 全为 null
    uint8_t  gridK = 0;       // GridFloat 的 10^k（k=0 表示整数）
    uint32_t columnId = 0;    // 列在 EventColumns 里的下标（写入方填）
};

struct TypeDesc {
    uint16_t nameId = 0;
    uint8_t  source = 0;      // EventSource
    uint16_t firstField = 0;
    uint16_t fieldCount = 0;
    uint32_t eventCount = 0;
};

struct Schema {
    std::vector<std::string> names;   // [0] = ""
    std::vector<TypeDesc>    types;
    std::vector<FieldDesc>   fields;

    const std::string& name(uint16_t id) const {
        return id < names.size() ? names[id] : names[0];
    }
};

// 打包期构建器。只接受 pack 路径调用（每张谱一次，性能不敏感）。
class SchemaBuilder {
public:
    SchemaBuilder();
    ~SchemaBuilder();   // Acc 在 .cpp 里定义（pimpl 式的轻量做法）

    // 追加一个事件对象。`obj` 必须是 JSON 对象（RapidJSON）。
    void addEvent(std::string_view typeName, EventSource source,
                  const rapidjson::Value& obj);

    // 收尾：按整列统计决定 kind（Enum/Color/GridFloat…），返回只读 schema。
    const Schema& finalize();

    // 序列化（自描述、可独立解回）：先名字表，再类型表，再字段表。
    std::vector<uint8_t> serialize() const;

    // 反序列化；失败必须显式返回 false（不猜、不静默回退）。
    static bool deserialize(const uint8_t* p, size_t n, Schema& out, std::string& err);

    const Schema& schema() const { return m_schema; }

private:
    struct Acc;   // 每字段的整列统计
    uint16_t internName(std::string_view s);
    uint32_t ensureField(uint16_t typeIdx, uint16_t nameId);

    Schema m_schema;
    std::vector<std::unique_ptr<Acc>> m_acc;
    std::vector<std::pair<uint64_t, uint32_t>> m_fieldIndex;   // (type,name) → field idx（线性查，字段少）
    std::vector<int32_t> m_typeIndex;                          // source*N + typeNameId → type idx
    bool m_finalized = false;
};

// 从 `LevelData` 的原始成员（`preserveRawJson` 捕获的切片）建 schema。
// 失败 = 某条事件解析不了 → 显式报错（绝不静默跳过）。
bool buildSchemaFromLevel(::adofai::LevelData& lv, SchemaBuilder& sb, std::string& err);

// 诊断摘要：类型数/字段数/kind 分布/rawJSON 字节占比（`adocao pack --codec-report` 用）。
std::string schemaSummary(const Schema& s);

}  // namespace adocao
}  // namespace adofai
