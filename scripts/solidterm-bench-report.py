#!/usr/bin/env python3
"""Summarise a solidterm-bench result dir (JSON per term/case) as Markdown tables."""
import glob
import json
import os
import statistics
import sys


def pct(values, q):
    v = sorted(values)
    if not v:
        return float("nan")
    k = (len(v) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def med(v):
    return statistics.median(v) if v else float("nan")


def main(d):
    rows = []
    lat_rows = []
    frame_rows = []
    gpu = set()
    sizes = set()
    for path in sorted(glob.glob(os.path.join(d, "*.json"))):
        term, case = os.path.basename(path)[:-5].split("-", 1)
        recs = json.load(open(path))
        for r in recs:
            for g in r.get("gpu", []):
                gpu.add(g)
            if "size" in r.get("timing", {}):
                sizes.add("%s %s" % (term, r["timing"]["size"]))
        secs = [r["seconds"] for r in recs if "seconds" in r]
        loads = [r["load"] for r in recs]
        cpu = [r["cpu"] for r in recs if r.get("cpu")]
        def cpu_pct(key):
            return med([100 * c.get(key, 0) / c["span"] for c in cpu if c.get("span")])
        total = med([100 * sum(v for k, v in c.items() if k != "span") / c["span"] for c in cpu if c.get("span")])
        if secs:
            rows.append((term, case, len(secs), med(secs), pct(secs, 0.95), total, cpu_pct("main"),
                         cpu_pct("render"), max(loads)))
        fr = [r["frames"] for r in recs if r.get("frames")]
        if fr:
            frame_rows.append((term, case, len(fr), med([f["fps"] for f in fr]), med([f["median_ms"] for f in fr]),
                               med([f["p95_ms"] for f in fr]), med([f["p99_ms"] for f in fr]),
                               med([f["max_ms"] for f in fr]),
                               med([100.0 * f["over_16_7"] / max(1, f["frames"] - 1) for f in fr]),
                               med([r["meta"].get("paint_ns_total", 0) / max(1, r["meta"].get("paint_count", 1)) / 1e6 for r in recs if r.get("meta")]),
                               med([r["meta"].get("paint_ns_max", 0) / 1e6 for r in recs if r.get("meta")])))
        lat = [k for r in recs for k in r.get("latency", [])]
        if lat:
            stages = ["ptywrite", "ptyread", "damage", "paint0", "paint1", "swap"]
            lat_rows.append((term, case, len(lat), [(s, med([k[s] for k in lat]), pct([k[s] for k in lat], 0.95)) for s in stages]))
    print("### Throughput / CPU (median of runs; CPU = % of one core over the measured interval)\n")
    print("| term | case | runs | median s | p95 s | CPU total % | main % | render % | max load |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| %s | %s | %d | %.3f | %.3f | %.0f | %.0f | %.0f | %.2f |" % r)
    if frame_rows:
        print("\n### Frames (frameSwapped intervals inside the measured interval)\n")
        print("| term | case | runs | fps | median ms | p95 ms | p99 ms | max ms | % intervals > 16.7 ms | updatePaintNode avg ms | max ms |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for r in frame_rows:
            print("| %s | %s | %d | %.1f | %.2f | %.2f | %.2f | %.1f | %.1f | %.3f | %.2f |" % r)
    if lat_rows:
        print("\n### Input latency (ms since keyPressEvent; cumulative; median / p95)\n")
        for term, case, n, st in lat_rows:
            print("%s, %d keys:\n" % (term, n))
            print("| stage | median | p95 |")
            print("|---|---|---|")
            for s, m, p in st:
                print("| %s | %.2f | %.2f |" % (s, m, p))
    if sizes:
        print("\nGrid: " + ", ".join(sorted(sizes)))
    if gpu:
        print("\nRHI/GPU (QSG_INFO):\n")
        for g in sorted(gpu):
            print("    " + g)


if __name__ == "__main__":
    main(sys.argv[1])
