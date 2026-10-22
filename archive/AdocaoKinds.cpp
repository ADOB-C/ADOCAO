#include "archive/AdocaoKinds.hpp"

#include <cctype>

namespace adofai {
namespace adocao {

bool isHexColor(std::string_view v) {
    if (!v.empty() && v.front() == '#') v.remove_prefix(1);
    if (v.size() != 6 && v.size() != 8) return false;
    for (char c : v) {
        if (!std::isxdigit((unsigned char)c)) return false;
    }
    return true;
}

bool looksLikeTag(std::string_view v) {
    if (v.empty() || v.size() > 64) return false;
    for (char c : v) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20 || c == ' ' || c == '"' || c == '\\' || c == ',' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == ':')
            return false;
    }
    return true;
}

Kind kindOfScalar(const char* s, size_t n) {
    std::string_view v(s, n);
    if (v == "true" || v == "false") return Kind::Bool;
    if (v == "null") return Kind::RawJSON;   // 三态由 presence/null 位图表达，不走 kind
    if (isHexColor(v)) return Kind::Color;
    // 整数 / 浮点：只做形态判定，真正的数值解析在上层（parseNumber / strtod）
    bool numeric = !v.empty();
    bool hasDot = false, hasExp = false;
    for (size_t i = 0; i < v.size() && numeric; ++i) {
        const char c = v[i];
        if (c == '-' && i == 0) continue;
        if (c == '.' && !hasDot && !hasExp) { hasDot = true; continue; }
        if ((c == 'e' || c == 'E') && !hasExp && i > 0) { hasExp = true; continue; }
        if ((c == '+' || c == '-') && (v[i - 1] == 'e' || v[i - 1] == 'E')) continue;
        if (!std::isdigit((unsigned char)c)) numeric = false;
    }
    if (numeric) return (hasDot || hasExp) ? Kind::Float : Kind::Int;
    if (v.size() <= kMaxEnumLen) return Kind::Enum;   // 列级统计再降级成 Tag/String
    return looksLikeTag(v) ? Kind::Tag : Kind::String;
}

}  // namespace adocao
}  // namespace adofai
