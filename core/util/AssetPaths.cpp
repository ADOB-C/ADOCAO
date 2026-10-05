#include "AssetPaths.hpp"

#include <filesystem>
#include <system_error>

namespace adofai {

namespace {

AssetOptions g_opts;   // 启动期单线程设置，之后只读（真要多线程改，外面加锁）

std::string join(const std::string& root, const std::string& rel) {
    return root.empty() ? rel : root + "/" + rel;
}

std::string resolve(const std::string& relative, bool wantDir) {
    std::error_code ec;
    for (const auto& root : g_opts.roots) {
        const std::string candidate = join(root, relative);
        if (wantDir ? std::filesystem::is_directory(candidate, ec)
                    : std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return relative;   // 与旧行为一致：找不到就原样返回，让调用方自己去失败
}

}  // namespace

const AssetOptions& assetOptions() { return g_opts; }

void setAssetOptions(AssetOptions opts) {
    if (opts.roots.empty()) opts.roots.push_back("");
    g_opts = std::move(opts);
}

std::string resolveAsset(const std::string& relative) { return resolve(relative, false); }
std::string resolveAssetDir(const std::string& relative) { return resolve(relative, true); }

}  // namespace adofai
