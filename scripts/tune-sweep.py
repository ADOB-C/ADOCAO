#!/usr/bin/env python3
"""可调常数的**单变量扫描**（2026-11-01 用户要求："把所有写死/常量都用所有谱面控制单一变量测一遍"）。

做什么：对每个可调常数（`core/util/Tuning.hpp` 的 `ADOCAO_TUNE`），在**其它一切不变**的前提下
扫一组取值；每个取值跑一遍**全语料**的确定性抓帧，记录：

    png_sha   —— 出图逐字节是否变了（= 这个常数对这张谱有没有可观测影响）
    cull/upload/draw/frame —— 帧内分段耗时（ms）
    drawn     —— 实际画了多少实例
    rss_mb    —— 峰值 RSS

用法：
    python3 scripts/tune-sweep.py                 # 小/中谱（< 50 MB），默认常量表
    python3 scripts/tune-sweep.py --heavy         # 加上 MYC(640MB)/primer(357MB xz)
    python3 scripts/tune-sweep.py --only maxShapeRows,maxPoints
    python3 scripts/tune-sweep.py --charts 4      # 每档只用前 N 张（冒烟）

输出：`build/tune-sweep/<name>.csv` + stdout 的汇总表。**不改仓库里任何文件**。
"""
import argparse
import csv
import glob
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "gate", "adocao")
OUT = os.path.join(ROOT, "build", "tune-sweep")
CHART_ROOTS = [os.path.expanduser("~/Documents/Charts"), os.path.join(ROOT, "tests", "charts")]

# 常量表：name → [取值...]（默认值写在 core 里，这里只列要扫的）
SWEEP = {
    "maxShapeRows":   [512, 1024, 2048, 4096, 8192],
    "maxAnimTiles":   [100000, 500000, 1000000, 4000000],
    "maxKeyframes":   [1000000, 5000000, 20000000],
    "maxPoints":      [1024, 8192, 65536, 262144],
    "rateMax":        [500, 4000, 64000],
    "rangeGrace":     [0.0, 0.02, 0.1, 0.5],
    "cullCand":       [65536, 524288, 4194304],
    "maxInst":        [65536, 262144, 1048576],
    "cullSerialMax":  [1024, 16384, 1048576],
    "denseTrailPts":  [0, 20000, 1000000],
}

# 每个常量要"打到底"需要的额外参数（不然它根本不绑定 —— 第一版就是这么白跑一轮的：
# 默认抓帧参数下 9 个常量的输出**一张谱都没变**，因为它们没被触发）。
STRESS = {
    "maxPoints":     ["--trail-duration", "0.4", "--trail-sample-rate", "262144"],
    "rateMax":       ["--trail-sagitta", "0.5"],
    "rateMin":       ["--trail-sagitta", "0.5"],
    "rangeGrace":    ["--trail-duration", "0.4", "--trail-sample-rate", "65536"],
    "cullCand":      ["--capture-fit-all"],
    "maxInst":       ["--capture-fit-all"],
    "cullSerialMax": ["--capture-fit-all"],
    "denseTrailPts": ["--trail-duration", "0.4", "--trail-sample-rate", "65536"],
}

SEG = re.compile(r"(cull|upload|draw|frame)=([0-9.]+)")
DRAWN = re.compile(r"drawn=(\d+)")
RSS = re.compile(r"^ *(\d+) +maximum resident set size", re.M)


def charts(heavy: bool, limit: int, min_kb: int = 0):
    out = []
    for root in CHART_ROOTS:
        for p in glob.glob(os.path.join(root, "**", "*.adofai"), recursive=True):
            sz = os.path.getsize(p)
            if sz >= 50 * 1024 * 1024 and not heavy:      # MYC(640MB)/primer 是 xz，不走这里
                continue
            if sz >= 400 * 1024 * 1024:
                continue
            if sz < min_kb * 1024:                 # 太小的 diag 谱什么都触发不了
                continue
            out.append((p, sz))
    out.sort(key=lambda t: t[1])
    if limit:
        out = out[:limit]
    return out


