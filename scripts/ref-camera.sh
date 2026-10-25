#!/usr/bin/env bash
# 用 **Re_ADOJAS 自己的 `CameraController`** 跑一遍运镜，打印指定时刻的相机状态，
# 与 `ADOCAO_STATE_LOG=1` 抓到的 `state:` 行逐项对比。
#
#   scripts/ref-camera.sh <chart.adofai> 180.15 185.15 190.16
#   scripts/ref-camera.sh <chart.adofai> --tiles 1550 1609      # 按砖号换算出时间
#
# 为什么需要它：`ADOCAO_CAM_DUMP` 只能说明"我们的链条自洽"，不能说明"和权威实现一致"。
# 这条路径直接把参考仓库的 CameraController.ts 用 esbuild 打成 CJS，喂**同一份**谱面事件
# + 我们的逐砖网格（build/trackvis_grid.csv），逐帧驱动到目标时刻，再把两边的
# cam / zoom / rot 摆在一起。
#
# 前置：`~/Repos/Re_ADOJAS` 有 node_modules（esbuild 在里头）；
#       先跑一次 `ADOCAO_TRACKVIS_DUMP=2 ./build/adocao --capture ... --level <chart>` 生成网格。
# 环境变量：ADOJAS_REF（默认 ~/Repos/Re_ADOJAS）、REF_NODE（默认 /opt/homebrew/bin/node）
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

CHART="${1:-}"; shift || true
[ -n "$CHART" ] || { echo "用法: scripts/ref-camera.sh <chart.adofai> <秒...> | --tiles <砖号...>" >&2; exit 2; }
REF="${ADOJAS_REF:-$HOME/Repos/Re_ADOJAS}"
NODE="${REF_NODE:-/opt/homebrew/bin/node}"
[ -x "$NODE" ] || NODE="$(command -v node || true)"
[ -n "$NODE" ] || { echo "找不到 node（设 REF_NODE）" >&2; exit 2; }
[ -x "$REF/node_modules/.bin/esbuild" ] || { echo "$REF 没装 esbuild（先 pnpm i）" >&2; exit 2; }
[ -f build/trackvis_grid.csv ] || {
    echo "缺 build/trackvis_grid.csv：先跑" >&2
    echo "  ADOCAO_TRACKVIS_DUMP=2 ./build/adocao --capture /tmp/x.png --level <chart> ..." >&2
    exit 2
}

OUT=build/ref-camera; mkdir -p "$OUT"

# 1) 把参考的 CameraController 打成 CJS（纯数学，无 GL 依赖）
"$REF/node_modules/.bin/esbuild" "$REF/src/lib/Player/CameraController.ts" \
    --bundle --format=cjs --platform=node --outfile="$OUT/refcam.cjs" >/dev/null 2>&1 || {
    echo "esbuild 失败（参考仓库路径对吗？）" >&2; exit 1; }

# 2) 谱面事件 + 我们的网格 → JSON
python3 - "$CHART" "$OUT/input.json" <<'PY'
import csv, json, re, sys
chart, out = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open('build/trackvis_grid.csv')))
start = [float(r['startTime']) for r in rows]
bpm = [float(r['bpm']) for r in rows]
pos = [(float(r['posX']), float(r['posY'])) for r in rows]
s = open(chart, encoding='utf-8-sig', errors='replace').read()
s = re.sub(r',(\s*),', r',\1', s)        # 有些谱有 `,,`
s = re.sub(r',(\s*[}\]])', r'\1', s)     # 和尾逗号
d = json.loads(s)
cams = [a for a in d['actions'] if a.get('eventType') == 'MoveCamera']
for i, e in enumerate(cams):
    e['id'] = i
json.dump({'settings': d['settings'], 'cams': cams, 'start': start, 'bpm': bpm, 'pos': pos},
          open(out, 'w'))
print(f"ref-camera: {len(start)} 砖 / {len(cams)} 条 MoveCamera", file=sys.stderr)
PY

# 3) 把秒或砖号换成秒
ARGS=()
if [ "${1:-}" = "--tiles" ]; then
    shift
    for tile in "$@"; do
        t=$(python3 - "$OUT/input.json" "$tile" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); i = int(sys.argv[2])
print(f"{d['start'][i]:.4f}")
PY
)
        ARGS+=("$t")
    done
else
    ARGS=("$@")
fi

# 4) 驱动参考相机
"$NODE" - "$OUT/input.json" "$OUT/refcam.cjs" "${ARGS[@]}" <<'JS'
const fs = require('fs')
const [inpPath, camPath, ...rest] = process.argv.slice(2)
const { CameraController } = require(require('path').resolve(camPath))
const inp = JSON.parse(fs.readFileSync(inpPath, 'utf8'))
const want = rest.map(Number)
const { settings, cams, start, bpm, pos } = inp
const positions = {
  getX: (i) => pos[Math.max(0, Math.min(pos.length - 1, i))][0],
  getY: (i) => pos[Math.max(0, Math.min(pos.length - 1, i))][1],
}
const cc = new CameraController({ settings, actions: cams }, start, bpm, positions)
const entries = cams.map((e) => {
  const f = e.floor || 0
  const spb = 60 / (bpm[f] || 100)
  return { time: (start[f] || 0) + ((e.angleOffset || 0) / 180) * spb, event: e }
}).sort((a, b) => a.time - b.time)
cc.loadCameraTimeline(entries)
const dt = 1 / 120
let last = -1, si = -1
const TMAX = Math.max(...want) + 0.5
for (let t = 0; t <= TMAX; t += dt) {
  while (si + 1 < start.length && start[si + 1] <= t) si++
  const i = Math.max(0, si)
  cc.update({ nowSeconds: t, deltaSeconds: dt, bpm: bpm[i] || 160, pitch: 1,
              planetWorldPos: { x: pos[i][0], y: pos[i][1] }, currentTileIndex: i })
  for (const w of want) if (last < w && t >= w) {
    const p = cc.getCameraPosition()
    console.log(`ref   t=${w.toFixed(2)}  cam=(${p.x.toFixed(2)},${p.y.toFixed(2)})` +
                ` zoom=${cc.getCameraZoom().toFixed(1)} rot=${cc.getCameraRotation().toFixed(1)}` +
                ` follow=${cc.isFollowMode()}`)
  }
  last = t
}
JS

echo
echo "对照：ADOCAO_STATE_LOG=1 跑同一段，看 'state:' 行里的 cam/zoom/rot。"
echo "注意：我们日志里的 zoom 是**内部**值（100 = 视野高 12 砖），换算 ADOFAI 口径："
echo "      adoZoom = 21937.8427 / internalZoom"
