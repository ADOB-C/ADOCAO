#include "PlatformPaths.hpp"

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

std::string executableDirectory() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string p(buf, n);
    const auto pos = p.find_last_of("\\/");
    return pos == std::string::npos ? std::string{} : p.substr(0, pos);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::string dir(buf.data());
    const auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved)) dir = resolved;
    return dir;
#elif defined(__linux__)
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string dir(buf);
    const auto pos = dir.find_last_of('/');
    return pos == std::string::npos ? std::string{} : dir.substr(0, pos);
#else
    return {};
#endif
}
