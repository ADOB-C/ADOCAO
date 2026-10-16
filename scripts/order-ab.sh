#!/bin/bash
# 绘制顺序 A/B（2026-10-15 实验）：层号升序（新，默认） vs 按形状分组（旧，ADOCAO_DRAW_ORDER=grouped）
# 覆盖：三种 Won't You + Thousand Nights Remembered + primer-final（6841 万层）。
# 判定：同一 (谱, 砖, 缩放) 两个顺序的 PNG **逐字节**相同。
set -u
cd "$(dirname "$0")/.."
BIN=build/ADOCAO
OUT=build/gate-scratch/order-ab
mkdir -p "$OUT"

run() {   # name chart tile zoom
  local name="$1" chart="$2" tile="$3" zoom="$4"
  local rss_asc=0 rss_grp=0
  for order in asc grouped; do
    local png="$OUT/${name}_${order}.png"
    if [ "$order" = grouped ]; then
      env ADOCAO_DRAW_ORDER=grouped /usr/bin/time -l "$BIN" "$chart" --capture "$png" \
          --capture-tile "$tile" --capture-zoom "$zoom" --width 1280 --height 720 \
          --no-hitsound --no-progress > "$OUT/${name}_${order}.txt" 2>&1
    else
      /usr/bin/time -l "$BIN" "$chart" --capture "$png" \
          --capture-tile "$tile" --capture-zoom "$zoom" --width 1280 --height 720 \
          --no-hitsound --no-progress > "$OUT/${name}_${order}.txt" 2>&1
    fi
    local rss
    rss=$(grep "maximum resident set size" "$OUT/${name}_${order}.txt" | awk '{print $1}')
    [ "$order" = asc ] && rss_asc=$((rss / 1048576)) || rss_grp=$((rss / 1048576))
  done
  local ca cg
  ca=$(grep "capture: " build/ADOCAO.log | tail -1 | sed 's/.*drawn=/drawn=/')
  if ! cmp -s "$OUT/${name}_asc.png" "$OUT/${name}_grouped.png"; then
    ca=$(cmp "$OUT/${name}_asc.png" "$OUT/${name}_grouped.png" 2>&1 | head -1)
    printf "DIFF %-14s tile=%-10s zoom=%-8s rss=%s/%sMB  %s\n" "$name" "$tile" "$zoom" "$rss_asc" "$rss_grp" "$ca"
    return 1
  fi
  printf "OK   %-14s tile=%-10s zoom=%-8s rss=%s/%sMB  %s\n" "$name" "$tile" "$zoom" "$rss_asc" "$rss_grp" "$ca"
  return 0
}

C="$HOME/Documents/Charts"
fail=0
echo "=== 三种 Won't You（各 2 帧）==="
W1="$C/Won't You Make a Song with Me/level.adofai"
W2="$C/Won't You Make a Song with Me (no three planets)/level_no three planets.adofai"
W3="$C/Seedbean - Won't You Make a Song with Me (Final_Fix)/Won't you make a chart with me_MYC.adofai"
run wy_level   "$W1" 3385456 25  || fail=1
run wy_level_w "$W1" 3385456 1   || fail=1
run wy_np      "$W2" 3188800 25  || fail=1
run wy_np_w    "$W2" 3188800 1   || fail=1
run wy_myc     "$W3" 3385456 25  || fail=1
run wy_myc_w   "$W3" 3385456 1   || fail=1

echo "=== Thousand Nights Remembered（915 万层，3 帧）==="
TNR="$C/Song.adofaiiiii/Quree - Thousand Nights Remembered/Thousand Nights Remembered.adofai.xz"
run tnr   "$TNR" 4577000 25   || fail=1
run tnr_w "$TNR" 4577000 1    || fail=1
run tnr_x "$TNR" 4577000 0.01 || fail=1

echo "=== primer-final（6841 万层，3 帧，最后一帧进深度并列区）==="
PR="$C/Song.adofaiiiii/primer-final.adofai.xz"
run primer   "$PR" 34205538 25     || fail=1
run primer_w "$PR" 34205538 1      || fail=1
run primer_x "$PR" 34205538 0.00001 || fail=1

echo "=== 汇总 ==="
if [ "$fail" = 0 ]; then echo "全部逐字节相同"; else echo "有差异（见上面 DIFF 行）"; fi
exit $fail
