#include "core/level/LevelPath.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <vector>


namespace adofai {

namespace fs = std::filesystem;

namespace {

bool endsWithLower(const std::string& lower, const char* suffix) {
    const size_t l = std::strlen(suffix);
    return lower.size() >= l && lower.compare(lower.size() - l, l, suffix) == 0;
}

bool isLevelFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    std::string n = p.filename().string();
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return endsWithLower(n, ".adofai") || endsWithLower(n, ".adofai.xz")
        || endsWithLower(n, ".adofai.zst");
}

// descend=false：只看 dir 的直接子文件；true：再看一层子目录
std::vector<fs::path> levelFilesIn(const fs::path& dir, bool descend) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (isLevelFile(it->path())) out.push_back(it->path());
    }
    if (descend && out.empty()) {
        std::error_code ec2;
        for (fs::directory_iterator it(dir, ec2), end; it != end && !ec2; it.increment(ec2)) {
            std::error_code ec3;
            if (!fs::is_directory(it->path(), ec3)) continue;
            for (fs::directory_iterator sub(it->path(), ec3), send; sub != send && !ec3; sub.increment(ec3)) {
                if (isLevelFile(sub->path())) out.push_back(sub->path());
            }
        }
    }
    return out;
}

}  // namespace

std::vector<std::string> listLevelCandidates(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    std::error_code ec;
    const fs::path p(dir);
    if (!fs::is_directory(p, ec)) return out;
    for (const fs::path& f : levelFilesIn(p, true)) out.push_back(f.string());
    std::sort(out.begin(), out.end());
    return out;
}

std::string resolveLevelPath(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const fs::path p(path);
    if (!fs::is_directory(p, ec)) return path;              // 文件 / 符号链接目标 / 不存在
    const auto direct = levelFilesIn(p, false);
    if (direct.size() == 1) return direct.front().string();
    if (direct.empty()) {
        const auto nested = levelFilesIn(p, true);
        if (nested.size() == 1) return nested.front().string();
    }
    return path;                                            // 0 个或多个：不猜
}

}  // namespace adofai
