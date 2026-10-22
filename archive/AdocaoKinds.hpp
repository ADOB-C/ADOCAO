#pragma once
// `.adocao` v7 的"值种类"（kind）与三态规则。
//
// v7 的目标：容器不再为每类事件手写定长记录，而是**按 kind 分列**存任何事件的任何字段。
// kind 决定了用哪种变换（字典 / 差分 / 网格整型 / 池化 / 递归），也决定了扩展性的边界：
//
//   * 新增**事件类型/字段** → 不改格式、不 bump version（schema 自描述，见 AdocaoSchema）；
//   * 新增**kind**（新的物理表示）→ 必须 bump version，并在这里登记。
//
// 三态（2026-10-22 由 202x 谱面实测逼出来的）：`null` 与"字段缺失"语义不同
// （ADOFAI：`positionOffset:[null,null]` = 该通道不动；字段没写 = 用默认值/继承），
// 所以每列除了值本身，还要能区分 Missing / Null / Value。存法：
//   presence 位图（1 = 写了）+ null 位图（1 = 写了但为 null，仅当该列出现过 null 时才写）。
#include <cstdint>
#include <string_view>

namespace adofai {
namespace adocao {

enum class Kind : uint8_t {
    Bool = 0,       // true/false → 位图
    Int,            // 整数（floor/ticks/count…）→ delta/varint 或 dict
    Float,          // 非网格浮点 → 原始位模式（逐位无损）
    GridFloat,      // 十进制网格上的浮点 → 存整数（v*10^k），逐位可逆
    Enum,           // 短字符串、取值少（ease/relativeTo/plane…）→ 字典
    Color,          // 6 或 8 位 hex（RRGGBB / RRGGBBAA）→ u32
    String,         // 一般字符串（路径/注释…）→ 池
    Tag,            // 标识符串（tag/eventTag）→ 池 + front-coding 友好
    Pair,           // 二维数组 [a, b] → 拆两列 + 相等位
    Nested,         // JSON 对象 → 递归子 schema
    RawJSON,        // 兜底：原样字节（长度前缀）
};

// 字符串长度上限（超过一律 String/ RawJSON，不进枚举判定）
inline constexpr size_t kMaxEnumLen = 24;
// 一列里不同取值不超过它就允许 Enum（超过仍可 Dict，但那是编码器的选择，不是 kind）
inline constexpr uint32_t kMaxEnumDistinct = 64;

// 单个值的三态结果。
enum class Presence : uint8_t { Missing = 0, Null = 1, Value = 2 };

// 把一个 JSON 值（只有 pack 期会调用，性能不敏感）映射成 kind。
// `allSameKind` 之类"整列统计"由调用方收集后决定（例如是否 GridFloat），
// 这里只给"单值最具体"的判定：
//   bool → Bool；整数 → Int；浮点 → Float；"#hex" 或纯 hex → Color；对象 → Nested；
//   二维数组 → Pair；其余数组 → RawJSON；字符串 → Enum/Tag/String（按长度与形态）。
Kind kindOfScalar(const char* s, size_t n);

// 字符串是否像标识符（用于 Tag 判定：无空格、无引号、长度 ≤ 64）。
bool looksLikeTag(std::string_view v);

// 字符串是否是 6 或 8 位十六进制（大小写都认）+ 可选 '#' 前缀。
bool isHexColor(std::string_view v);

}  // namespace adocao
}  // namespace adofai
