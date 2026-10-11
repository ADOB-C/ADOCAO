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

# ---- Kick 和弦的"正确方法"：**恒定 BPM + Twirl 固定大体方向** ----
#
# 参考谱实测（`The Moon - Coal/level.adofai`，就是"能听到和弦"那一族）：
#   BPM 14817.6 **恒定**、600,538 砖、**303,540 个 Twirl**（约每 2 砖一个）、
#   每砖时长中位 4.56 ms（≈ 1 拍）、几何转角均值 92.7°、angleData 均值 157。
#
# 机制（`Timeline.cpp:63-83`，设 `t` = 几何转角、`delta = (180 - t) mod 360`）：
#     CW  : duration = delta/(3 x BPM)     = (180 - t)/(3B)
#     CCW : duration = (360 - delta)/(3B)  = (180 + t)/(3B)
# 同一对角度（同样的 |t|）在 Twirl 两侧给出 `d = c ± |t|/(3B)`（c = 60/BPM 是载波 = 一拍）。
# 于是：**|t| 只由"差多少"决定，符号由 Twirl 决定** —— 恒定 BPM 下就能精确写出任意间隔。
#
# 和弦这里还有一个结构性便利（用户 2026-10-11 指出）：三音取**纯律 4:5:6** 时它们有
# **精确的公共周期**（最小公倍数）T = 1/f0（f0 = 65.4075 Hz → T = 15.2888 ms，周期内
# 4+5+6-1 = 11 个点 / 12 段）。把 Twirl 的符号模式也做成**逐周期重复**，并让每个周期的
# 净转角 = 0，那么：**每个 LCM 边界处朝向都回到同一个值**，砖路成了"逐周期平移"的规则图形
# （"比较规律"），而把整体角度平移一下就能让 **LCM 边界正好落在 180°**（用户要求）。
CHORD_F0 = 261.63 / 4.0                                   # 纯律 4:5:6 的根音 65.4075 Hz
CHORD_NOTES = (4.0 * CHORD_F0, 5.0 * CHORD_F0, 6.0 * CHORD_F0)   # 261.63 / 327.04 / 392.44
CHORD_PERIOD = 1.0 / CHORD_F0                             # 公共周期 15.2888 ms
# BPM 取 `720 x f0`（= 120 / 并集里最大的那个间隙）：于是**每个间隙都恰好落在整数个"拍"上**，
# 而且三种结构自然出现（这就是参考截图里那些形态的来源）：
#   * 间隙 2.548 ms = 2 拍 → `delta ≈ 0` 走 `Timeline.cpp:71` 的特例 → 几何转角 **180°**
#     = **U 形掉头**，砖路沿原路折回 ⇒ "**双线段**"（一条线走两遍）；
#   * 间隙 1.274 ms = 1 拍 → 转角 **0°** = **笔直的条**；
#   * 其余落在 72/108/144/216°（都是 36° 的整数倍）。
# 并集里的所有间隙都是 δ = 1/(60 f0) = 0.25481 ms 的整数倍（2/3/4/5/6/10 倍），
# 所以转角被量化成 36° 的倍数 —— 这正是它看起来"规整"的原因。
CHORD_BPM = 720.0 * CHORD_F0     # = 47093.4；一拍 c = 60/B = 1.2740 ms（= 4 格间隙）
CHORD_SECONDS = 2.5


def _chord_period_gaps():
    """一个公共周期内的并集网格（含回绕的那一段）。"""
    pts = set()
    for k in (4, 5, 6):
        for j in range(1, k):
            pts.add(round(j * CHORD_PERIOD / k, 12))
    pts = sorted(pts)
    return ([pts[0]] + [pts[i] - pts[i - 1] for i in range(1, len(pts))] +
            [CHORD_PERIOD - pts[-1]])


