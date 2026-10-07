#!/usr/bin/env bash
# 本地运行 ADOCAO（macOS / Linux）
# 用法: run.sh [运行选项] [程序参数...]
#   示例: run.sh                             启动 launcher
#         run.sh --debug                     调试模式（程序参数）
#         run.sh --level x.adofai --music x.ogg   直接播放关卡
#   调试选项（run.sh 自身，会从程序参数中剥离）:
#         --debugger [lldb|gdb]              用调试器启动；不带名字时按平台取默认
#                                            （macOS → lldb，Linux → gdb）
#         --lldb / --gdb                     等价于 --debugger lldb / --debugger gdb
set -euo pipefail

BIN=""
for cand in ./build/adocao ./build/adocao.exe ./build/adocao; do
    if [ -x "$cand" ]; then BIN="$cand"; break; fi
done

if [ -z "$BIN" ]; then
    echo "未找到构建产物，请先构建: cmake --build build --parallel" >&2
    exit 1
fi

# ── 解析 run.sh 自身选项（--debugger/--lldb/--gdb），其余透传给程序 ──
DEBUGGER=""
declare -a ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --debugger)
            DEBUGGER="auto"; shift
            if [ $# -gt 0 ] && [ "$1" != -* ]; then DEBUGGER="$1"; shift; fi
            ;;
        --lldb) DEBUGGER="lldb"; shift ;;
        --gdb)  DEBUGGER="gdb";  shift ;;
        *) ARGS+=("$1"); shift ;;
    esac
done
if [ "${#ARGS[@]}" -gt 0 ]; then set -- "${ARGS[@]}"; else set --; fi

# ── 调试器启动 ───────────────────────────────────────────────────
if [ -n "$DEBUGGER" ]; then
    if [ "$DEBUGGER" = "auto" ]; then
        if [ "$(uname)" = "Darwin" ]; then
            DEBUGGER="lldb"
        else
            DEBUGGER="$(command -v gdb 2>/dev/null || echo lldb)"
        fi
    fi
    if ! command -v "$DEBUGGER" >/dev/null 2>&1; then
        echo "未找到调试器: $DEBUGGER（可用 --debugger lldb / gdb 指定）" >&2
        exit 1
    fi
    echo "▸ $DEBUGGER $BIN $*"
    if [ "$DEBUGGER" = "lldb" ]; then
        exec lldb -o run "$BIN" -- "$@"
    else
        exec gdb -ex run --args "$BIN" "$@"
    fi
fi

exec "$BIN" "$@"
