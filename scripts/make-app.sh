#!/usr/bin/env bash
# make-app.sh — package build/adocao into a self-contained macOS ADOCAO.app
#
# The bundle ships its runtime assets in Contents/Resources/assets, so it runs
# from anywhere (Finder, /Applications, a downloaded artifact) instead of
# depending on the repo layout next to it.
#
# 用法:
#   scripts/make-app.sh             # 打包 build/adocao → build/adocao.app（发布用：拷真二进制 + 签名）
#   scripts/make-app.sh <binary>    # 指定可执行文件
#   scripts/make-app.sh --link      # 开发用：可执行文件做成指向 build/adocao 的**符号链接**
#                                   #（`./build.sh` 之后不必重跑本脚本；代价是 bundle 不签名）
#
# 关于 --link：bundle 签名会随链接目标变化而失效，所以链接模式**跳过 codesign**
#（发布产物请用默认模式）。另外见 app/Application.cpp 的 `initGlfwHints()`：
# GLFW 默认会 chdir 到 bundle 的 Contents/Resources，已显式关掉，所以链接模式与直跑
# `build/adocao` 的相对路径行为一致。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
LINK_MODE=0
if [ "${1:-}" = "--link" ]; then LINK_MODE=1; shift; fi
BIN="${1:-$BUILD/ADOCAO}"
APP="$BUILD/ADOCAO.app"

if [ ! -x "$BIN" ]; then
    echo "❌ 找不到可执行文件: ${BIN}（先构建：cmake --build build --parallel）" >&2
    exit 1
fi
if [ ! -d "$ROOT/assets/shaders" ] || [ ! -d "$ROOT/assets/hitsounds" ]; then
    echo "❌ 缺少 $ROOT/assets/{shaders,hitsounds}" >&2
    exit 1
fi

# Version comes from CMakeLists so the bundle cannot drift from the build.
VERSION="$(sed -n 's/^project(ADOCAO VERSION \([^ ]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || VERSION="0.0.0"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

if [ "$LINK_MODE" = 1 ]; then
    # 相对链接：bundle 在哪、build/adocao 在哪都跟着走（不受绝对路径影响）
    ln -sf "../../../$(basename "$BIN")" "$APP/Contents/MacOS/ADOCAO"
    echo "🔗 可执行文件 = 符号链接 → $(readlink "$APP/Contents/MacOS/ADOCAO")（开发模式）"
else
    cp "$BIN" "$APP/Contents/MacOS/ADOCAO"
    chmod +x "$APP/Contents/MacOS/ADOCAO"
fi

# **--link 模式下资源也做成符号链接**：否则 bundle 里的 assets 是构建时的快照，
# 改了 shader/资产后 .app 仍加载旧文件（2026-10-21 真踩过：pivot 修复没进 .app，
# 用户对着旧画面怀疑人生）。快照模式（正式打包）仍然复制。
if [ "$LINK_MODE" = 1 ]; then
    rm -rf "$APP/Contents/Resources/assets"
    ln -s "$ROOT/assets" "$APP/Contents/Resources/assets"
else
    cp -R "$ROOT/assets" "$APP/Contents/Resources/assets"
fi

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ADOCAO</string>
    <key>CFBundleIdentifier</key>
    <string>com.adocao.ADOCAO</string>
    <key>CFBundleName</key>
    <string>ADOCAO</string>
    <key>CFBundleDisplayName</key>
    <string>ADOCAO</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# Ad-hoc sign last: the seal covers the executable and Resources.
# 链接模式下跳过：签名会随 `./build.sh` 换掉目标而立刻失效，签了也没意义。
if [ "$LINK_MODE" = 1 ]; then
    echo "ℹ️  链接模式：跳过 codesign（bundle 未签名，仅本机开发用）"
elif command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - "$APP" >/dev/null 2>&1 \
        && echo "✅ 已 ad-hoc 签名" \
        || echo "⚠️  codesign 失败（bundle 仍可用，可能触发 Gatekeeper 提示）"
fi

echo "✅ 已打包 $APP"
echo "   版本       : $VERSION"
echo "   可执行文件 : $(du -h "$APP/Contents/MacOS/ADOCAO" | cut -f1)"
echo "   资产       : $(find -L "$APP/Contents/Resources/assets" -type f | wc -l | tr -d ' ') 个文件, $(du -shL "$APP/Contents/Resources/assets" | cut -f1)"