def _chord_period_signs(gaps):
    """逐周期的 Twirl 符号模式。

    目标是**净转角 = ±360°（绕一整圈）**，而不是 0：
      * ±360 → 每个公共周期收成一个**闭合的环**（参考视频里就是一串环：见
        `build/ref/f_11.png`，一圈约 14 砖），环之间沿一个方向漂移 → 弧线；
      * 0 → 每周期净转角为 0，砖路是一条笔直的锯齿线（我们上一版就是这样）。
    两者的"LCM 边界朝向"都回到同一个值（360 ≡ 0 mod 360），所以边界一样能锁 180°。
    """
    B = CHORD_BPM
    # 注意 u=360 那一档（2 拍的特例）：几何转角是 180°，不是 |180-360|=180 —— 两者恰好一样，
    # 所以这里直接取 |180-u| 也对；写成 min 是为了将来 u>360 时不静默出错。
    base = [min(abs(180.0 - 3.0 * B * g), 180.0) for g in gaps]
    n = len(base)
    best = None
    for mask in range(1 << n):
        tot = 0.0
        for i in range(n):
            tot += base[i] if (mask >> i) & 1 else -base[i]
        for tgt in (360.0, -360.0, 0.0):
            err = abs(tot - tgt)
            if err < 1e-6:
                key = (abs(tgt), -err)          # 先要"整整一圈"，再要误差小
                if best is None or key > best[0]:
                    best = (key, mask)
    if best is None:
        raise AssertionError("找不到让一个周期净转角为 0/±360 的符号组合")
    mask = best[1]
    return [1 if (mask >> i) & 1 else -1 for i in range(n)]


def chart_arpeggio(seed=0x9911):
    """**零事件**版本：只有 angleData，没有 Twirl / SetSpeed（回答"他们为什么不用事件"）。

    因为（`Timeline.cpp:63-83`，固定不 Twirl 时）：

        delta    = (180 - Δa) mod 360
        duration = delta/(3B)                  （delta≈0 特例 → 2 拍）

    也就是**时长是角度的纯函数**：给定一拍 c = 60/B，任意 ≤ 2 拍的间隙都能用一个角度写出来，
    不需要任何事件。代价是"往哪边转"也被一起定死了：

        Δa = 180 - 3B x gap      → gap < c 时 Δa > 0，gap > c 时 Δa < 0

    所以只要**把所有间隙都放在载波同侧**（这里取 c = 4f0 的周期 = 最大间隙），
    所有砖就同向弯 ⇒ 砖路自然是一条**平滑的弧**，弧上按间隙大小弯 0°/36°/60° —— 就是
    参考截图里"长弧 + 规律小弯"的形态，而且一行事件都不需要。

    BPM = 240 x f0 → 一拍 = 4f0 的周期（261.63 Hz）：
        4f0 段 → 1 拍 → Δa = 0°（笔直）    5f0 → 0.8 拍 → 36°     6f0 → 2/3 拍 → 60°
    """
    del seed
    B = 240.0 * CHORD_F0
    notes = (4.0 * CHORD_F0, 5.0 * CHORD_F0, 6.0 * CHORD_F0)
    hold = 8
    angles = [0.0]
    for n in notes * 4:
        g = 1.0 / n
        u = 3.0 * B * g
        assert 0.0 < u < 359.5, f"{n:.2f} Hz 的间隙 {g*1e3:.4f} ms 超出 BPM {B} 可达范围"
        for _ in range(hold):
            angles.append(round((angles[-1] + 180.0 - u) % 360.0, 6))
    return angles


