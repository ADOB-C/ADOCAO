#!/bin/bash
# 像素级验收门槛（GLSL 几何这类改动的唯一硬证据）。
#
#   scripts/capture-gate.sh store [--out DIR] [--only REGEX]
#   scripts/capture-gate.sh check --against DIR [--only REGEX] [--scratch DIR]
#
# 为什么需要它：`--capture` 是确定性抓帧（不走 wall clock / 音频 / 帧时间反馈），同一组参数
# 两次跑必须逐字节相同。清单在 tests/capture_states.txt（由 tests/gen_render_fixtures.py 生成）：
#     <name>|<chart|ABS>|<tile|time>|<值>|<zoom>|<WxH>|[额外开关]
#   用 `|` 分隔是因为谱面路径里有空格（"The Moon - Coal" 会被空白分隔切碎）。
# `tile` 走 --capture-tile（按砖，谱面再长也不漂），`time` 是既有的历史基线口径。
#
# 每次运行都会：
#   * 断言跑之前没有别的 ADOCAO 进程（日志共享，并行会互相污染）
#   * 从 build/ADOCAO.log 取 `capture: … tile=N req=M`，断言 N == M。应用启动时会**重写**日志，
#     所以直接取整份日志里的最后一条，不要按字节偏移取增量。
#   * 记录墙钟、峰值 RSS、`… unique shapes`、`Built track: … -> K shape groups`（K ≈ 每帧 draw 次数）
# check 逐字节比对；不一致的状态报告差异像素数（有 Pillow 时，可用 ADOCAO_GATE_PY 指定 python3）
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
BIN=build/ADOCAO
STATES=tests/capture_states.txt
LOG=build/ADOCAO.log
PY="${ADOCAO_GATE_PY:-python3}"
if /usr/bin/time -l true >/dev/null 2>&1; then TIME_FLAG=-l; else TIME_FLAG=-v; fi

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

MODE="${1:-}"; shift 2>/dev/null || true
OUT=""; AGAINST=""; ONLY=""; SCRATCH=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out)     OUT="${2:?--out 需要目录}"; shift 2 ;;
        --against) AGAINST="${2:?--against 需要目录}"; shift 2 ;;
        --only)    ONLY="${2:?--only 需要正则}"; shift 2 ;;
        --scratch) SCRATCH="${2:?--scratch 需要目录}"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "未知参数: $1"; usage ;;
    esac
done

