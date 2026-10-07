#!/usr/bin/env python3
"""生成渲染验收谱面（GLSL 几何改造的像素门槛）。

产物（都提交进仓库）：
  tests/charts/angles360.adofai         ~6k 砖：360° 全方向 × 全部几何模式 × 全部图标组合
  tests/charts/shapes_exhaustive.adofai 全 (sa,ea) 有序对（360×360 = 129,600 砖，欧拉回路）
  tests/capture_states.txt              抓帧清单（砖号 → 倍率），由本脚本生成以免索引漂移

为什么按"砖号"而不是"时刻"抓帧：见 app/GameWindow.cpp 的 `--capture-tile`。6 千砖的谱面上
按秒给的时刻会随砖时长累积漂移，按砖给则永远落在同一砖（相机 snap 到它），门槛不受时间轴数学影响。

形状键的复现（必须与 render/TileMesh.cpp / core/level/LevelData.cpp 的 float 语义一致）：
  direction[i] = float32(angleData[i])，angleData[i] == 999 时 = direction[i-1] + 180（float32）
  砖 i 的 sa = i==0 ? -180 : direction[i-1] - 180，ea = direction[i]，mid = angleData[i] == 999
  GeoKey  = (round_half_away(sa*100), round_half_away(ea*100), mid)
模式（ang 单位弧度，见 render/TileGeometry.cpp）：
  ang = a1 - a0 ∈ [0, 360°]；直行（转向 0°）→ ang=180°（big），急转（转向>60°）→ ang<120°（arc），
  U 型（转向 180°）→ ang=0（zero），中旋（999）→ 五边形（pent）

用法：
  python3 tests/gen_render_fixtures.py
"""
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHART_DIR = os.path.join(HERE, "charts")
STATES_PATH = os.path.join(HERE, "capture_states.txt")

PI = 3.14159265


def wrap360(x):
    """对应 TileGeometry.hpp 的 fmodWrap（float32 语义在这里用 python float 足够：只用于选段）"""
    r = x % 360.0
    return r


class Chart:
    def __init__(self, bpm=60.0, name="chart"):
        self.name = name
        self.bpm = bpm
        self.angles = []          # angleData（含 999）
        self.actions = []         # floor 从 0 开始
        self.marks = {}           # 名称 → 砖号（给抓帧清单用）
        self.sections = []        # (名称, 起始砖, 结束砖)

    # ---- 方向序列 -----------------------------------------------------
    def dirs(self, seq, mark=None):
        """追加一串 direction（= angleData 值，999 表示中旋）"""
        start = len(self.angles)
        for d in seq:
            self.angles.append(999 if d == 999 else float(d) % 360.0)
        end = len(self.angles) - 1
        if mark:
            self.marks[mark] = end
            self.marks[mark + "_start"] = start      # 段内取砖要基于起始砖，否则会跑到下一段
        self.sections.append((mark or "?", start, end))
        return start, end

    def walk(self, start_dir, deltas, mark=None):
        """从 start_dir 起按 deltas 逐步转向；返回 (start, end)"""
        seq = []
        d = start_dir
        for delta in deltas:
            d = (d + delta) % 360.0
            seq.append(d)
        return self.dirs(seq, mark)

    # ---- 事件 ---------------------------------------------------------
    def twirl(self, floor):
        self.actions.append({"floor": int(floor), "eventType": "Twirl"})

    def setspeed(self, floor, mul):
        self.actions.append({"floor": int(floor), "eventType": "SetSpeed",
                             "speedType": "Multiplier", "bpmMultiplier": float(mul)})

    # ---- 输出 ---------------------------------------------------------
    def json_text(self):
        doc = {
            "angleData": [int(a) if a == 999 else round(a, 6) for a in self.angles],
            "settings": {
                "version": 15, "bpm": self.bpm, "offset": 0.0, "countdownTicks": 4,
                "zoom": 100.0, "position": [0, 0], "relativeTo": "Player",
                "hitsound": "Kick", "hitsoundVolume": 100.0,
                "trackColor": "debb7b", "secondaryTrackColor": "ffffff",
                "backgroundColor": "000000", "trackStyle": "Standard",
            },
            "actions": self.actions,
            "decorations": [],
        }
        return json.dumps(doc, ensure_ascii=False, separators=(",", ":"))