# ---- "双线"：两个声部各自持音 + 各自独奏/合奏（参考视频的形态） ----
#
# 为什么这样才有"自然弯曲"：
#   * 载波（一拍 c = 60/BPM）取在**最低音**的周期上 ⇒ 并集里**所有间隙 ≤ c** ⇒
#     转角 Δa = 180 x (1 - gap/c) **全部同号** ⇒ 砖路同向弯（不需要任何 Twirl）；
#   * **持音**（一个音保持若干击）⇒ 一段之内速率恒定 ⇒ 转角恒定 ⇒ 一段直/一段 36°/一段 60°
#     = 长直段 + 规律花环（独奏段落）；
#   * 两个声部**同时**时两条网格交织 ⇒ 间隙变小 ⇒ 转角变大（36~144°）⇒ 密网/小叉
#     —— 参考截图里那几种形态，本质就是"独奏段"与"合奏段"的交替。
DUET_BPM = 240.0 * CHORD_F0          # 一拍 = 4f0 的周期 = 3.8222 ms = 并集里最大的间隙
DUET_HOLD = 8                        # 每个音保持几击
# 段落：(活跃声部, 音高序列)  —— 音高用下标 0/1/2 = 4f0/5f0/6f0
DUET_SECTIONS = (
    ("A",  (0, 1, 2, 1)),
    ("AB", (0, 1, 2, 1)),
    ("B",  (0, 1, 2, 1)),
    ("AB", (2, 1, 0, 1)),
    ("A",  (2, 1, 0, 1)),
    ("AB", (1, 0, 1, 2)),
)


def chart_duet(seed=0x9922):
    """双线赫兹：两个声部各自持音，独奏/合奏交替 —— 零事件（纯 angleData）。"""
    del seed
    notes = (4.0 * CHORD_F0, 5.0 * CHORD_F0, 6.0 * CHORD_F0)
    B = DUET_BPM
    c = 60.0 / B
    # 1) 逐声部把击打时刻铺出来
    voice_times = {"A": [], "B": []}
    for who, motif in DUET_SECTIONS:
        for v in who:
            t0 = voice_times[v][-1] + 1.0 / notes[motif[0]] if voice_times[v] else 0.0
            for idx in motif:
                f = notes[idx]
                for k in range(DUET_HOLD):
                    t = t0 + k / f
                    if not voice_times[v] or t > voice_times[v][-1] + 1e-9:
                        voice_times[v].append(t)
                t0 += DUET_HOLD / f - 1.0 / f + 1.0 / f   # 下一音从本音最后一击之后接上
            voice_times[v] = sorted(set(round(x, 12) for x in voice_times[v]))
    # 2) 合并（一条时间轴 = 一个 planet 的击打序列）
    hits = sorted(set(voice_times["A"]) | set(voice_times["B"]))
    # 3) 间隙 → 角度：Δa = 180 - 3B x gap（所有间隙 ≤ c ⇒ 转角全部同号）
    angles = [0.0]
    for a, b in zip(hits, hits[1:]):
        g = b - a
        u = 3.0 * B * g
        assert 0.0 < u <= 180.0 + 1e-6, f"间隙 {g*1e3:.4f} ms 超出载波（u={u:.2f}）"
        angles.append(round((angles[-1] + 180.0 - u) % 360.0, 6))
    angles.append(round((angles[-1] + 180.0 - 90.0) % 360.0, 6))     # 尾砖
    return angles


# ---- "花瓣扇"：间隙只用 1 拍和 2 拍 ----
#
# 载波 c = 60/BPM，转角 Δa = 180 x (1 - gap/c)：
#     gap = 1 拍 → Δa = 0°     （直段）
#     gap = 2 拍 → u = 360 → 走 `delta≈0` 特例 → Δa = 180°（U 形掉头 = 花瓣的尖）
# 所以只要谱面里**只有这两种间隙**，砖路就只会"直着走"和"折返" —— 直段 + 折返 =
# 一片花瓣；段里如果**每砖还有一点小转角**（跑动的音不是载波那个音），花瓣就会扇开成扇形。
#
# 载波取 `1/(4 f0)`（= 4f0 的周期，261.63 Hz 那一档）：
#   * 尖：用 **2f0**（130.81 Hz，低一个八度）→ 周期 2 拍 → U 形掉头 ✓
#   * 直段：用 **5f0 / 6f0** → 0.8 / 0.667 拍 → 每砖 36° / 60°（段内恒速 ⇒ 弧线）
Petal_BPM = 240.0 * CHORD_F0          # 一拍 = 1/(4 f0) = 3.8222 ms


