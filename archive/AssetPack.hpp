#pragma once

// 资产包：`ADOCAO-data.tar.xz`（xz 压缩的 tar，条目名就是 `assets/shaders/...`）。
//
// 为什么是 tar.xz 而不是 zip（2026-10-25）：
//   * zip 需要 miniz 这个独立依赖；`.xz` 的解码器（liblzma）**已经是 archive 模块的私有依赖**
//     （`.adofai.xz` / `.ilytx` 都用它），所以换过来净少一个第三方库；
//   * tar 是标准容器，`tar -cJf` 一条命令就能生成，不需要我们自己写打包器。
//
// 解压与索引都在第一次读取时做一次（本机实测 4.4 MB 资产 → 解压 ~3 MB 常驻），之后只查表。

#include <cstdint>
#include <vector>

namespace adofai {
namespace archive {

// 从资产包里读一个条目（名字如 "assets/shaders/tile.vert"）。
// 返回 false = 没有包 / 包里没有这个条目 / 解压失败（失败原因只在日志里说一次）。
bool readAssetFromPack(const char* name, std::vector<uint8_t>& out);

}  // namespace archive
}  // namespace adofai
