#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// `ilybin` — Re_ADOJAS 的 `.ilytx` 里的**谱面核心二进制**（他们那边叫 v1，见
// `~/Repos/Re_ADOJAS/src/lib/ilytx/ilybin.ts`；我们的规范摘要见 `docs/ilytx.md`）。
//
// 我们**只解码、不编码**：需求是"能打开别人导出的谱面"。编码是 ADOCAO 自己的 `.adocao`
// （列式 + 按列选编码，见 `docs/adocao-format.md`）。
//
// 与 `.adofai` 的对应关系（这正是能无损接回明文路径的原因）：
//   section 1 settings  → 文件里除 angleData/actions/decorations 以外的**顶层对象**（原样文本）
//   section 2 direction → `angleData` 的**值**（含 999 中心块与 555/666/777/888 特殊砖标记）
//   section 6 actions   → `actions` 数组（每个对象带 floor）
//   section 7 decorations → `decorations` 数组
//   section 3 angle     → 相对角，纯派生量，**我们不要**（core 自己按 angleData 重算；
//                          v2 起他们也不再写这一段）
//   section 5 twirl     → 累计 Twirl 计数，同样是派生量 —— 语义权威是 actions 里的 Twirl 事件
//   section 8 extraProps→ 逐层附加属性（ADOCAO 目前没有对应概念，忽略并在文档里记着）

namespace adofai {
namespace archive {

struct IlybinLevel {
    uint8_t  tileMode = 0;        // 0 = 对象模式（f64 源）/ 1 = 紧凑模式（f32 源）
    std::string settingsJson;     // 顶层 settings 对象，原样文本
    std::vector<double> direction;    // 长度 == tileCount
    std::string actionsJson;      // 空 = 没有这一段（= 没有事件）
    std::string decorationsJson;  // 空 = 没有这一段
};

// 解码 ilybin。失败一律返回 false 并在 reason 里给原因 —— **绝不**静默产出半份谱面：
// magic 不对 / 版本不认识 / 段头或段数据越界 / 缺必需段（settings·direction·twirl）/
// 未知浮点编码 kind。tileCount 上限按 2^31−1 的目标卡（int64 计数，见 CountLimits.hpp）。
bool decodeIlybin(const uint8_t* data, size_t length, IlybinLevel& out, std::string& reason);

}  // namespace archive
}  // namespace adofai