def run_one(chart: str, tune: str, extra=None) -> dict:
    png = os.path.join(OUT, "shot.png")
    env = dict(os.environ)
    env["ADOCAO_PROFILE"] = "1"        # 要 frame=/cull= 这些分段
    if tune:
        env["ADOCAO_TUNE"] = tune
    else:
        env.pop("ADOCAO_TUNE", None)
    with open(os.path.join(OUT, "run.time"), "wb") as ft:
        subprocess.run(["/usr/bin/time", "-l", BIN, chart, "--capture", png,
                        "--capture-tile", "100", "--capture-zoom", "25",
                        "--width", "640", "--height", "360", "--no-hitsound"] + (extra or []),
                       env=env, stdout=subprocess.DEVNULL, stderr=ft, check=False)
    log = os.path.join(ROOT, "build", "gate", "adocao.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    segs = dict((k, float(v)) for k, v in SEG.findall(text[::-1][:4000][::-1]))
    drawn = DRAWN.search(text[::-1][:4000][::-1])
    t = open(os.path.join(OUT, "run.time"), "rb").read().decode("utf-8", "replace")
    rss = RSS.search(t)
    sha = ""
    if os.path.exists(png):
        sha = hashlib.sha256(open(png, "rb").read()).hexdigest()[:16]
    return {
        "png": sha,
        "drawn": int(drawn.group(1)) if drawn else -1,
        "rss_mb": round(int(rss.group(1)) / 1048576.0, 1) if rss else -1.0,
        "cull": segs.get("cull", -1), "upload": segs.get("upload", -1),
        "draw": segs.get("draw", -1), "frame": segs.get("frame", -1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heavy", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--charts", type=int, default=0)
    ap.add_argument("--min-kb", type=int, default=0)
    args = ap.parse_args()
    if not os.path.exists(BIN):
        sys.exit(f"缺少 {BIN}（先 scripts/capture-gate.sh 跑一次会复制，或 cp build/adocao build/gate/adocao）")
    os.makedirs(OUT, exist_ok=True)
    names = [n.strip() for n in args.only.split(",") if n.strip()] or list(SWEEP)
    corpus = charts(args.heavy, args.charts, args.min_kb)
    print(f"语料 {len(corpus)} 张；常量 {len(names)} 个\n")

    # 基线只跑一次（每个常量都重跑基线是白烧 10× 的时间）
    base = {}
    for chart, _sz in corpus:
        base[chart] = run_one(chart, "", None)
        print(f"  基线 {os.path.basename(chart)}: drawn={base[chart]['drawn']} "
              f"frame={base[chart]['frame']:.1f}ms png={base[chart]['png']}", flush=True)

    summary = []
    for name in names:
        rows = []
        for chart, _sz in corpus:
            b = base[chart]
            for v in SWEEP[name]:
                ex = STRESS.get(name)
                r = run_one(chart, f"{name}={v}", ex)
                # 基线也要用同一组额外参数，否则"变化"只是在比不同的抓帧参数
                rb = run_one(chart, "", ex) if ex else b
                r.update(chart=os.path.basename(chart), name=name, value=v,
                         changed=int(bool(rb["png"]) and r["png"] != rb["png"]),
                         d_drawn=r["drawn"] - rb["drawn"],
                         d_draw=round(r["draw"] - rb["draw"], 2))
                rows.append(r)
        path = os.path.join(OUT, f"{name}.csv")
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        # 汇总：输出变了多少张、最坏/最好帧时间
        changed = {}
        for r in rows:
            changed[r["value"]] = changed.get(r["value"], 0) + r["changed"]
        frames = {}
        for r in rows:
            if r["frame"] >= 0:
                frames.setdefault(r["value"], []).append(r["frame"])
        parts = []
        for v in SWEEP[name]:
            fr = frames.get(v) or [0]
            parts.append(f"{v}:改{changed.get(v,0)}张/帧中位{sorted(fr)[len(fr)//2]:.1f}")
        summary.append((name, "; ".join(parts)))
        print(f"{name:14s} " + " | ".join(parts))

    print("\n汇总（每个取值：出图变化的谱数 / 帧时间中位 ms）")
    for n, s in summary:
        print(f"  {n:14s} {s}")


if __name__ == "__main__":
    main()
