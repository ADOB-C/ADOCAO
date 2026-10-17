#!/bin/bash
# 重新生成 tests/camera_gold.inc（跨语言相机金标）。需要：node ≥ 22、本机有 Re_ADOJAS 克隆。
#   RE_ADOJAS=~/Repos/Re_ADOJAS tests/gen_camera_gold.sh
set -eu
cd "$(dirname "$0")/.."
REPO="${RE_ADOJAS:-$HOME/Repos/Re_ADOJAS}"
[ -f "$REPO/src/lib/Player/CameraController.ts" ] || { echo "找不到 $REPO/src/lib/Player/CameraController.ts（设 RE_ADOJAS=…）"; exit 1; }
TMP=build/camera-gold
rm -rf "$TMP" && mkdir -p "$TMP"
cp "$REPO/src/lib/Player/CameraController.ts" "$REPO/src/lib/Player/Easing.ts" \
   "$REPO/src/lib/Player/EventUtils.ts" "$TMP/"
# Re_ADOJAS 的 ESM import 不带扩展名；Node 的类型擦除只认显式 .ts
sed -i '' "s#from './Easing'#from './Easing.ts'#; s#from './EventUtils'#from './EventUtils.ts'#; s#from './TileAccess'#from './TileAccess.ts'#" "$TMP/CameraController.ts" 2>/dev/null || \
  sed -i "s#from './Easing'#from './Easing.ts'#; s#from './EventUtils'#from './EventUtils.ts'#; s#from './TileAccess'#from './TileAccess.ts'#" "$TMP/CameraController.ts"
cp tests/gen_camera_gold.mjs "$TMP/"
node "$TMP/gen_camera_gold.mjs"
echo "已写入 tests/camera_gold.inc"
