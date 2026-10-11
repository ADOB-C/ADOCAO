#!/usr/bin/env python3
"""生成 `.adocao` v3 判据用的一组测试谱（**确定性**：同一个脚本永远出同一批字节）。

为什么要专门的测试谱：真实谱面（MYC / The Moon）每种结构只占一种，没法把
`docs/adocao-format.md` §8.4 的判据逐条拆开验。这里按**结构**造七张小谱：

    diag_dict       小字母表 + 随机 + 长短游程     → Dict / Rle 该赢
    diag_glide      环上的滑音（转向率缓变）        → WrapDelta(mod 360) 该赢
    diag_irregular  连续均匀分布（全精度）          → 无解，Raw + zstd 地板
    diag_fourier    大七和弦（纯律 5/4/3/2/15/8），周期 120 → 频谱是六条谱线；Motif 该赢
    diag_midspin    滑音 + 每 8 层一个 `999` 哨兵   → 哨兵掩码的**负向对照**
    diag_mixed      滑音/不规则/字典/傅里叶 各 4096 → 分段型（v3 分块）
    diag_overlay    闭合单元重复（4 砖方块 x1024）  → 叠加型（空间自重叠率高）

两条硬约束（错了就不是"无损容器"的测试谱）：

  * **确定性**：随机一律走脚本里那个 LCG（不用 `random`，别让 Python 版本决定字节）。
  * **量化**：除 `diag_irregular` / `diag_mixed` 的不规则段外，角度都量化到定点网格
    （4~6 位小数）。这不只是为了文件小 —— `Motif`/`Dict` 要的是**逐位重复**：
    全精度的正弦每层都差 1 ULP，窗口哈希永远不重复，`Motif` 必然失效。

用法：
    python3 tests/gen_adocao_charts.py            # 写进 tests/charts/
    python3 tests/gen_adocao_charts.py --out DIR  # 写到别处（如 build/gate-scratch）
"""

import argparse
import json
import math
import os

# ---- 确定性伪随机（LCG，数值取自 Numerical Recipes）----

_MASK = (1 << 64) - 1


class Lcg:
    def __init__(self, seed):
        self.s = seed & _MASK

    def next_u32(self):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & _MASK
        # 取**高** 32 位：`>> 33` 只剩 31 位，uniform() 于是永远只覆盖量程的一半
        # （真踩过：diag_irregular 的"均匀 0~360"实际全挤在 0~180）
        return (self.s >> 32) & 0xFFFFFFFF

    def uniform(self, lo, hi):
        return lo + (hi - lo) * (self.next_u32() / 4294967296.0)

    def below(self, n):
        return self.next_u32() % n


def mod360(x):
    return x % 360.0


# ---- 七张谱 ----

# 规模：**砖数要小**。为什么不是 16384：
#   ① 没有音频文件时，播放器把整谱的打拍音预混成一段 PCM，缓冲区正比于**总时长**；
#   ② 想"每一下都听得清"就需要每砖 ~0.2 s，而 16384 砖 x 0.2 s = 55 分钟 → 预混 1.1 GB。
# 两头不能都要，所以取 1024 砖 + 每砖 0.2 s（整谱 ~3.5 分钟、预混 ~60 MB、每秒 3~6 下）。
# 1024 仍然够诊断用：周期检测最大 p=120（见 diag_fourier），窗口数 905 个、塌 7.5 倍。
N = 1024           # 单结构谱
N_IRREG = 1024     # 不规则谱（全精度，值几乎不重复）
N_OVERLAY = 512    # 叠加谱（闭合单元 x128）
N_MIXED = 2048     # 分段谱：4 x 512


def chart_dict(n=N, seed=0x1101):
    """字典主导：10 个取值 + 几何分布的游程（游程一多，Rle 也会来竞争）。"""
    alphabet = [0.0, 22.5, 45.0, 67.5, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]
    rng = Lcg(seed)
    out = []
    while len(out) < n:
        v = alphabet[rng.below(len(alphabet))]
        # 游程长度 1~16（平均 4）：P(len=k) ~ 3/4^k
        run = 1
        while run < 16 and rng.below(4) != 0:
            run += 1
        out.extend([v] * run)
    return out[:n]


