#pragma once

#include <string>

// Clean ADOFAI JSON (fix missing commas, trailing commas, double commas)
//
// ⚠️ 这个函数是**逐字符复现旧编辑器/旧版本产出的容错行为**，不是"通用 JSON 修复器"：
// 两遍状态机（补逗号 → 去重复逗号）的交互别按直觉改。改动前先跑 `ctest -R level_parse`
// 与 `scripts/check-fast-path.sh` —— 快路径（`tryFastParse`）必须与它逐字节等价。

namespace adofai {

std::string cleanJson(const std::string& raw);

}  // namespace adofai
