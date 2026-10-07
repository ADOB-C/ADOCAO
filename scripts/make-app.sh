#!/usr/bin/env bash
# make-app.sh — package build/adocao into a self-contained macOS ADOCAO.app
#
# The bundle ships its runtime assets in Contents/Resources/assets, so it runs
# from anywhere (Finder, /Applications, a downloaded artifact) instead of
# depending on the repo layout next to it.
#
# 用法:
#   scripts/make-app.sh             # 打包 build/adocao → build/adocao.app
#   scripts/make-app.sh <binary>    # 指定可执行文件
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
BIN="${1:-$BUILD/ADOCAO}"
APP="$BUILD/ADOCAO.app"

if [ ! -x "$BIN" ]; then
    echo "❌ 找不到可执行文件: $BIN（先构建：cmake --build build --parallel）" >&2
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

cp "$BIN" "$APP/Contents/MacOS/ADOCAO"
chmod +x "$APP/Contents/MacOS/ADOCAO"

cp -R "$ROOT/assets" "$APP/Contents/Resources/assets"

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
if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - "$APP" >/dev/null 2>&1 \
        && echo "✅ 已 ad-hoc 签名" \
        || echo "⚠️  codesign 失败（bundle 仍可用，可能触发 Gatekeeper 提示）"
fi

echo "✅ 已打包 $APP"
echo "   版本       : $VERSION"
echo "   可执行文件 : $(du -h "$APP/Contents/MacOS/ADOCAO" | cut -f1)"
echo "   资产       : $(find "$APP/Contents/Resources/assets" -type f | wc -l | tr -d ' ') 个文件, $(du -sh "$APP/Contents/Resources/assets" | cut -f1)"