def chart_petals(seed=0x9933):
    """花瓣扇：每片花瓣 = 6 块 5f0 的砖（每砖 36°）+ 1 块 2f0 的尖（2 拍 → U 形掉头）。

    一片花瓣的净转角 = 6x36 + 180 = 396 ≡ **36°**（mod 360）⇒ 每片相对上一片转过 36°
    ⇒ 10 片正好铺满一圈 = 扇形；花瓣本身细长（段内每砖只转 36°、来回沿同一条窄带）。
    """
    del seed
    B = Petal_BPM
    runTiles, petals = 6, 10
    angles = [0.0]
    u_run = 3.0 * B / (5.0 * CHORD_F0)      # 5f0 → 144 → Δa = 36°
    for _ in range(petals):
        for _ in range(runTiles):
            angles.append(round((angles[-1] + 180.0 - u_run) % 360.0, 6))
        angles.append(round((angles[-1] + 180.0 - 0.0) % 360.0, 6))     # 尖：delta=0 → 2 拍
    angles.append(round((angles[-1] + 180.0 - 90.0) % 360.0, 6))
    return angles


def chart_chord(seed=0x8808):
    """拿 Kick 打出来的 Do-Mi-Gol 和弦：恒定 BPM + Twirl（逐 LCM 周期重复）。"""
    del seed
    gaps = _chord_period_gaps()
    signs = _chord_period_signs(gaps)
    B = CHORD_BPM
    per = len(gaps)
    reps = int(round(CHORD_SECONDS / CHORD_PERIOD))

    angles = [0.0]
    twirls = []
    cw = True                                  # 与 Timeline 的初值一致
    for r in range(reps):
        for i, g in enumerate(gaps):
            u = 3.0 * B * g                    # 这一砖总共要转多少度
            # u 必须落在 (0,360]：360 就是"最长的那一档 = 2 拍"，
            # 在 Timeline 里走 `delta < 0.0001 → relAngle = 360` 的特例（`Timeline.cpp:71`）。
            assert -1e-6 < u <= 360.0 + 1e-6, \
                f"gap {g*1e3:.4f} ms 超出 BPM {B} 的可达范围（u={u:.1f}）"
            trap = abs(u - 360.0) < 1e-6
            if trap:
                # delta 写 0 → 特例 → 2 拍；几何转角 = 180° = **U 形掉头**（"双线段"的来历）。
                # 特例与 Twirl 无关（delta 判定在分支之后），所以这一砖不动符号。
                delta = 0.0
            else:
                tCw = 180.0 - u                # CW 分支下的几何转角（带符号）
                want = float(signs[i])
                useCW = (1.0 if tCw > 0.0 else -1.0) == want
                delta = u if useCW else (360.0 - u)
                if useCW != cw:
                    twirls.append(len(angles))  # Twirl 写在**这一砖**上（floor = 砖号）
                    cw = useCW
            angles.append(round((angles[-1] + 180.0 - delta) % 360.0, 6))

    # 整体平移：让**每个 LCM 边界**正好落在 180°（用户要求"最小公倍数的位置靠近 180 度"）
    boundary = angles[per]                     # 第一个周期结束那一砖
    shift = (180.0 - boundary) % 360.0
    angles = [round((a + shift) % 360.0, 6) for a in angles]

    actions = [{"floor": f, "eventType": "Twirl"} for f in twirls]
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
FIXED_BPM = {"diag_chord": CHORD_BPM, "diag_wav_rate": WAV_RATE * 60.0,
             "diag_arpeggio": 240.0 * CHORD_F0, "diag_duet": DUET_BPM,
             "diag_petals": Petal_BPM}

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
    "diag_arpeggio": chart_arpeggio,
    "diag_duet": chart_duet,
    "diag_petals": chart_petals,
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
        if name in ("diag_chord", "diag_arpeggio", "diag_duet", "diag_petals"):
            # 每砖一击 → 音色与音量写在 settings 里就够（arpeggio 连事件都没有）
            extra = {"hitsound": "Kick", "hitsoundVolume": 50}
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
