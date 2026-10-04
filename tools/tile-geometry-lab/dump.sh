#!/usr/bin/env bash
# 几何层验证：把 render/TileGeometry.cpp 的真实输出 dump 成 JSON。
# 编的就是**游戏里那份** C++（不是复制品），所以这里出来的形状就是画面上的形状。
#
#   tools/tile-geometry-lab/dump.sh [输出.json]      # 默认 geo.json
#
# 下一步（出图）：node tools/tile-geometry-lab/json2svg.mjs geo.json > geo.svg
#                 NODE_PATH=$(npm root -g) node tools/tile-geometry-lab/svgshot.cjs geo.svg geo.png
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="${1:-geo.json}"

compiler="${CXX:-clang++}"
"$compiler" -std=c++20 -O1 -I"$repo/render" "$here/dump.cpp" "$repo/render/TileGeometry.cpp" -o "$here/dump"
"$here/dump" > "$out"
echo "ok $out （$(python3 -c "import json,sys;print(len(json.load(open('$out'))))" 2>/dev/null || echo '?') 个用例）"
