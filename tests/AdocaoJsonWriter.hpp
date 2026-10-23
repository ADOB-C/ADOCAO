#pragma once
// `.adocao` → `.adofai` 明文回写。**只存在于 tests/**：ADOCAO 不提供导出功能，
// 这个回写器是"容器无损"的验收工具（容器 → 明文 → 重新打包 → 逐字节相同，见
// `adocao_v7_roundtrip_json_test.cpp`）。对外部实现者的规范见 `docs/adocao-export.md`。
//
// 只依赖 v7 的 `EventSchema`/`EventColumns`（`v7IncludeLegacy` 的包才有全部事件），
// 加上 settings / angleData / pathData。没有 v7 段的旧文件会**明确报错**，不猜、不静默残缺。
#include <string>

#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {

bool writeLevelJson(const LevelData& lv, std::string& out, std::string& err);

}  // namespace adocao
}  // namespace adofai
