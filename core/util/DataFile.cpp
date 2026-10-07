#include "DataFile.hpp"
#include "AssetPaths.hpp"
#include "Logger.hpp"

#ifdef ADOCAO_HAVE_MINIZ
#  include "miniz.h"
#endif

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

    // 1) 配了 zip 就先在 zip 里找。zip 自己**也按搜索根找**（旧行为只认当前目录，
    //    于是从别的目录启动就找不到 exe 旁边的 ADOCAO-data.zip）。
#ifdef ADOCAO_HAVE_MINIZ
    if (!opts.zipName.empty()) {
        const std::string zipPath = resolveAsset(opts.zipName);
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
            size_t size = 0;
            void* data = mz_zip_reader_extract_file_to_heap(&zip, relativePath.c_str(), &size, 0);
            mz_zip_reader_end(&zip);
            if (data) {
                std::vector<uint8_t> result((uint8_t*)data, (uint8_t*)data + size);
                mz_free(data);
                LOG_D("DataFile: %s ← %s (%zu bytes)", relativePath.c_str(), zipPath.c_str(), size);
                return result;
            }
        }
    }
#else
    // 这个构建没编 miniz：跳过 zip 那条路，直接走 dataDir / 搜索根 / 直接路径
    // （行为与调用方把 zipName 留空完全一致）。
#endif

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

    LOG_D("DataFile: %s 没找到（zip=%s dataDir=%s roots=%zu）", relativePath.c_str(),
          opts.zipName.empty() ? "(未配置)" : opts.zipName.c_str(),
          opts.dataDir.empty() ? "(未配置)" : opts.dataDir.c_str(), opts.roots.size());
    return {};
}

}  // namespace adofai
