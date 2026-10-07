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
        || endsWithLower(n, ".adofai.zst") || endsWithLower(n, ".adocao");
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

    // 扩展名被落下时补一次（macOS 的 Finder 默认**隐藏扩展名**，用户手打/粘贴时看到的就是
    // `.../primer`，而 `.adocao` / `.adofai` 都真的在后面）。原则与下面一致：**唯一命中才生效、
    // 绝不猜** —— 0 个或多个命中都原样返回，让调用方报错。
    if (!fs::exists(p, ec)) {
        std::string n = p.filename().string();
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const char* kExts[] = {".adofai", ".adofai.xz", ".adofai.zst", ".adocao"};
        bool hasKnownExt = false;
        for (const char* e : kExts)
            if (endsWithLower(n, e)) { hasKnownExt = true; break; }
        if (!hasKnownExt) {
            std::vector<fs::path> hits;
            for (const char* e : kExts) {
                fs::path cand = p;
                cand += e;
                if (fs::is_regular_file(cand, ec)) hits.push_back(cand);
            }
            if (hits.size() == 1) return hits.front().string();
        }
        return path;                                        // 不存在又补不出唯一解：原样返回
    }

    if (!fs::is_directory(p, ec)) return path;              // 文件 / 符号链接目标
    const auto direct = levelFilesIn(p, false);
    if (direct.size() == 1) return direct.front().string();
    if (direct.empty()) {
        const auto nested = levelFilesIn(p, true);
        if (nested.size() == 1) return nested.front().string();
    }
    return path;                                            // 0 个或多个：不猜
}

}  // namespace adofai
