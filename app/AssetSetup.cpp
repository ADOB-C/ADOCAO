#include "AssetSetup.hpp"

#include "core/util/AssetPaths.hpp"
#include "audio/HitsoundManager.hpp"

#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace {

// 可执行文件所在目录（平台代码留在 app：core 禁平台头）。macOS 上会先 realpath，
// 否则通过符号链接启动（/Applications/ADOCAO.app → repo/build/ADOCAO.app）时
// 下面每个搜索根都会指到真实 bundle 之外。
std::string executableDirectory() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::string dir(buf.data());
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved)) dir = resolved;
    return dir;
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    std::string dir(buf);
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir;
#else
    return {};
#endif
}

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
    opts.zipName = "ADOCAO-data.zip";
    opts.dataDir = "ADOCAO-data";
    adofai::setAssetOptions(std::move(opts));

    adofai::HitsoundManager::setDefaultHitsoundSubdir("assets/hitsounds");
}
