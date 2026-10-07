#pragma once

// `.adocao` 读取方：解到**已存在**的 LevelData（覆盖写）。
//
// 注意分工：这里**不做收尾** —— tiles / 位置 / direction / tileBPMs / processActions 全部由调用方
// 的 `LevelData::finishLoad()` 负责，而那段收尾与两条 JSON 路径**共用同一份代码**。
// 这是"逐位对拍"能成立的前提：三条路径（旧 DOM / 快路径 / .adocao）产出的是同一个 LevelData，
// 再喂给同一个收尾。

#include <cstddef>
#include <cstdint>
#include <string>

namespace adofai {

struct LevelData;

namespace adocao {

// data/length 是整个 `.adocao` 文件。失败时 err 是原因（**明确失败，绝不静默回退**）。
bool unpackLevel(const uint8_t* data, size_t length, LevelData& out, std::string& err);

}  // namespace adocao
}  // namespace adofai