abs() { case "$1" in /*) echo "$1" ;; *) echo "$ROOT/$1" ;; esac; }

case "$MODE" in
    store)
        OUT="${OUT:-/tmp/adocao-capture-baseline}"
        mkdir -p "$OUT"; CAPDIR=$(cd "$OUT" && pwd)
        : > "$CAPDIR/_run.txt"
        ;;
    check)
        [ -n "$AGAINST" ] && [ -d "$AGAINST" ] || { echo "✗ check 需要 --against <store 出来的目录>"; exit 2; }
        OUT=$(cd "$AGAINST" && pwd)
        SCRATCH="${SCRATCH:-/tmp/adocao-capture-check}"
        mkdir -p "$SCRATCH"; CAPDIR=$(cd "$SCRATCH" && pwd)
        ;;
    *) usage ;;
esac
[ -x "$BIN" ] || { echo "✗ 没有 $BIN（先 ./build.sh）"; exit 1; }
[ -f "$STATES" ] || { echo "✗ 没有 $STATES（跑 python3 tests/gen_render_fixtures.py）"; exit 1; }

run_capture() {   # name chart mode value zoom WxH → 写 $CAPDIR/$name.png；打印一行记录
    local name="$1" chart="$2" mode="$3" value="$4" zoom="$5" size="$6"
    local W="${size%x*}" H="${size#*x}"
    local chart_abs; chart_abs=$(abs "$chart")
    [ -f "$chart_abs" ] || { echo "SKIP|$name|谱面不存在（机器本地谱面？）"; return 0; }
    # 只关心**写同一个日志**的进程（= 直接跑 build/ADOCAO 的那个）。macOS 的
    # build/ADOCAO.app 是另一条日志路径（~/Library/Logs/ADOCAO/，见 AGENTS），
    # 而且可能是用户自己开着的，所以不拦它 —— 正则也顺带避开"匹配到自己命令行"。
    local running; running=$(pgrep -f '(^|/)build/ADOCAO( |$)' | wc -l | tr -d ' ')
    if [ "$running" != "0" ]; then
        echo "FAIL|$name|跑之前有 $running 个 build/ADOCAO 进程（日志共享，先收干净）"; return 1
    fi
    local flag
    case "$mode" in
        tile) flag=(--capture-tile "$value") ;;
        time) flag=(--capture-time "$value") ;;
        *) echo "FAIL|$name|未知模式 $mode"; return 1 ;;
    esac
    local t0; t0=$(date +%s)
    local extra_args=() hitsound_flag=(--no-hitsound)
    if [ -n "$extra" ]; then
        read -r -a extra_args <<< "$extra"
        # `+hitsounds` = 这个状态**加载音色**（默认 --no-hitsound 是为了快与确定，
        # 但那样音色路径就没人覆盖了 —— 真踩过：hitsounds 目录少个尾斜杠只在实玩时暴露）
        local filtered=()
        for a in "${extra_args[@]}"; do
            if [ "$a" = "+hitsounds" ]; then hitsound_flag=(); else filtered+=("$a"); fi
        done
        extra_args=("${filtered[@]}")
    fi
    /usr/bin/time "$TIME_FLAG" "$BIN" "$chart_abs" --capture "$CAPDIR/$name.png" "${flag[@]}" \
        --capture-zoom "$zoom" --width "$W" --height "$H" "${hitsound_flag[@]}" "${extra_args[@]}" \
        >/dev/null 2>"$CAPDIR/$name.time"
    local rc=$?
    local t1; t1=$(date +%s)
    local rss tline tile req shapes groups
    rss=$(grep -oE "^ *[0-9]+  *maximum resident set size" "$CAPDIR/$name.time" | grep -oE "[0-9]+" | head -1)
    if [ -n "${rss:-}" ]; then
        rss=$(awk -v r="$rss" 'BEGIN{printf "%.1f", r/1048576}')      # macOS /usr/bin/time -l：字节
    else
        rss=$(grep -oE "Maximum resident set size \(kbytes\): [0-9]+" "$CAPDIR/$name.time" | grep -oE "[0-9]+$" | head -1)
        [ -n "${rss:-}" ] && rss=$(awk -v r="$rss" 'BEGIN{printf "%.1f", r/1024}')   # Linux：KiB
    fi
    tline=$(grep "capture:" "$LOG" | tail -1)
    tile=$(echo "$tline" | grep -oE "tile=[-0-9]+" | cut -d= -f2)
    req=$(echo "$tline" | grep -oE "req=[-0-9]+" | cut -d= -f2)
    shapes=$(grep -oE "[0-9]+ unique shapes" "$LOG" | tail -1 | grep -oE "^[0-9]+")
    groups=$(grep -oE "\-> [0-9]+ shape groups" "$LOG" | tail -1 | grep -oE "[0-9]+")
    [ -n "${groups:-}" ] || groups=$(grep -oE "draws=[0-9]+" "$LOG" | tail -1 | grep -oE "[0-9]+")
    # 音色路径的守卫：门槛默认 --no-hitsound（快且确定），于是"hitsounds 目录不对"这类 bug
    # 只会出现在真实游玩里（真踩过：少一个尾斜杠 → assets/hitsoundsKick.wav，像素门槛全程看不见）。
    # 带 +hitsounds 的状态会真的加载 WAV；这里对**所有**状态断言日志里没有音色加载失败。
    if grep -qE "Hitsound: (Cannot open|Failed to read WAV)" "$LOG" 2>/dev/null; then
        echo "FAIL|$name|音色加载失败：$(grep -m1 -oE 'Hitsound: (Cannot open|Failed to read WAV).*' "$LOG")"; return 1
    fi
    if [ -z "${tline:-}" ] || [ "$(echo "$tline" | grep -c "$name.png")" = "0" ]; then
        echo "FAIL|$name|日志里没有本次 capture 行（拿到的是: ${tline:-空}）"; return 1
    fi
    if [ "$rc" != "0" ] || [ ! -f "$CAPDIR/$name.png" ]; then
        echo "FAIL|$name|退出码 $rc，没写出 PNG（看 $CAPDIR/$name.time）"; return 1
    fi
    if [ "$mode" = "tile" ] && [ "$tile" != "$value" ]; then
        echo "FAIL|$name|抓错砖：tile=$tile 期望 $value（--capture-tile 定点失效）"; return 1
    fi
    printf 'OK|%s|tile=%s req=%s|zoom=%s|shapes=%s|groups=%s|rss_mb=%s|sec=%s\n' \
        "$name" "${tile:-?}" "${req:-?}" "$zoom" "${shapes:-?}" "${groups:-?}" "${rss:-?}" "$((t1 - t0))"
}

pixel_diff() {  # a.png b.png → "diff=N maxdelta=M"
    "$PY" - "$1" "$2" <<'PYEOF' 2>/dev/null || echo "diff=?（像素统计失败）"
import sys
try:
    from PIL import Image, ImageChops
    import numpy as np
except Exception:
    print("diff=? （没装 Pillow；设 ADOCAO_GATE_PY 指向带 Pillow 的 python3）")
    sys.exit(0)
a = Image.open(sys.argv[1]).convert("RGBA")
b = Image.open(sys.argv[2]).convert("RGBA")
if a.size != b.size:
    print("diff=尺寸不同 %s vs %s" % (a.size, b.size))
    sys.exit(0)
arr = np.asarray(ImageChops.difference(a, b))
n = int((arr.sum(axis=2) > 0).sum())
out = sys.argv[1].replace(".png", "") + ".diff.png"
if n:
    Image.fromarray(arr.clip(0, 255).astype("uint8")).save(out)
print("diff=%d maxdelta=%d%s" % (n, int(arr.max()), (" → " + out) if n else ""))
PYEOF
}

fails=0; oks=0; skips=0
while IFS='|' read -r name chart mode value zoom size extra; do
    case "$name" in ''|'#'*) continue ;; esac
    if [ -n "$ONLY" ] && ! echo "$name" | grep -qE "$ONLY"; then continue; fi
    line=$(run_capture "$name" "$chart" "$mode" "$value" "$zoom" "$size" "$extra")
    case "${line%%|*}" in
        SKIP) skips=$((skips + 1)); echo "SKIP $name  ${line#*|*|}"; continue ;;
        FAIL) fails=$((fails + 1)); echo "${line//|/  }"; continue ;;
    esac
    echo "${line//|/  }"
    echo "$line" >> "$CAPDIR/_run.txt"
    if [ "$MODE" = "store" ]; then
        oks=$((oks + 1))
    else
        base="$OUT/$name.png"; cur="$CAPDIR/$name.png"
        if [ ! -f "$base" ]; then
            echo "     FAIL 基线缺失: $base"; fails=$((fails + 1)); continue
        fi
        if cmp -s "$base" "$cur"; then
            oks=$((oks + 1)); echo "     OK   逐字节相同"
        else
            fails=$((fails + 1)); echo "     DIFF $(pixel_diff "$base" "$cur")"
        fi
    fi
done < "$STATES"

echo
if [ "$MODE" = "store" ]; then
    echo "基线已存到 $CAPDIR（OK=$((oks)) SKIP=$skips FAIL=$fails）"
else
    echo "capture-gate: 逐字节相同=$oks  不一致=$fails  跳过=$skips"
fi
[ "$fails" = "0" ] || exit 1
