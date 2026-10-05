#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adofai {

// 资产解析：一次配置、到处用。
//
// **默认值与具体产品无关**：只有一个"相对当前目录"的搜索根，不找 zip、不找 data 目录。
// 产品在启动时用 setAssetOptions() 把它自己那套设进来（例如 ADOCAO 设 zip = "ADOCAO-data.zip"、
// dataDir = "ADOCAO-data"、roots = exe 目录 + ../Resources（macOS bundle）+ 上溯 3 级）。
//
// "可执行文件在哪"这段平台代码**不在库里**：core 禁平台头（scripts/check-core-purity.sh），
// 所以由调用方算好根列表传进来（ADOCAO 那边是 app/AssetSetup.cpp 干的）。
struct AssetOptions {
    std::vector<std::string> roots{""};  // 依次尝试的目录前缀；"" = 相对当前目录
    std::string zipName;                 // 非空：先在名为它的 zip 里找
    std::string dataDir;                 // 非空：再在 <dataDir>/<相对路径> 找
};

const AssetOptions& assetOptions();
void setAssetOptions(AssetOptions opts);

// 返回第一个存在的候选；都找不到就原样返回 relative（与旧行为一致）
std::string resolveAsset(const std::string& relative);
std::string resolveAssetDir(const std::string& relative);

// 资产读取：zip（配了的话）→ dataDir → 各搜索根 → 直接按相对路径。找不到返回空。
std::vector<uint8_t> readDataFile(const std::string& relativePath);

}  // namespace adofai
