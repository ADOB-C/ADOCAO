#!/bin/bash
# 环境变量总表与源码对账：`getenv("ADOCAO_...")` 出现的每个名字都必须在 docs/env.md 里登记。
#
# 为什么：2026-10-18 清点时发现 39 个 ADOCAO_* 钩子散在 12 个文件里、四种布尔写法、
# 文档里只有一半。这份检查是"别再加一个没人知道的钩子"的机械护栏 —— 加/删环境变量时
# 一起改 docs/env.md，漏了这里就红。
set -u
cd "$(dirname "$0")/.."
DOC=docs/env.md
[ -f "$DOC" ] || { echo "缺少 $DOC"; exit 1; }

# 源码里真正读的（排除构建目录）
names=$( { grep -rhoE 'getenv\("(ADOCAO_[A-Z0-9_]+|ADOFAO_[A-Z0-9_]+)"\)' \
               --include=*.cpp --include=*.hpp app core render archive audio glad 2>/dev/null \
             | sed -E 's/getenv\("([A-Z0-9_]+)"\)/\1/'
             grep -rhoE 'flag\("(ADOCAO_[A-Z0-9_]+|ADOFAO_[A-Z0-9_]+)"\)' \
               --include=*.cpp --include=*.hpp app core render archive audio glad 2>/dev/null \
             | sed -E 's/flag\("([A-Z0-9_]+)"\)/\1/' ; } | sort -u)
# 脚本里用的
script_names=$(grep -rhoE '\b(ADOCAO_[A-Z0-9_]+|ADOFAO_[A-Z0-9_]+)\b' scripts/ 2>/dev/null \
        | sort -u)

all=$(printf '%s\n' $names $script_names | grep . | sort -u)
missing=0
for n in $all; do
    if ! grep -q "\`$n\`" "$DOC"; then
        echo "未登记到 $DOC: $n"
        missing=1
    fi
done

count=$(printf '%s\n' $all | grep -c .)
echo "check-env-docs: $count 个环境变量（源码 getenv/flag + 脚本）全部已在 $DOC 登记"
[ "$missing" = 0 ] || { echo "有环境变量没写进 $DOC（规则见该页顶部）"; exit 1; }
