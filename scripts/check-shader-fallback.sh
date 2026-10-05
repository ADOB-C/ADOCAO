#!/bin/bash
# 内嵌回退 GLSL（render/Shaders.hpp）必须与 assets/shaders/ 下的文件一致 ——
# 找不到 shader 文件时会用回退串，漂移了就是"文件版本对、打包版本错"的静默 bug。
# 用法：scripts/check-shader-fallback.sh   （退出码 0 = 一致）
set -u
cd "$(dirname "$0")/.."
PY="${PYTHON:-python3}"
"$PY" - <<'PYEOF'
import re, sys
sh = open("render/Shaders.hpp", encoding="utf-8").read()
def grab(name):
    m = re.search(r'constexpr const char\* ' + name + r' = R"\((.*?)\)";', sh, re.S)
    return m.group(1) if m else None
def file(p):
    return open(p, encoding="utf-8").read()
checks = [("kTileVertSrc", "assets/shaders/tile.vert"),
          ("kTileFragSrc", "assets/shaders/tile.frag"),
          ("kHighlightFragSrc", "assets/shaders/highlight.frag"),
          ("kPlanetVertSrc", "assets/shaders/planet.vert"),
          ("kPlanetFragSrc", "assets/shaders/planet.frag"),
          ("kTrailVertSrc", "assets/shaders/trail.vert"),
          ("kTrailFragSrc", "assets/shaders/trail.frag")]
bad = 0
for name, path in checks:
    a, b = grab(name), file(path)
    if a is None:
        print("✗ Shaders.hpp 里找不到 %s" % name); bad += 1
    elif a != b:
        print("✗ %s 与 %s 不一致（回退串要和文件逐字相同）" % (name, path)); bad += 1
if bad:
    sys.exit(1)
print("✓ 内嵌回退 GLSL 与 assets/shaders/ 一致（%d 对）" % len(checks))
PYEOF
