#!/usr/bin/env bash
# 标准库头缺失守卫：某文件用了 std::sort / std::strlen / std::deque / std::filesystem / std::snprintf，
# 却没有包含对应的头 → libc++ 常经由别的头间接带进来（macOS 绿），MinGW/glibc++ 直接红。
# 真踩过两次（530363b、7b5dbbd 的上一版），都是只在 windows-portable 上炸。
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

fail=0
check() {  # check <用法正则> <需要的头> <说明>
    local pat="$1" hdr="$2" what="$3"
    while IFS= read -r f; do
        grep -q "^#include ${hdr}$" "$f" && continue
        # 头文件里也可能只是声明；只报真正出现用法的
        printf '缺 %s（用了 %s）：%s\n' "$hdr" "$what" "$f"
        fail=1
    done < <(grep -rlE "$pat" --include='*.cpp' --include='*.hpp' core archive render audio app tests 2>/dev/null)
}

check 'std::(stable_)?sort[<(]' '<algorithm>' 'std::sort'
check 'std::(lower|upper)_bound[<(]' '<algorithm>' 'std::lower_bound'
check 'std::(min|max)_element[<(]' '<algorithm>' 'std::min_element'
check 'std::strlen\(|std::memcpy\(|std::memcmp\(|std::memset\(' '<cstring>' 'std::strlen/mem*'
check 'std::deque<' '<deque>' 'std::deque'
check 'std::filesystem::' '<filesystem>' 'std::filesystem'
check 'std::snprintf\(|std::fprintf\(' '<cstdio>' 'std::snprintf'
# 数学函数：libc++ 常经由别的头带进 <cmath>（macOS 绿），GCC/MinGW 直接报
# "'pow' is not a member of 'std'"（2026-10-24 真踩过：AdocaoColumns 的定点编码）
check 'std::(pow|round|llround|lround|isfinite|fabs|floor|ceil|sqrt|exp|log|log2)\(' '<cmath>' 'std::pow/round/...'

if [ "$fail" -ne 0 ]; then
    echo "check-std-includes: 有文件漏了标准库头（见上）" >&2
    exit 1
fi
echo "check-std-includes: OK"
