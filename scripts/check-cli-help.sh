#!/bin/bash
# 帮助文本不许和解析器漂移：main.cpp 里解析的每个 --flag 都必须在 printHelp 里出现。
# 用法：scripts/check-cli-help.sh   （退出码 0 = 一致）
set -u
cd "$(dirname "$0")/.."
MAIN=app/main.cpp
# 只取 "CLI-FLAGS-BEGIN/END" 之间（否则会把帮助文本里的开关名算进来，等于自己跟自己比）
parsed=$(sed -n '/CLI-FLAGS-BEGIN/,/CLI-FLAGS-END/p' $MAIN | grep -oE '"--[a-z0-9-]+"' | tr -d '"' | sort -u)
help=$(sed -n '/void printHelp/,/^}/p' $MAIN | grep -oE '\-\-[a-z0-9-]+' | sort -u)
missing=$(comm -23 <(echo "$parsed") <(echo "$help"))
n=$(echo "$parsed" | wc -l | tr -d ' ')
if [ -n "$missing" ]; then
    echo "✗ 这些开关没写进 --help："
    echo "$missing" | sed 's/^/    /'
    exit 1
fi
echo "✓ --help 覆盖全部 $n 个开关"