def chart_glide(n=N, seed=0x2202):
    """螺旋/滑音：恒定的**无理**转向率 + 缓慢调制，4 位小数。

    转向率取 137.508°（黄金角）：360/137.508 不是有理数 → 值几乎不重复（Dict 赢不了），
    而且每 2~3 层就跨一次 0/360（环绕率 ~38%）→ raw 的 Δ² 在每次跨环时冒出 ±360 的尖峰，
    wrapped 之后只剩调制那点曲率。**这才是 `WrapDelta` 的目标谱型。**

    反例（也值得知道）：Δ=0.37°/层的"慢滑音"环绕率只有 0.1%，`WrapDelta` 换来的塌陷
    只有 ~4 倍 —— 那种谱 `Delta → Dict` 就够了（见 §8.5）。
    """
    del seed
    period = 5000.0
    ang = 0.0
    out = []
    for i in range(n):
        delta = 137.508 + 3.0 * math.sin(2.0 * math.pi * i / period)
        ang = mod360(ang + delta)
        out.append(round(ang, 4))
    return out


def chart_irregular(n=N_IRREG, seed=0x3303):
    """纯不规则：连续均匀分布、全精度（shortest round-trip）。

    值集基数 ≈ n、任何差分都是高熵 —— 这就是"地板由生成器决定"那张谱。
    """
    rng = Lcg(seed)
    return [rng.uniform(0.0, 360.0) for _ in range(n)]


def chart_fourier(n=N, seed=0x4404):
    """**一个大七和弦**（纯律 C-E-G-B），公共周期 120，量化到 1e-6。

    六个分音写成"相对基频的谐波号" h（`一周期内的圈数 = h`），周期 = 120/h：

        h =  2 → 周期 60  根音（低八度）
        h =  4 → 周期 30  根音（八度）
        h =  8 → 周期 15  根音
        h = 10 → 周期 12  大三度 10/8 = 5/4
        h = 12 → 周期 10  纯五度 12/8 = 3/2
        h = 15 → 周期  8  大七度 15/8

    于是频谱上是**六条固定音高的谱线**（和弦），而不是滑音那种扫频带；而且是**纯律**
    （5/4、3/2、15/8 都是小整数比），不是十二平均律的近似。

    所有周期都整除 120 —— 这是 `Motif` 能成立的前提：第 i 层与第 i+120 层的**存储值逐位相同**
    （量化到 1e-6 把 1e-15 的浮点差抹平）。
    """
    del seed
    harmonics = [(60, 30.0), (30, 18.0), (15, 24.0), (12, 20.0), (10, 14.0), (8, 10.0)]
    out = []
    for i in range(n):
        a = 180.0
        for period, amp in harmonics:
            a += amp * math.sin(2.0 * math.pi * i / period)
        out.append(round(mod360(a), 6))
    return out


def chart_midspin(n=N, seed=0x5505):
    """滑音 + 每 8 层一个 midspin 哨兵（`999`）。

    **负向对照**：不掩码时相邻对里 999↔真角度会造出大量假的 |Δ|>180，
    环绕率被抬到 ~25%；掩码之后应该回落到滑音本身的真实水平。
    """
    base = chart_glide(n, seed)
    out = list(base)
    for i in range(3, n, 8):
        out[i] = 999.0
    return out


def chart_mixed(n=N_MIXED, seed=0x6606):
    """分段型：滑音 / 不规则 / 字典 / 傅里叶，每段 512 层（`n/4`）。

    专门给 v3 的分块（§8.3）用：全局一个 codec 只能迁就最差的那段，分块则每段各自选。
    ⚠️ 512 层的段**看不到 p=120**（窗口数 393 < 8x120）—— 周期检测用独立的 `diag_fourier` 看。
    """
    seg = n // 4
    return (chart_glide(seg, seed) + chart_irregular(seg, seed + 1) +
            chart_dict(seg, seed + 2) + chart_fourier(seg, seed + 3))


