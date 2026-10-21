#!/usr/bin/env bash
# macOS 侧急救：ADOCAO 退出后可能留下的坏状态（光标不见/不动、输入源被写坏、Mission Control 卡住、
# 全屏空壳 Space、卡住不动的进程）。**只做可逆、可解释的动作**，不改系统偏好设置里任何选项。
#
#   scripts/mac-recover.sh            # 只报告（安全）
#   scripts/mac-recover.sh fix        # 依次：杀 ADOCAO → 重置光标 → 修输入源 → 重启 Dock
#
# 背景（2026-10-19 两次真事故）：
#   * `NSApplicationPresentationFullScreen` 会禁用 Mission Control/⌘Tab —— app 一挂就把用户困住；
#   * `CGAssociateMouseAndMouseCursorPosition(false)` / `CGDisplayHideCursor` 是**系统级**的，
#     会把整台机器的鼠标解绑/光标藏掉；
#   * 把 `com.apple.HIToolbox AppleCurrentKeyboardLayoutInputSourceID` 写成系统里没启用的布局，
#     会出现"Keyboard Viewer 能看到按键、App 收不到，只有 Command 有效"。
# 这三条在代码里都已移除（见 app/MacBorderless.hpp 的注释），本脚本用于事后收拾。
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=tools/mac-recover/recover.c
BIN=build/recover
mkdir -p build
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    clang -Wno-deprecated-declarations -framework ApplicationServices -framework CoreGraphics \
        "$SRC" -o "$BIN"
fi

case "${1:-report}" in
  report)
    "$BIN" report
    echo
    echo "ADOCAO 进程：$(pgrep -if 'adocao' | tr '\n' ' ' || true)"
    ;;
  fix)
    echo "== 1) 杀掉可能卡住的 ADOCAO =="
    pkill -f "ADOCAO" 2>/dev/null || pkill -f "adocao" 2>/dev/null || echo "  没有 ADOCAO 进程"
    echo "== 2) 重置光标 =="
    "$BIN" fix-cursor
    echo "== 3) 修正输入源 =="
    "$BIN" fix-input-source
    echo "== 4) 关闭/重启 Dock（清 Mission Control） =="
    "$BIN" close-mission-control
    echo "== 5) 复查 =="
    "$BIN" report
    ;;
  *)
    echo "用法: scripts/mac-recover.sh [report|fix]" >&2
    exit 2
    ;;
esac
