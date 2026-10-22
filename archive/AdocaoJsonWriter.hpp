#pragma once
// `.adocao` → `.adofai` 明文回写（ADOCAO-E 的无损回写基础）。
//
// 只依赖 v7 的 `EventSchema`/`EventColumns`（`pack --v7-all` 会**不去重**地写入全部事件），
// 加上 settings / angleData / pathData。没有 v7 段的旧文件会**明确报错**，不猜、不静默残缺。
#include <string>

#include "core/level/LevelData.hpp"

namespace adofai {
namespace adocao {

bool writeLevelJson(const LevelData& lv, std::string& out, std::string& err);

}  // namespace adocao
}  // namespace adofai