def chart_overlay(n=N_OVERLAY, seed=0x7707):
    """叠加型：每 4 层是一个**闭合方块**，重复 n/4 次。

    `angleData` 是每一砖的**绝对**方向（`calculateTilePositions` 里
    `direction = angleData[i]`、步长恒为 1 砖），所以闭合方块要写 0/90/180/270
    而不是恒定的 90 —— 恒定 90 是一条直线（我最先就是这么写错的，重叠率报 0）。

    砖位于是**反复回到同一批格子**（空间自重叠率 → 1），但 angleData 只有 4 个取值
    —— §5.5 那种"单元 × 摆放"的最小模型。
    """
    del seed
    unit = [0.0, 90.0, 180.0, 270.0]
    return [unit[i % 4] for i in range(n)]


# ---- 两种"能听见和弦"的写法 ----
#
# ① `diag_chord`：**拿 Kick 做对**（跟别人那张"能听到和弦"的谱一路子）。
#    打拍音每砖一次，听感音高 = **重复频率**，所以要出和弦就得让击打时刻落在
#    三个频率网格的**并集**上（三条脉冲列相加 = 三条谱线）。
#    时刻怎么控制？`Timeline.cpp:63-83` 给了公式（CW 时）：
#        delta    = (angleData[i-1] + 180 - angleData[i]) mod 360
#        duration = (delta/360) x 2 x (60/BPM)         （delta≈0 特例成 360，即两拍）
#    反解就是 angleData[i] = (angleData[i-1] + 180 - 3 x BPM x gap_i) mod 360。
#    于是**角度序列 = 相位的累积**：方向转得快的地方 = 音高，转得慢的地方 = 低音；
#    砖路自然画成一圈圈花环（这正是参考谱的样子），而耳朵听到的就是那段和弦。
# BPM 决定"每砖能转多少"：duration = (delta/360) x 2 x (60/BPM)，反解
#     delta(度) = 3 x BPM x gap
# 所以 **BPM 越低，每砖转角越小、砖路越舒展**（不是提高 BPM！）：
#   BPM = 30000 → 最大间隙 3.8 ms 要转 344° → 方向几乎每砖翻一圈 → 画成一坨实心饼；
#   BPM =  1000 → 同一间隙只转 11.5° → 半径 ~5 砖的花环，一圈圈铺开（参考谱的样子）。
# 音高只看击打时刻，跟 BPM 无关 —— 所以调 BPM 只改"看相"，不改"听感"。
CHORD_SECONDS = 2.5
CHORD_NOTES = (261.63, 329.63, 392.00)   # C4 E4 G4 —— 十二平均律的 Do-Mi-Gol
CHORD_MIN_GAP = 0.00035      # 比这更近的并集点直接并掉（<0.35 ms 耳朵分不开）

# 砖路画什么：一条**慢速版**的和弦波形（Hz，比真正的和弦低 5 个八度）。
# 它只决定"看相"，不影响声音；取这么低是为了让相邻砖的角度差很小（→ 砖路舒展）。
# 实测过的教训：想让砖路舒展，**不是**把 BPM 调高，而是让"每砖转角 ≈ 180°（= 走直线）"，
# 也就是**把时刻从角度里拿出来**（逐砖 SetSpeed），角度只负责画画。
CHORD_DRAW_NOTES = (4.1, 5.15, 6.1)


