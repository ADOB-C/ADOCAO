#pragma once

// `.adocao` 写入方（pack）：把**已加载**的 LevelData 打成 `.adocao` 字节流。
//
// 分层：archive 依赖 core（容器实现方），所以这里可以引用 LevelData —— core 侧则永远
// 不知道 `.adocao` 的存在（依赖倒置，见 AGENTS 的 core/archive 一节）。
//
// 关键约定：
//   * **只存输入，不存派生数据**：Tile::position / direction / tileBPMs / Timeline 全部由
//     angleData + actions 重算（position 是全局前缀和），所以这里压根不写它们。
//   * **字符串池原样保留 actionStrTable 的顺序**（strId 是它的下标）：只在末尾追加 settings
//     用到的字符串，于是解码方拿到逐位相同的 id 空间。
//   * **确定性**：同一份输入必须打出逐字节相同的文件（settings 字段按固定顺序入池）——
//     `adocao_roundtrip` 会断言这一点。

#include <cstdint>
#include <string>
#include <vector>

namespace adofai {

struct LevelData;

namespace adocao {

// 一列的体积报告（`adocao pack --codec-report` 打印它；判据"每段 B/层"就从这里来）
struct ColumnReport {
    std::string name;        // angleData / actions.floor / …
    std::string codec;       // 实际选中的编码
    uint64_t elements = 0;
    uint64_t rawBytes = 0;      // 朴素表示
    uint64_t encodedBytes = 0;  // 列头 + 载荷
    uint64_t dictEntries = 0;
    int bits = 0;
    // —— v3 分块（§8.3）：单块与分块**各自的实测存储字节**，用来量"分块赚了多少" ——
    bool     blocked = false;
    bool     forced = false;      // true = 触发了"强制分块"（裸数据 ≥ 阈值，窗口化的要求）
    uint64_t blocks = 0;
    uint64_t frames = 0;
    uint64_t singleBytes = 0;
    uint64_t blockedBytes = 0;
};

struct PackResult {
    uint64_t fileBytes = 0;
    uint64_t checkpoints = 0;       // `Derived` 段的记录条数（= 每 4096 层一条）
    uint64_t checkpointBytes = 0;   // 落盘字节（段级压缩后）
    std::vector<ColumnReport> columns;
};

// 是否写 `Derived` 段（每 4096 层的续算快照，§8.8 ⑧）。**默认关**：它天生是 O(n/4096) 字节
// （2³¹ 层 ≈ 19.4 MB），写进去就不再是"v3 文件大小 == v2"。要 ③ 的 O(1) seek / 窗口化就打开它。
struct PackOptions {
    bool withCheckpoints = false;
};

// 打包。out 是完整的文件字节（含头与段目录）；失败时 err 里是原因。
bool packLevel(const LevelData& level, std::vector<uint8_t>& out, std::string& err,
               PackResult* result = nullptr, const PackOptions& opts = PackOptions{});

}  // namespace adocao
}  // namespace adofai