def build_angles360():
    """360° 全方向 × 全模式 × 全图标。段序即阶段标记（marks 给抓帧清单定位）。"""
    ch = Chart(name="angles360")

    # 1) BIG：转向 1° 的整圈（覆盖全部 360 个方向），位置走成一个 360 边形 → 紧凑
    for d0, mark in [(0.0, "big_step1_a"), (90.0, "big_step1_b")]:
        ch.walk(d0, [1.0] * 360, mark)
    # 更小的/更缓的转向，把 big 分支的 ang 覆盖开
    ch.walk(0.0, [2.0] * 180, "big_step2")
    ch.walk(30.0, [15.0] * 120, "big_step15")
    ch.walk(10.0, [45.0] * 96, "big_step45")

    # 2) ARC：每个基准方向来回 90°（两个手性的四分之一弧，覆盖全部 360 个朝向）
    seq = []
    for k in range(360):
        seq += [k % 360, (k + 90) % 360]
    ch.dirs(seq, "arc90_all")
    for delta, mark in [(70.0, "arc70_all"), (119.0, "arc119_all")]:
        seq = []
        for k in range(0, 360, 5):
            seq += [k % 360, (k + delta) % 360]
        ch.dirs(seq, mark)

    # 3) ZERO：U 型（转向 180°）来回；位置在两个点之间ping-pong，最紧凑
    seq = []
    for k in range(360):
        seq += [k % 360, (k + 180.0) % 360]
    ch.dirs(seq, "zero_uturn_all")

    # 4) 中旋（999）：每个来路方向一个，五边形覆盖全部 360 个朝向
    seq = []
    for k in range(360):
        seq += [k % 360, 999]
    ch.dirs(seq, "midspin_all")

    # 5) 确定性伪随机：形状最多、路径自交最多（剔除/重叠/顺序最敏感的一段）
    rnd = random.Random(20260501)
    spread = [x * 0.25 for x in range(-720, 721)]
    seq = []
    for _ in range(2000):
        seq.append(rnd.choice(spread) % 360.0)
    ch.dirs(seq, "random_mixed")

    # 6) 图标：覆盖在"各种形状"的砖上（twirl/ss-up/ss-down/两者都有/无图标但带 SetSpeed）
    def at(mark, offset):
        """段内第 offset 块砖"""
        return ch.marks[mark + "_start"] + offset

    icons = [
        ("icon_twirl_arc", at("arc90_all", 41), "twirl"),
        ("icon_twirl_big", at("big_step1_a", 40), "twirl"),
        ("icon_twirl_zero", at("zero_uturn_all", 1), "twirl"),      # 奇数下标才是 U 型砖
        ("icon_twirl_midspin", at("midspin_all", 1), "twirl"),      # 奇数下标才是 999 砖
        ("icon_ssup", at("arc90_all", 301), "ssup"),
        ("icon_ssdown", at("arc90_all", 401), "ssdown"),
        ("icon_both_up", at("arc90_all", 501), "both_up"),
        ("icon_both_down", at("arc90_all", 601), "both_down"),
        ("icon_ss_subthreshold", at("arc90_all", 701), "ss_sub"),
        ("icon_random_both", at("random_mixed", 801), "both_up"),
        ("icon_random_down", at("random_mixed", 1201), "ssdown"),
    ]
    for name, floor, kind in icons:
        ch.marks[name] = floor
        if kind in ("twirl", "both_up", "both_down"):
            ch.twirl(floor)
        if kind == "ssup":
            ch.setspeed(floor, 1.5)
        elif kind == "ssdown":
            ch.setspeed(floor, 1.0 / 1.5)
        elif kind == "both_up":
            ch.setspeed(floor, 1.5)
        elif kind == "both_down":
            ch.setspeed(floor, 1.0 / 1.5)
        elif kind == "ss_sub":
            # 1.02 在 ±5% 之内 → tileHasSetSpeed 为真但**没有**图标（图标判据是比值）
            ch.setspeed(floor, 1.02)
    return ch


def build_exhaustive():
    """全 (sa,ea) 有序对：在 360 个方向的完全有向图上取欧拉回路（Hierholzer，确定性）。"""
    n = 360
    adj = [list(range(n)) for _ in range(n)]
    stack = [0]
    circuit = []
    while stack:
        v = stack[-1]
        if adj[v]:
            w = adj[v].pop(0)
            stack.append(w)
        else:
            circuit.append(stack.pop())
    circuit.reverse()          # 顶点序列，长度 n*n+1，首尾同为 0
    assert len(circuit) == n * n + 1, len(circuit)
    edges = len({(circuit[i], circuit[i + 1]) for i in range(n * n)})
    assert edges == n * n, edges
    ch = Chart(name="shapes_exhaustive")
    ch.dirs(circuit, "euler_all_pairs")
    return ch