def chart_chord(seed=0x8808):
    """拿 Kick 打出来的 Do-Mi-Gol 和弦：**角度画画、逐砖 SetSpeed 管时刻**。

    为什么不能"角度管时刻"（两轮实测换来的）：
      * 时长公式 `duration = (delta/360) x 2 x (60/BPM)`，其中
        `delta = (angleData[i-1] + 180 - angleData[i]) mod 360`；
      * "每砖走直线"（delta ≈ 180，即**平均角度 ≈ 180**）等价于**每砖恰好一拍**
        → 击打间隔恒定 → 只有**一个音高**，出不了和弦；
      * 和弦要求间隔在 2.5~3.8 ms 间变 → delta 得跟着变 ±25% → 砖路必然卷起来
        （之前那两张"没展开"的图就是这么来的）。
    所以两件事分开：角度写一条**慢速和弦波形**（均值 180、相邻差小 → 舒展花环），
    时刻交给逐砖 `SetSpeed` —— 而且 BPM 要**按 delta 反算补偿**
    （`BPM_i = delta_i / (3 x gap_i)`，见下面注释），否则时长会被角度差缩放掉。
    """
    del seed
    times = set()
    for f in CHORD_NOTES:
        k = 0
        while k / f <= CHORD_SECONDS:
            times.add(round(k / f, 9))
            k += 1
    ts = sorted(times)
    keep = [ts[0]]
    for t in ts[1:]:
        if t - keep[-1] >= CHORD_MIN_GAP:
            keep.append(t)
    n = len(keep)

    def drawn(t):
        smp = sum(math.sin(2.0 * math.pi * f * t) for f in CHORD_DRAW_NOTES) / len(CHORD_DRAW_NOTES)
        return round(180.0 + 180.0 * 0.9 * smp, 6)

    def delta(a_prev, a_cur):
        d = (a_prev + 180.0 - a_cur) % 360.0
        return 360.0 if d < 0.0001 else d          # 与 Timeline 的特例一致

    # n+1 块：tile i 的**起点**是 keep[i]（0 起算）→ 最后一个网格点也有砖起跳
    angles = [drawn(t) for t in keep]
    angles.append(drawn(keep[-1] + 1.0 / CHORD_NOTES[-1]))

    # tile i（1..n）的时长应当是 gap_{i-1} = keep[i] - keep[i-1]：
    #   duration = delta_i/(3 x BPM_i)  ⇒  BPM_i = delta_i / (3 x gap)
    actions = []
    for i in range(1, n):
        gap = keep[i] - keep[i - 1]
        d = delta(angles[i - 1], angles[i])
        actions.append({"floor": i, "eventType": "SetSpeed", "speedType": "Bpm",
                        "beatsPerMinute": round(d / (3.0 * gap), 6)})
    return angles, actions


# ② `diag_wav_rate`：audio-as-chart（raw-PCM 直通）—— 整谱就是波形本身。
#    1 层 = 1 采样、采样率 = bpm/60、样值 = hitsoundVolume/50（`Timeline::buildRawPcm`）。
#    角度取**恒定** 90：实测只有"相对转角 0"才是恰好一拍，时间轴才与采样时钟一致。
#    注意这条路要显式开 `--force-hitsound raw-pcm`（或启动器里选 raw-pcm），
#    不像 ① 那样直接就能听。
# 采样率要**高**：角度列写的是波形，相邻砖的转角就是相邻采样的差 ——
# 44.1 kHz 时一个采样只差 ~9°，砖路才是"展开"的波形；4 kHz 时一砖差 ~100°，
# 画出来是粗锯齿（而且时间轴会明显偏离"每砖一拍"）。真谱（Unity.wav_rate 一族）
# 就是 44.1/48/96/192 kHz。这里取 16 kHz 折中（0.4 s = 6400 砖，文件 ~200 KB）。
WAV_RATE = 16000.0
WAV_SECONDS = 0.4
WAV_NOTES = CHORD_NOTES


def chart_wav_rate(seed=0x9909):
    """audio-as-chart 的 Do-Mi-Gol：每砖一个采样。

    **两列都写波形**（参考谱就是这么写的）：
      * `hitsoundVolume` = 样值 → 声音（Kick 按采样率叠出来 = 波形；raw-pcm 直通更是原样）；
      * `angleData` = **180 + 180 x 样值** → 砖路把波形画出来，而且**平均角度 ≈ 180**
        （波形的均值是 0）—— 这就是"展开"的判据：转角 = 180 - 相邻样值差，
        采样率高时相邻差很小，于是砖路是一条舒展的波形曲线，而不是一坨乱线。
    """
    del seed
    n = int(WAV_RATE * WAV_SECONDS)
    angles, actions = [], []
    for i in range(n):
        t = i / WAV_RATE
        env = min(1.0, t / 0.02, (WAV_SECONDS - t) / 0.02)
        smp = sum(math.sin(2.0 * math.pi * f * t) for f in WAV_NOTES) / len(WAV_NOTES)
        smp *= 0.9 * max(0.0, env)
        angles.append(round(180.0 + 180.0 * smp, 6))
        if i > 0:
            actions.append({"floor": i, "eventType": "SetHitsound",
                            "hitsoundVolume": round(smp * 50.0, 3)})
    return angles, actions


# 这些谱的 BPM 由别的东西决定，不能按"目标时长"反推
FIXED_BPM = {"diag_chord": 1000.0, "diag_wav_rate": WAV_RATE * 60.0}

