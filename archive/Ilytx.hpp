#pragma once

#include <cstddef>
#include <string>

// `.ilytx`（Re_ADOJAS 的自研格式）= **xz( tar( manifest.json, level.ilybin, audio/…, decor/… ) )**。
// 规范摘要见 `docs/ilytx.md`；权威实现是 `~/Repos/Re_ADOJAS/src/lib/ilytx/`。
//
// 我们**不做**一条平行的 LevelData 填充路径（那会变成"两套解析各说一套"）：这里只把
// `.ilytx` **翻译成等价的 `.adofai` 明文 JSON**（{angleData, settings…, actions, decorations}），
// 之后完全复用 core 原有的明文解析 + finishLoad —— 于是 ilytx 与 .adofai 的语义、像素、
// 对拍断言天然一致（tests/ilytx_test.cpp 就是拿参照谱逐位比）。
//
// 代价（要认）：2³¹−1 层那种规模上，"翻译成文本"会多一份 ~15 B/层的中间字符串；
// 这种体量请先用 `adocao pack` 转成列式容器（见 docs/adocao-format.md）。
namespace adofai {
namespace archive {

// `tar` = **已解压**的 tar 字节。成功时 `out` = 等价的 .adofai JSON 文本。
// 失败一律 false + reason：找不到 level.ilybin / tar 头坏 / 成员越界 / ilybin 坏 /
// settings 不是 JSON 对象 / direction 里有非有限值。
bool ilytxToAdofaiJson(const char* tar, size_t length, std::string& out, std::string& reason);

}  // namespace archive
}  // namespace adofai