def shape_stats(angles):
    """按 app 的 float32 语义复现 GeoKey，报告覆盖率（需要 numpy）。"""
    try:
        import numpy as np
    except Exception:
        return None
    ad = np.array([999.0 if a == 999 else float(a) for a in angles], dtype=np.float64)
    f = ad.astype(np.float32)
    is999 = ad == 999.0
    for i in np.nonzero(is999)[0]:
        f[i] = (f[i - 1] if i > 0 else np.float32(0.0)) + np.float32(180.0)
    sa = np.empty(len(ad), dtype=np.float32)
    sa[1:] = f[:-1] - np.float32(180.0)
    sa[0] = np.float32(-180.0)

    def rnd(x):
        x = np.asarray(x, dtype=np.float32)
        return np.where(x >= 0, np.floor(x + np.float32(0.5)), np.ceil(x - np.float32(0.5))).astype(np.int64)

    k0 = rnd(sa * np.float32(100.0))
    k1 = rnd(f * np.float32(100.0))
    keys = np.stack([k0, k1, is999.astype(np.int64)], axis=1)
    uniq = np.unique(keys, axis=0)

    def fmw(x, y):
        r = np.fmod(x, y)
        return np.where(x >= 0, r, r + y)

    aa = fmw(sa - f, np.float32(360))
    bb = fmw(f - sa, np.float32(360))
    cond = aa >= bb
    a0 = np.where(cond, fmw(sa, np.float32(360)), fmw(f, np.float32(360))) * np.float32(PI) / np.float32(180)
    a1 = a0 + np.where(cond, bb, aa) * np.float32(PI) / np.float32(180)
    ang = a1 - a0
    mid = is999
    arc = int(((~mid) & (ang > 0) & (ang < np.float32(2.0943952))).sum())
    big = int(((~mid) & (ang >= np.float32(2.0943952))).sum())
    zero = int(((~mid) & (ang == np.float32(0))).sum())
    return {"tiles": int(len(ad)), "shapes": int(len(uniq)), "arc": arc, "big": big,
            "zero": zero, "midspin": int(mid.sum())}


