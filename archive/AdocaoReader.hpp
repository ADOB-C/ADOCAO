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
#include <vector>

#include "archive/AdocaoCheckpoints.hpp"

namespace adofai {

struct LevelData;

namespace adocao {

// `Derived` 段（§8.8 的 checkpoint）解析出来的东西。**可选**：没有这一段时 checkpoints 为空。
// 现在还没有消费方（③ 才用），但"读得回来 + 能对上"必须先立住，否则写出去的就是没人验的空壳。
struct DerivedInfo {
    uint32_t stride = 0;
    std::vector<FloorCheckpoint> checkpoints;
    // 按层索引（`ActionIndex` 段，③）：`firstAction[b]` = 第一个 floor >= b*stride 的 action 下标。
    // 长度 = 块数 + 1（最后一条是哨兵 = actions.size()），所以第 b 块的事件区间是
    // `[firstAction[b], firstAction[b+1])`。写入方保证 actions 按 floor 非递减，否则这一段不写。
    uint32_t actionStride = 0;
    std::vector<ActionIndexEntry> firstAction;
};

// 单个段的**载荷视图**（③ 的窗口化加载按块取数据要用它：不该为了一个块把整份谱所有段都摊开）。
// `storage` 非空时表示这一段是段级压缩的，`p` 指向 `storage`（调用方要让它活着）；
// 否则 `p` 直接指向传入的文件字节（调用方必须让那份缓冲活着）。
struct SectionView {
    const uint8_t* p = nullptr;
    size_t size = 0;
    uint16_t flags = 0;
    std::string storage;
};

// 取某一段的载荷（校验头/表/CRC 与这一段自己的 CRC；段级压缩在这里解开）。
bool getSectionView(const uint8_t* data, size_t length, uint8_t sectionId,
                    SectionView& out, std::string& err);

// data/length 是整个 `.adocao` 文件。失败时 err 是原因（**明确失败，绝不静默回退**）。
// derived 非空时顺带把 `Derived` 段解出来（没有该段则保持为空，不算错误）。
bool unpackLevel(const uint8_t* data, size_t length, LevelData& out, std::string& err,
                 DerivedInfo* derived = nullptr);

}  // namespace adocao
}  // namespace adofai
