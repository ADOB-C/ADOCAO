#!/usr/bin/env python3
"""把 ADOCAO.prof（Profiler 的 CSV 流）转成干净的 CSV / 分组 CSV。

.prof 本身是 CSV，但混了四类行（kind=interval/end/segment/frame）和 `#` 注释，
列又有大量空位。这个工具做两件事：

  1) 默认：去掉 `#` 注释行，原样写出一个整齐的 CSV（列 = 文件表头，行 = 全部数据）。
  2) `--split`：按 kind 拆成多个 CSV，每个只保留该 kind 真正有值的列，
     方便直接喂 Excel / pandas（interval 看随时间变化，segment 做透视）。

用法（任何平台，只要 Python 3）：
  python prof_to_csv.py ADOCAO.prof                 # -> ADOCAO.csv
  python prof_to_csv.py ADOCAO.prof -o out.csv
  python prof_to_csv.py ADOCAO.prof --split        # -> ADOCAO.interval.csv / .segment.csv / ...
  python prof_to_csv.py ADOCAO.prof --summary      # 只打印摘要

没有第三方依赖（也就能在 Windows 的 py -3 上直接跑）。
"""
from __future__ import annotations

import argparse
import csv
import os
import statistics
import sys


def read_prof(path: str) -> tuple[list[str], list[dict[str, str]]]:
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            cells = next(csv.reader([line]))
            if header is None:
                header = [c.strip() for c in cells]
                continue
            # 容忍尾随空列 / 列数不足（以后加列也不会炸）
            cells = (cells + [""] * len(header))[: len(header)]
            rows.append({k: v.strip() for k, v in zip(header, cells)})
    if header is None:
        raise SystemExit(f"{path}: 找不到表头（空文件或格式不对）")
    return header, rows


def non_empty_cols(header: list[str], rows: list[dict[str, str]]) -> list[str]:
    return [c for c in header if any(r.get(c, "") != "" for r in rows)]


def write_csv(path: str, cols: list[str], rows: list[dict[str, str]]) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in rows:
            w.writerow([r.get(c, "") for c in cols])


def fnum(r: dict[str, str], key: str) -> float | None:
    try:
        return float(r[key]) if r.get(key, "") != "" else None
    except ValueError:
        return None


def summary(header: list[str], rows: list[dict[str, str]], path: str) -> None:
    kinds: dict[str, int] = {}
    for r in rows:
        kinds[r.get("kind", "?")] = kinds.get(r.get("kind", "?"), 0) + 1
    runs = sorted({r.get("run", "") for r in rows if r.get("run", "")})
    print(f"{path}: {len(rows)} 行，kind={kinds}，run={runs}")
    interval = [r for r in rows if r.get("kind") == "interval"]
    if interval:
        p50 = [v for v in (fnum(r, "work_p50_ms") for r in interval) if v is not None]
        fmax = [v for v in (fnum(r, "frame_max_ms") for r in interval) if v is not None]
        drawn = [v for v in (fnum(r, "drawn_med") for r in interval) if v is not None]
        over = sum(int(fnum(r, "over_16_7") or 0) for r in interval)
        if p50:
            print(f"  interval: {len(interval)} 行  work_p50 中位 {statistics.median(p50):.3f} ms")
        if fmax:
            print(f"            帧最大值 {max(fmax):.1f} ms  drawn 中位 {statistics.median(drawn):.0f}"
                  f"  掉帧(>16.7ms) 合计 {over}")
    seg = [r for r in rows if r.get("kind") == "segment"]
    if seg:
        print("  segment（p50 ms）:")
        for r in sorted(seg, key=lambda r: -(fnum(r, "seg_p50_ms") or 0.0)):
            print(f"    {r.get('segment',''):<14} {fnum(r, 'seg_p50_ms') or 0.0:8.3f}"
                  f"  [{fnum(r, 'seg_min_ms') or 0.0:.3f} .. {fnum(r, 'seg_max_ms') or 0.0:.3f}]")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="ADOCAO.prof -> CSV")
    ap.add_argument("prof", help="ADOCAO.prof 文件")
    ap.add_argument("-o", "--out", help="输出 CSV（默认 <输入名>.csv）")
    ap.add_argument("--split", action="store_true",
                    help="按 kind 拆成 <输入名>.<kind>.csv（每个只留非空列）")
    ap.add_argument("--summary", action="store_true", help="只打印摘要，不写文件")
    ap.add_argument("-q", "--quiet", action="store_true", help="不打印摘要")
    args = ap.parse_args(argv)

    header, rows = read_prof(args.prof)
    if not args.quiet:
        summary(header, rows, args.prof)
    if args.summary:
        return 0

    stem, _ = os.path.splitext(args.prof)
    if args.split:
        kinds = sorted({r.get("kind", "") for r in rows if r.get("kind", "")})
        if not kinds:
            print("没有数据行", file=sys.stderr)
            return 1
        for kind in kinds:
            sub = [r for r in rows if r.get("kind") == kind]
            cols = non_empty_cols(header, sub)
            out = f"{stem}.{kind}.csv"
            write_csv(out, cols, sub)
            print(f"  -> {out}  ({len(sub)} 行，{len(cols)} 列)")
        return 0

    out = args.out or (stem + ".csv")
    write_csv(out, header, rows)
    print(f"  -> {out}  ({len(rows)} 行，{len(header)} 列)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
