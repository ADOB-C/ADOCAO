#!/usr/bin/env bash
# 用**真 GCC** 做全仓语法检查（只跑 -fsyntax-only，不链接、不重建依赖）。
#
# 为什么需要它：macOS 默认的 `/usr/bin/g++` 是 **Apple clang 的壳**，很多"只在 GCC/MinGW
# 上炸"的问题它看不见。2026-10-23/24 连踩两次，都是本地全绿、CI 红：
#   ① `std::pow/round/isfinite/llround` 没 include `<cmath>`（libc++ 经由别的头间接带进来）；
#   ② `(void)y0;` —— `y0` 是 `<math.h>` 里的 Bessel 函数名，clang 当"取函数地址"，
#      MinGW 没这个声明 → 直接报未声明。
#
# 用法：
#   scripts/check-gcc.sh                # 全量（286 个 TU，8 并行）
#   scripts/check-gcc.sh archive app    # 只查指定目录前缀
#   scripts/check-gcc.sh --jobs 4
#
# 退出码：0 全过 / 1 有错误 / 2 环境不满足（没装真 GCC）。
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

JOBS=8
FILTERS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) FILTERS+=("$1"); shift ;;
    esac
done

find_gcc() {
    for c in g++-16 g++-15 g++-14 g++-13 g++-12 g++; do
        command -v "$c" >/dev/null 2>&1 || continue
        # 排除 Apple clang 的壳
        "$c" --version 2>/dev/null | head -1 | grep -qi "clang" && continue
        echo "$c"
        return 0
    done
    return 1
}

GXX=$(find_gcc) || {
    echo "check-gcc: 没找到真 GCC（macOS 上 /usr/bin/g++ 是 Apple clang 的壳）" >&2
    echo "            brew install gcc 之后重试" >&2
    exit 2
}
echo "check-gcc: 编译器 $($GXX --version | head -1)"

# 需要 compile_commands.json：重新 configure 一次（不重建）
if [ ! -f build/compile_commands.json ] || [ build/CMakeCache.txt -nt build/compile_commands.json ]; then
    cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null || exit 2
fi

GXX_PATH=$(command -v "$GXX")
export GXX_PATH JOBS
python3 - "${FILTERS[@]}" <<'PY'
import concurrent.futures as cf
import json, os, shlex, subprocess, sys

root = os.path.abspath('.')
gxx = os.environ['GXX_PATH']
jobs = int(os.environ.get('JOBS', '8'))
filters = [a for a in sys.argv[1:] if a]

db = json.load(open('build/compile_commands.json', encoding='utf-8'))
jobs_list = []
for e in db:
    f = os.path.abspath(e['file'])
    if not f.startswith(root + os.sep):
        continue
    if '/build/_deps/' in f:
        continue
    if filters and not any(('/' + flt.strip('/') + '/') in f for flt in filters):
        continue
    # .mm 是 Objective-C++：GCC 不支持 Apple 的 blocks（`^`）语法，SDK 头都过不了。
    # 这两个文件只在 macOS 上编译，由 macOS 那份 CI 负责。
    if f.endswith('.mm'):
        continue
    # 引 Apple 框架头的 .cpp 同理（CoreAudio/AudioToolbox 的 SDK 头里有 blocks）
    try:
        head = open(f, encoding='utf-8', errors='replace').read(8192)
    except OSError:
        head = ''
    if any(t in head for t in ('<CoreAudio', '<AudioToolbox', '<Cocoa/', '<AVFoundation')):
        continue
    args = shlex.split(e['command'])
    # 换编译器
    args[0] = gxx
    # 去掉 clang 专有 / 不适用的开关
    cleaned, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a == '-arch':            # clang: -arch arm64
            skip = True
            continue
        if a.startswith('-flto=thin'):
            cleaned.append('-flto')
            continue
        if a in ('-fcolor-diagnostics',):
            continue
        cleaned.append(a)
    cleaned.append('-fsyntax-only')
    jobs_list.append((f, cleaned))

def run(item):
    f, args = item
    p = subprocess.run(args, cwd=root, capture_output=True, text=True)
    return (f, p.returncode, p.stderr)

bad = 0
skipped = 0
with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
    for f, rc, err in pool.map(run, jobs_list):
        if rc == 0:
            continue
        rel = os.path.relpath(f, root)
        errs = [l for l in err.splitlines() if 'error' in l]
        # 错误全在**仓库外**的头文件里（Apple SDK / 第三方）→ 不是我们的代码问题，
        # GCC 只是解析不了那些头（blocks/ObjC 扩展）。这类记为 SKIP。
        own = [l for l in errs if root in l or rel in l]
        if errs and not own:
            skipped += 1
            print(f"SKIP {rel}（错误都在系统/第三方头里，GCC 解析不了）")
            continue
        bad += 1
        print(f"FAIL {rel}")
        for l in (own or errs)[:3]:
            print(f"     {l}")
print(f"check-gcc: {len(jobs_list)} 个翻译单元，{bad} 个失败，{skipped} 个跳过")
sys.exit(1 if bad else 0)
PY