CHARTS = {
    "diag_dict": chart_dict,
    "diag_glide": chart_glide,
    "diag_irregular": chart_irregular,
    "diag_fourier": chart_fourier,
    "diag_midspin": chart_midspin,
    "diag_mixed": chart_mixed,
    "diag_overlay": chart_overlay,
    "diag_chord": chart_chord,
    "diag_wav_rate": chart_wav_rate,
}

# 谱面骨架：只要 angleData + settings 就能加载（见 tests/charts/fastpath_decimal.adofai）
SETTINGS = {
    "version": 15,
    "song": "adocao diagnostics",
    "artist": "generated",
    "bpm": 120.0,     # 由 write_chart() 按 --seconds/--bpm 覆盖
    "offset": 0,
    "trackColor": "debb7b",
    "backgroundColor": "000000",
}

# 默认让每张谱**整谱 ~180 秒**。为什么不是 120 BPM：这些谱没有音频文件，播放器于是走
# "把整谱的打拍音预混成一段 PCM"那条路（`HitsoundManager::preSynthesize`），缓冲区大小
# 正比于**总时长** —— 16384 层 @120 BPM = 8192 s → 实测峰值 RSS **2.26 GB**（抓帧探针量过），
# 而且要先算完那一大段才出声。按目标时长反推 BPM（BPM = 层数 × 60 / 秒数）后整谱 ~180 s、
# 缓冲 ~60 MB，按下播放立刻能听见；每砖 ~0.2 s（每秒 3~6 下，**每一下都分得开**）。
# 反面教材：曾经用 16384 砖 + 120 s，等于 137 下/秒 —— 听感是一堵"蚊子声"音墙。
DEFAULT_SECONDS = 180.0


def write_chart(path, angles, bpm, actions=None, extra_settings=None):
    settings = dict(SETTINGS, bpm=bpm)
    if extra_settings:
        settings.update(extra_settings)
    obj = {"angleData": angles, "settings": settings,
           "actions": actions or [], "decorations": []}
    # separators 去掉多余空格；float 走 repr（shortest round-trip），于是"量化过的值"
    # 打出来的字符串也短（22.5 而不是 22.500000000000000）
    text = json.dumps(obj, separators=(",", ":"), ensure_ascii=False)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return len(text)


def main():
    ap = argparse.ArgumentParser()
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "charts")
    ap.add_argument("--out", default=default_out, help="输出目录（默认 tests/charts/）")
    ap.add_argument("--seconds", type=float, default=DEFAULT_SECONDS,
                    help=f"目标整谱时长（默认 {DEFAULT_SECONDS:g} s；BPM = 层数 x 60 / seconds）")
    ap.add_argument("--bpm", type=float, default=0.0,
                    help="直接给 BPM（给了就忽略 --seconds）")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    print(f"{'谱':<16} {'层数':>7} {'BPM':>9} {'时长':>8} {'不同值':>8} {'文件':>10}  说明")
    for name, fn in CHARTS.items():
        out = fn()
        angles, actions = out if isinstance(out, tuple) else (out, None)
        n = len(angles)
        bpm = FIXED_BPM.get(name)
        if bpm is None:
            bpm = args.bpm if args.bpm > 0 else round(n * 60.0 / args.seconds)
        # audio-as-chart 要"整谱默认静音 + 逐砖给样值"（第 0 层没事件 → 取默认值）
        if name == "diag_chord":
            # 每砖一击 → 音色与音量直接写在 settings 里就够（不用逐砖事件）
            extra = {"hitsound": "Kick", "hitsoundVolume": 8}
        elif actions:
            extra = {"hitsoundVolume": 0}                        # audio-as-chart：默认静音
        else:
            extra = None
        path = os.path.join(args.out, name + ".adofai")
        size = write_chart(path, angles, bpm, actions, extra)
        distinct = len(set(angles))
        sentinels = sum(1 for a in angles if a == 999.0)
        secs = n * 60.0 / bpm
        note = f"哨兵 {sentinels}" if sentinels else ""
        print(f"{name:<16} {n:>7} {bpm:>9.0f} {secs:>7.1f}s {distinct:>8} {size:>9} B  {note}")
    print(f"\n→ {args.out}")


if __name__ == "__main__":
    main()