def write_chart(ch, path):
    text = ch.json_text()
    json.loads(text)                     # 必须是合法 JSON（文档示例吃过这个亏）
    os.makedirs(CHART_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return len(text)


def main():
    os.makedirs(CHART_DIR, exist_ok=True)
    a = build_angles360()
    ex = build_exhaustive()
    sizes = {
        "angles360.adofai": write_chart(a, os.path.join(CHART_DIR, "angles360.adofai")),
        "shapes_exhaustive.adofai": write_chart(ex, os.path.join(CHART_DIR, "shapes_exhaustive.adofai")),
    }

    # 抓帧清单：新 fixture 用砖号；既有 3 个状态沿用时刻（它们是历史基线，不改口径）。
    # 字段用 `|` 分隔（谱面路径带空格）。
    lines = [
        "# 抓帧清单（tests/gen_render_fixtures.py 生成，勿手改）",
        "# 格式: <name>|<chart|ABS>|<tile|time>|<值>|<zoom>|<WxH>",
        "#   tile → --capture-tile（按砖，谱面再长也不漂）；time → --capture-time（历史基线口径）",
        "# fixtures（仓库内，提交）",
    ]
    fixture_states = [
        ("overview_big_fan", "big_step1_a", 359, 6.0),
        ("overview_arc_zone", "arc90_all", 360, 12.0),
        ("arc90_a", "arc90_all", 199, 260.0),
        ("arc90_b", "arc90_all", 341, 260.0),
        ("arc90_c", "arc90_all", 645, 260.0),
        ("arc70", "arc70_all", 71, 260.0),
        ("arc119", "arc119_all", 73, 260.0),
        ("big45", "big_step45", 45, 260.0),
        ("zero_uturn_a", "zero_uturn_all", 1, 260.0),
        ("zero_uturn_b", "zero_uturn_all", 401, 260.0),
        # 中旋段是 [k, 999] 成对：**锚在 999 前一砖**（偶偏移）。锚在 999 本身不行：中旋砖的
        # preExtraRot 会让 m_tileStartTimes 在该处**非单调**（实测同一时刻 findTileIndex 给出
        # 隔壁砖），--capture-tile 的定点就失效。zoom 200 → 中旋砖在 1 单位外，看得清。
        ("midspin_a", "midspin_all", 718, 200.0),
        ("midspin_b", "midspin_all", 318, 200.0),
        ("random_overlap_a", "random_mixed", 900, 200.0),
        ("random_overlap_b", "random_mixed", 1500, 200.0),
    ]
    # 图标：**锚在图标砖的前一砖**（zoom 250 → 图标离画面中心 1 单位 = 250 px，看得清）。
    # 锚在图标砖本身不行：行星（Z=9.5）正好把该砖中心盖住，图标 0.11 一格全被挡住。
    icon_states = [
        ("icon_twirl", "icon_twirl_arc"),
        ("icon_ssup", "icon_ssup"),
        ("icon_ssdown", "icon_ssdown"),
        ("icon_both_up", "icon_both_up"),
        ("icon_both_down", "icon_both_down"),
        ("icon_subthreshold", "icon_ss_subthreshold"),
        ("icon_random_both", "icon_random_both"),
        ("icon_random_down", "icon_random_down"),
    ]

    def tile_of(mark, offset):
        if offset == 0:
            return a.marks[mark]
        return a.marks[mark + "_start"] + offset

    def state(name, chart, mode, value, zoom, extra=""):
        lines.append("%s|%s|%s|%s|%.1f|1280x720|%s" % (name, chart, mode, value, zoom, extra))

    FIXTURE = "tests/charts/angles360.adofai"
    for name, mark, off, zoom in fixture_states:
        state(name, FIXTURE, "tile", str(tile_of(mark, off)), zoom)
    # 一条**真的加载音色**的状态：门槛其余状态都是 --no-hitsound，音色路径得有人覆盖
    state("hitsound_path", FIXTURE, "tile", "359", 6.0, "+hitsounds")

    for name, mark in icon_states:
        # 锚在图标砖**之后 2 砖**（zoom 120）：行星在图标前方，拖尾正好从行星往回扫过图标 ——
        # 专门覆盖"图标不许盖住拖尾"这条。老顺序是"砖 → 拖尾 → 行星 → 图标"，图标不透明、
        # 会把拖尾擦掉一块（The Moon t=1s 实测 97 个像素的 alpha 从 255 变 154~194），那是 bug。
        #   `--trail-tiles 8` 是必须的：默认拖尾是 0.4 s，BPM 60 的谱面上不到一砖，扫不到图标
        state(name + "_under_trail", FIXTURE, "tile", str(a.marks[mark] + 2), 120.0, "--trail-tiles 8")
        # 锚在图标砖本身：行星（Z=9.5）盖住图标 —— 覆盖另一个方向（近处的东西必须赢）
        state(name + "_under_planet", FIXTURE, "tile", str(a.marks[mark]), 250.0)
        state(name, FIXTURE, "tile", str(a.marks[mark] - 1), 250.0)
        state(name + "_wide", FIXTURE, "tile", str(a.marks[mark] - 2), 90.0)
    lines.append("")
    # 本地谱面根：默认 ~/Documents/Charts，可用 ADOCAO_CHARTS 覆盖。
    # **写进清单的一律是 ~ 相对形式**，绝不写机器绝对路径（用户名不该进仓库）。
    CHARTS = os.environ.get("ADOCAO_CHARTS", "~/Documents/Charts")
    lines.append("# 既有 3 个状态（历史基线：The Moon / MYC，机器本地谱面；文件不在就跳过）")
    MOON = f"{CHARTS}/The Moon - Coal/level.adofai"
    MYC = (f"{CHARTS}/Seedbean - Won't You Make a Song with Me (Final_Fix)/"
           "Won't you make a chart with me_MYC.adofai")
    state("moon_t1_z100", MOON, "time", "1.0", 100.0)
    state("moon_t1_z25", MOON, "time", "1.0", 25.0)
    state("myc_t30_z25", MYC, "time", "30.0", 25.0)
    lines.append("")
    lines.append("# 形状压力：形状数最多的本地谱面（今天每形状一次 draw → 这里最糟）")
    state("singularity_mid", f"{CHARTS}/15. Singularity at 2.64e+6 BPM/"
                             "15. Singularity at 2.64e+6 BPM.adofai", "tile", "500000", 25.0)
    with open(STATES_PATH, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")

    for name, size in sizes.items():
        st = shape_stats(a.angles if name.startswith("angles360") else ex.angles)
        print("%-26s %8d B  %s" % (name, size, st if st else "tiles=%d" % len(a.angles)))
    print("%-26s %8d B" % ("capture_states.txt", os.path.getsize(STATES_PATH)))
    print("angles360 段:", ", ".join("%s=%d..%d" % s for s in a.sections[:6]), "… 共", len(a.sections), "段")
    for k in ("arc90_all", "zero_uturn_all", "midspin_all", "random_mixed",
              "icon_twirl_arc", "icon_ssup", "icon_ssdown", "icon_both_up", "icon_ss_subthreshold"):
        print("  mark %-22s tile=%d" % (k, a.marks[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
