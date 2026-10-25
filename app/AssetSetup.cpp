#include "AssetSetup.hpp"

#include "core/util/AssetPaths.hpp"
#include "audio/HitsoundManager.hpp"

#include <string>
#include <vector>

#include "PlatformPaths.hpp"

namespace {

// 搜索根顺序；可执行文件目录由 PlatformPaths 统一实现。
}  // namespace

void configureAssetPaths() {
    std::vector<std::string> roots;
    const std::string exeDir = executableDirectory();

#ifdef _WIN32
    // Windows 上历史上优先找 exe 旁边（Finder/桌面双击启动时 CWD 不可靠）
    if (!exeDir.empty()) roots.push_back(exeDir);
    roots.push_back("");
#else
    // 命令行从仓库/构建根跑时优先 CWD
    roots.push_back("");
    if (!exeDir.empty()) roots.push_back(exeDir);
#endif
    if (!exeDir.empty()) {
        roots.push_back(exeDir + "/../Resources");   // macOS bundle 的标准布局
        auto dir = exeDir;
        for (int i = 0; i < 3 && !dir.empty(); i++) {
            const auto slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) { dir.clear(); break; }
            dir = dir.substr(0, slash);
        }
        if (!dir.empty()) roots.push_back(dir);
    }

    adofai::AssetOptions opts;
    opts.roots = std::move(roots);
    opts.packName = "ADOCAO-data.tar.xz";
    opts.dataDir = "ADOCAO-data";
    adofai::setAssetOptions(std::move(opts));

    adofai::HitsoundManager::setDefaultHitsoundSubdir("assets/hitsounds");
}
