#pragma once
// **可调常数的单一入口**（2026-11-01 用户要求："把所有写死的也好，常量也好，都用所有谱面
// 控制单一变量测一遍"）。
//
// 为什么要有它：这些上限/阈值原来全是编译期 `constexpr`，想扫一个值就得重编一次
// （几十个值 × 30 张谱 = 不可能跑完）。这里给一个**运行时覆盖**：
//
//     ADOCAO_TUNE="maxPoints=262144,cullCand=1048576,rateMax=4000"
//
// 解析一次、按名字查；**每个调用点用 `static const` 缓存**，热路径上没有 map 查找。
// 未设置时行为与"编译期常量"**逐位相同**（默认值就是原来的常量）。
//
// 用法（调用点）：
//     const double k = tune::num("maxPoints", 262144.0);
//     const int    n = tune::integer("cullCand", 1u << 19);
//
// 约定：
//   * 名字用 camelCase，与 `docs/env.md` 的 ADOCAO_TUNE 表逐条对齐（check-env-docs.sh 会查）；
//   * **只放"可扫的旋钮"**，不要把 π、协议常量、GL 枚举塞进来；
//   * 布尔一律 `0/1`（`tune::flag`），与 `env::flag()` 的 false 词表一致。
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace adofai {
namespace tune {

// 解析 ADOCAO_TUNE（只做一次）。格式 "k1=v1,k2=v2"，允许空格；坏 token 忽略。
inline const std::unordered_map<std::string, double>& table() {
    static const std::unordered_map<std::string, double> t = [] {
        std::unordered_map<std::string, double> m;
        const char* s = std::getenv("ADOCAO_TUNE");
        if (!s) return m;
        std::string cur;
        auto flush = [&] {
            const size_t eq = cur.find('=');
            if (eq != std::string::npos && eq > 0) {
                const std::string k = cur.substr(0, eq);
                const char* v = cur.c_str() + eq + 1;
                char* end = nullptr;
                const double d = std::strtod(v, &end);
                if (end && end != v) m[k] = d;
            }
            cur.clear();
        };
        for (const char* p = s; *p; ++p) {
            if (*p == ',') flush();
            else if (*p != ' ') cur.push_back(*p);
        }
        flush();
        return m;
    }();
    return t;
}

inline bool has(const char* name) {
    const auto& t = table();
    return t.find(name) != t.end();
}

// 数值：未覆盖时返回 default。**调用点请用 `static const` 缓存**（见文件头）。
inline double num(const char* name, double def) {
    const auto& t = table();
    auto it = t.find(name);
    return it == t.end() ? def : it->second;
}

inline long long integer(const char* name, long long def) {
    return (long long)num(name, (double)def);
}

inline bool flag(const char* name, bool def) {
    if (!has(name)) return def;
    const double d = num(name, def ? 1.0 : 0.0);
    return d != 0.0;
}

}  // namespace tune
}  // namespace adofai
