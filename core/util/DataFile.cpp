#include "DataFile.hpp"
#include "AssetPaths.hpp"
#include "Logger.hpp"
#include "core/level/ByteSource.hpp"   // 资产包读取钩子（实现在 archive 模块）

#include <fstream>
#include <cstring>


namespace adofai {

namespace {

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> result(size);
    f.read((char*)result.data(), (std::streamsize)size);
    return result;
}

}  // namespace

std::vector<uint8_t> readDataFile(const std::string& relativePath) {
    const AssetOptions& opts = assetOptions();

    // 1) 资产包（`ADOCAO-data.tar.xz`）优先。解压与 tar 索引在 **archive 模块**里
    //    （core 不依赖 liblzma），这里只调钩子；没注册后端/没有包就是一次空转。
    //    包自己**也按搜索根找**，所以从别的目录启动也能找到 exe 旁边的包。
    if (!opts.packName.empty()) {
        if (const auto& be = archiveBackend(); be.readAssetPack) {
            std::vector<uint8_t> result;
            if (be.readAssetPack(relativePath.c_str(), result) && !result.empty()) {
                LOG_D("DataFile: %s ← %s (%zu bytes)", relativePath.c_str(), opts.packName.c_str(),
                      result.size());
                return result;
            }
        }
    }

    // 2) <dataDir>/<相对路径>（也按搜索根找）
    if (!opts.dataDir.empty()) {
        const std::string path = resolveAsset(opts.dataDir + "/" + relativePath);
        if (auto data = readWholeFile(path); !data.empty()) {
            LOG_D("DataFile: %s ← %s (%zu bytes)", relativePath.c_str(), path.c_str(), data.size());
            return data;
        }
    }

    // 3) 各搜索根下的相对路径（roots 里通常含 ""，即当前目录）
    const std::string path = resolveAsset(relativePath);
    if (auto data = readWholeFile(path); !data.empty()) return data;

    LOG_D("DataFile: %s 没找到（pack=%s dataDir=%s roots=%zu）", relativePath.c_str(),
          opts.packName.empty() ? "(未配置)" : opts.packName.c_str(),
          opts.dataDir.empty() ? "(未配置)" : opts.dataDir.c_str(), opts.roots.size());
    return {};
}

}  // namespace adofai
