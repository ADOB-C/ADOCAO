#!/usr/bin/env bash
# 快路径必须吃下**非整数**的 action 字段（并带负向对照，证明 ADOCAO_FAST_REQUIRE 真会触发）。
#
# 背景：parseNumber 曾把"token 正好填满 e"（两个调用方传的 e 都是该值/该区间的真实末尾）误判成
# 截断而返回 false → 所有含小数 action 字段的谱整份退回旧路径：MYC 实测 4.60 s / 4,943 MB，
# 快路径是 1.39 s / 1,473 MB。三路对拍测试永远绿（快路径一放弃，"快路径那次加载"就是旧路径），
# 所以必须由"进程外、顺序无关"的机械护栏来盯。
#
# 为什么不做成进程内自检：它跟前序自检共用进程状态，换工作目录就会假红（踩过）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP="$ROOT/build/ADOCAO"
if [ ! -x "$APP" ]; then echo "缺少 $APP（先构建）"; exit 1; fi
fail=0

# ① 正向：含 bpmMultiplier: 0.5 的谱必须走快路径
if ! ADOCAO_FAST_REQUIRE=1 "$APP" image "$ROOT/build/fp_ok.png" \
        --level "$ROOT/tests/charts/fastpath_decimal.adofai" --size 8x8 >/dev/null 2>&1; then
    echo "✗ 含非整数 action 字段的谱被快路径放弃了（应当吃下）"
    fail=1
fi

# ② 负向对照：非整数 floor 本来就该让快路径放弃（旧路径 GetInt() 会 UB）。
#    临时谱写在 build/ 里，不入库（避免有人拿它去跑旧路径）。
cat > "$ROOT/build/fp_noninteger_floor.adofai" <<'JSON'
{"angleData":[0,90,180],"settings":{"bpm":120},"actions":[{"floor":1.5,"eventType":"Twirl"}],"decorations":[]}
JSON
if ADOCAO_FAST_REQUIRE=1 "$APP" image "$ROOT/build/fp_bad.png" \
        --level "$ROOT/build/fp_noninteger_floor.adofai" --size 8x8 >/dev/null 2>&1; then
    echo "✗ 负向对照失败：非整数 floor 竟被快路径吃下（ADOCAO_FAST_REQUIRE 形同虚设？）"
    fail=1
fi

if [ $fail -eq 0 ]; then echo "✓ 快路径吃下非整数 action 字段（负向对照成立：非整数 floor 被拒）"; fi
exit $fail
