#!/usr/bin/env python3
"""生成 `.adocao` v3 判据用的一组测试谱（**确定性**：同一个脚本永远出同一批字节）。

为什么要专门的测试谱：真实谱面（MYC / The Moon）每种结构只占一种，没法把
`docs/adocao-format.md` §8.4 的判据逐条拆开验。这里按**结构**造七张小谱：

    diag_dict       小字母表 + 随机 + 长短游程     → Dict / Rle 该赢
    diag_glide      环上的滑音（转向率缓变）        → WrapDelta(mod 360) 该赢
    diag_irregular  连续均匀分布（全精度）          → 无解，Raw + zstd 地板
    diag_fourier    六谐波、周期 96（量化到 1e-6）  → Motif 该赢（**必须逐位重复**）
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

N = 16384          # 主规模：正好 4 个 4096 块（v3 的分块/checkpoint 间隔）
N_IRREG = 8192     # 不规则谱全精度，压到 8192 层免得文件太大


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
    """六谐波、公共周期 96，量化到 1e-6。

    六个分音周期 96/48/32/24/16/12 **都整除 96** —— 这是 `Motif` 能成立的前提：
    第 i 层与第 i+96 层的**存储值逐位相同**（量化把 1e-15 的浮点差抹平）。
    """
    del seed
    harmonics = [(96, 30.0), (48, 20.0), (32, 12.0), (24, 8.0), (16, 5.0), (12, 3.0)]
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


def chart_mixed(n=N, seed=0x6606):
    """分段型：滑音 / 不规则 / 字典 / 傅里叶，每段 4096 层。

    专门给 v3 的分块（§8.3）用：全局一个 codec 只能迁就最差的那段，
    按 4096 分块则每段各自选。
    """
    seg = n // 4
    return (chart_glide(seg, seed) + chart_irregular(seg, seed + 1) +
            chart_dict(seg, seed + 2) + chart_fourier(seg, seed + 3))


def chart_overlay(n=4096, seed=0x7707):
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


CHARTS = {
    "diag_dict": chart_dict,
    "diag_glide": chart_glide,
    "diag_irregular": chart_irregular,
    "diag_fourier": chart_fourier,
    "diag_midspin": chart_midspin,
    "diag_mixed": chart_mixed,
    "diag_overlay": chart_overlay,
}

# 谱面骨架：只要 angleData + settings 就能加载（见 tests/charts/fastpath_decimal.adofai）
SETTINGS = {
    "version": 15,
    "song": "adocao diagnostics",
    "artist": "generated",
    "bpm": 120.0,
    "offset": 0,
    "trackColor": "debb7b",
    "backgroundColor": "000000",
}


def write_chart(path, angles):
    obj = {"angleData": angles, "settings": SETTINGS, "actions": [], "decorations": []}
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
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    print(f"{'谱':<16} {'层数':>7} {'不同值':>8} {'文件':>10}  说明")
    for name, fn in CHARTS.items():
        angles = fn()
        path = os.path.join(args.out, name + ".adofai")
        size = write_chart(path, angles)
        distinct = len(set(angles))
        sentinels = sum(1 for a in angles if a == 999.0)
        note = f"哨兵 {sentinels}" if sentinels else ""
        print(f"{name:<16} {len(angles):>7} {distinct:>8} {size:>9} B  {note}")
    print(f"\n→ {args.out}")


if __name__ == "__main__":
    main()
