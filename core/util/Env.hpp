#pragma once
// ADOCAO_* 环境变量：布尔开关的**唯一**解析口径。
//
// 2026-10-18 清点：全仓 39 个 ADOCAO_* 钩子，布尔判断原本有四种写法 ——
//   `getenv() != nullptr`（设了就开，`=0` 也开）、`atoi(e) != 0`、`atoi(e) == 1`、
//   首字符 `e[0] != '0'`。
// 于是同一句 "=0" 在不同变量上含义相反（`ADOCAO_CAM_DEBUG=0` 开日志、
// `ADOCAO_UNCAPPED=0` 反而关掉 320fps 节流）。现在布尔一律走 flag()：
//   未设置 / 空 / "0" / "false" / "no" / "off" → false
//   其它任何值（"1" / "true" / "yes" …）      → true
// 数字、路径、枚举（`ADOCAO_CULL=scan` 之类）仍用 getenv() + atoi/atof/atoll ——
// 它们本来就要解析值，不是"开/关"。
//
// 规则：新增环境变量必须登记到 docs/env.md（scripts/check-env-docs.sh 会查漏）。

#include <cstdlib>
#include <cstring>

namespace adofai {
namespace env {

inline bool flag(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
           std::strcmp(v, "no") != 0 && std::strcmp(v, "off") != 0;
}

}  // namespace env
}  // namespace adofai
