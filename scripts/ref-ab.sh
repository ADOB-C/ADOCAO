#!/usr/bin/env bash
# 动态 A/B：同一张谱、同一时刻，分别用 Re_ADOJAS（参考实现）和 ADOCAO 出帧，并排拼图。
#
#   scripts/ref-ab.sh <chart.adofai> --time 44 [--width 1280 --height 720] [--zoom 100]
#   scripts/ref-ab.sh <chart.adofai> --time 44 --no-camera      # 不用关卡相机（看砖面细节）
#
# 为什么需要它：像素门槛只证明"和上一版一致"，不证明"和参考实现一致"。这条路径直接跑权威实现
# （~/Repos/Re_ADOJAS 的 vite dev server）并抓同一时刻的帧 —— 2026-10-18 就是这么确认
# "砖被狗啃"在参考实现里也存在（不是我们几何的锅），以及相机 framing 才是真正的差异。
#
# 前置：参考仓库已装依赖（node_modules）、playwright 全局可用、系统有 Chromium。
# 环境变量：
#   ADOJAS_REF  参考仓库路径（默认 ~/Repos/Re_ADOJAS）
#   REF_PORT    dev server 端口（默认 5199）
#   REF_NODE    跑 vite/playwright 的 node（默认 /opt/homebrew/bin/node；见下面 runtime 说明）
#   REF_PY      带 Pillow 的 python3（默认探测 ADOCAO_GATE_PY → python3）
#   CHROME      Chromium 可执行文件
set -euo pipefail
cd "$(dirname "$0")/.."

CHART=""; TIME=""; TILE=""; CAMERA=1; ZOOM=""; W=1280; H=720
while [ $# -gt 0 ]; do
  case "$1" in
    --time) TIME="$2"; shift 2;;
    --tile) TILE="$2"; shift 2;;
    --zoom) ZOOM="$2"; shift 2;;
    --width) W="$2"; shift 2;;
    --height) H="$2"; shift 2;;
    --no-camera) CAMERA=0; shift;;
    -*) echo "未知选项 $1" >&2; exit 2;;
    *) CHART="$1"; shift;;
  esac
done
[ -n "$CHART" ] || { echo "用法: scripts/ref-ab.sh <chart.adofai> --time 44 [--zoom 100] [--no-camera]" >&2; exit 2; }
[ -n "$TIME" ] || [ -n "$TILE" ] || { echo "要么 --time，要么 --tile" >&2; exit 2; }

REF="${ADOJAS_REF:-$HOME/Repos/Re_ADOJAS}"
PORT="${REF_PORT:-5199}"
NODE="${REF_NODE:-/opt/homebrew/bin/node}"
[ -x "$NODE" ] || NODE="$(command -v node || true)"
[ -x "$NODE" ] || { echo "找不到可用的 node（设 REF_NODE）" >&2; exit 2; }
[ -d "$REF/node_modules" ] || { echo "$REF 没有 node_modules（先在那里 pnpm i）" >&2; exit 2; }

PY="${ADOCAO_GATE_PY:-}"
if [ -z "$PY" ]; then
  # 候选：系统 python3 → harness 自带的运行时 python（~/.dsh/... 里有 Pillow）
  for c in python3 /usr/bin/python3 "$HOME"/.dsh/dsh-runtimes/*/dependencies/python/bin/python3; do
    if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then
      if "$c" -c 'import PIL' >/dev/null 2>&1; then PY="$c"; break; fi
    fi
  done
fi
[ -n "$PY" ] || { echo "需要带 Pillow 的 python3（设 ADOCAO_GATE_PY）" >&2; exit 2; }

if [ -z "${NODE_PATH:-}" ]; then
  NODE_PATH="$(npm root -g 2>/dev/null || true)"
fi
export NODE_PATH
"$NODE" -e 'require("playwright")' >/dev/null 2>&1 || \
  echo "警告: 找不到 playwright —— 设 NODE_PATH=$(npm root -g 2>/dev/null || echo '<全局 node_modules>')" >&2

# **绝对路径**：下面起 vite 时在子 shell 里 `cd "$REF"`，相对路径会把日志写到参考仓库里去
OUT="$PWD/build/ref-ab"; mkdir -p "$OUT"
NAME="$(basename "${CHART%.adofai}")"
[ -n "$TIME" ] && NAME="${NAME}_t${TIME}"
[ -n "$TILE" ] && NAME="${NAME}_f${TILE}"
true
REF_PNG="$OUT/${NAME}_ref.png"; OUR_PNG="$OUT/${NAME}_our.png"; AB_PNG="$OUT/${NAME}_ab.png"

# --- 参考侧：必要时起 vite（已有服务就复用） ---
if ! curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
  echo "== 启动 Re_ADOJAS dev server (:${PORT}) =="
  ( cd "$REF" && "$NODE" ./node_modules/vite/bin/vite.js --port "$PORT" --strictPort \
      > "$OUT/vite.log" 2>&1 & )
  for _ in $(seq 1 40); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
    sleep 0.5
  done
fi
curl -s -o /dev/null "http://127.0.0.1:$PORT/" || { echo "参考 dev server 没起来，见 $OUT/vite.log" >&2; exit 1; }

T="${TIME:-0}"
echo "== 参考实现抓帧（t=${T}s） =="
REF_URL="http://127.0.0.1:$PORT/" "$NODE" tools/ref-ab/ref_play.cjs "$CHART" "$REF_PNG" "$T"

# --- 我们这边：同一时刻、同样的分辨率 ---
echo "== ADOCAO 抓帧 =="
OUR_ARGS=(--capture "$OUR_PNG" --capture-time "$T" --width "$W" --height "$H" --msaa 4)
[ "$CAMERA" = 1 ] && OUR_ARGS+=(--capture-camera)
[ -n "$ZOOM" ] && [ "$CAMERA" = 0 ] && OUR_ARGS+=(--capture-zoom "$ZOOM")
./build/adocao "$CHART" "${OUR_ARGS[@]}" >/dev/null 2>&1 || { echo "ADOCAO 抓帧失败" >&2; exit 1; }

# --- 并排拼图 ---
"$PY" - "$REF_PNG" "$OUR_PNG" "$AB_PNG" <<'PY'
import sys
from PIL import Image, ImageDraw
ref, our, out = sys.argv[1:4]
a = Image.open(ref).convert('RGB'); b = Image.open(our).convert('RGB')
if a.size != b.size: b = b.resize(a.size)
W, H = a.size
img = Image.new('RGB', (W, H * 2 + 10), (255, 255, 255))
img.paste(a, (0, 0)); img.paste(b, (0, H + 10))
d = ImageDraw.Draw(img)
d.text((8, 6), 'Re_ADOJAS (reference)', fill=(255, 60, 60))
d.text((8, H + 16), 'ADOCAO (ours)', fill=(255, 60, 60))
img.save(out)
print('A/B ->', out)
PY
