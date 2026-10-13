#!/usr/bin/env python3
"""SolidTerm performance bench driver — see scripts/solidterm-bench.sh (the entry point).

Each run launches ONE terminal process on the real display whose shell is a generated runner
script; the runner does the work, writes its own timing to a file, and exits, which closes the
terminal (solidterm quits when its last shell exits; Tilix closes its window). Test-hook cases
(latency / wheel) quit by themselves. While the process lives, /proc/<pid>/task/*/stat is sampled
every 50 ms so CPU can be attributed per thread (main vs QSGRenderThread) over exactly the
measured interval. Nothing here touches any other process: the only PID ever signalled is the
one this script spawned (and only on timeout).
"""
import json
import os
import signal
import statistics
import subprocess
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def loadavg():
    with open("/proc/loadavg") as f:
        return float(f.read().split()[0])


def mem_used_frac():
    info = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":")
            info[k] = int(v.split()[0])
    return (info["MemTotal"] - info["MemAvailable"]) / info["MemTotal"]


def wait_quiet(max_load, timeout=600):
    """Block until the 1-min load average is under max_load (and RAM < 70%)."""
    t0 = time.time()
    while True:
        la = loadavg()
        if la < max_load and mem_used_frac() < 0.70:
            return la
        if time.time() - t0 > timeout:
            return la
        time.sleep(5)


def thread_times(pid):
    """{tid: (comm, utime+stime ticks)} for every thread of pid (and its descendants' main
    threads are NOT included — the shell/cat work is not the terminal's)."""
    out = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except FileNotFoundError:
        return None
    for tid in tids:
        try:
            with open("/proc/%d/task/%s/stat" % (pid, tid)) as f:
                s = f.read()
        except (FileNotFoundError, ProcessLookupError):
            continue
        comm = s[s.index("(") + 1:s.rindex(")")]
        rest = s[s.rindex(")") + 2:].split()
        out[int(tid)] = (comm, int(rest[11]) + int(rest[12]))
    return out


def run_once(argv, env, timeout):
    """Spawn, sample per-thread CPU every 50ms until exit. Returns (samples, wall)."""
    samples = []  # (time.time(), {tid: (comm, ticks)})
    t0 = time.time()
    p = subprocess.Popen(argv, env=env, stdout=subprocess.DEVNULL, stderr=open(env["BENCH_LOG"], "w"))
    while p.poll() is None:
        tt = thread_times(p.pid)
        if tt is not None:
            samples.append((time.time(), tt))
        if time.time() - t0 > timeout:
            p.send_signal(signal.SIGTERM)  # our own child only
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
            break
        time.sleep(0.05)
    return samples, time.time() - t0, p.returncode


def cpu_between(samples, ta, tb):
    """Per-thread-group CPU seconds between wall times ta..tb using the nearest samples."""
    if not samples:
        return {}
    a = min(samples, key=lambda s: abs(s[0] - ta))
    b = min(samples, key=lambda s: abs(s[0] - tb))
    groups = {}
    for tid, (comm, ticks) in b[1].items():
        before = a[1].get(tid, (comm, 0))[1]
        key = "render" if comm.startswith("QSGRender") else ("main" if tid == min(b[1]) else "other")
        groups[key] = groups.get(key, 0.0) + (ticks - before) / HZ
    groups["span"] = b[0] - a[0]
    return groups


def pct(values, q):
    v = sorted(values)
    if not v:
        return float("nan")
    k = (len(v) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def parse_perf(path):
    frames, keys, meta = [], [], {}
    meta_extra = {"KE": {}, "G": []}
    if not os.path.exists(path):
        return frames, keys, meta, meta_extra
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "F":
                frames.append(int(parts[1]))
            elif parts[0] == "K":
                keys.append([int(x) for x in parts[1:]])
            elif parts[0] == "KE":
                meta_extra["KE"][int(parts[1])] = int(parts[2])
            elif parts[0] == "G":
                meta_extra["G"].append(int(parts[1]))
            else:
                meta[parts[0]] = int(parts[1])
    return frames, keys, meta, meta_extra


def tick_stats(ticks, lo_ns, hi_ns, period_ms=5.0):
    """Lateness of a periodic GUI-thread timer inside [lo, hi]: interval - period, in ms."""
    t = [x for x in ticks if lo_ns <= x <= hi_ns]
    late = [max(0.0, (b - a) / 1e6 - period_ms) for a, b in zip(t, t[1:])]
    if not late:
        return {}
    return {"ticks": len(t), "late_p50_ms": pct(late, 0.5), "late_p95_ms": pct(late, 0.95),
            "late_p99_ms": pct(late, 0.99), "late_max_ms": max(late),
            "over_10ms": sum(1 for x in late if x > 10), "over_16ms": sum(1 for x in late if x > 16.7)}


def frame_stats(frames, lo_ns=None, hi_ns=None):
    fr = [f for f in frames if (lo_ns is None or f >= lo_ns) and (hi_ns is None or f <= hi_ns)]
    d = [(b - a) / 1e6 for a, b in zip(fr, fr[1:])]
    if not d:
        return {}
    hist = {"<=8.3": 0, "8.3-16.7": 0, "16.7-25": 0, "25-33": 0, "33-50": 0, ">50": 0}
    for x in d:
        if x <= 8.4: hist["<=8.3"] += 1
        elif x <= 16.8: hist["8.3-16.7"] += 1
        elif x <= 25: hist["16.7-25"] += 1
        elif x <= 33.4: hist["25-33"] += 1
        elif x <= 50: hist["33-50"] += 1
        else: hist[">50"] += 1
    return {
        "frames": len(fr),
        "fps": len(d) / (sum(d) / 1e3),
        "median_ms": statistics.median(d),
        "p95_ms": pct(d, 0.95),
        "p99_ms": pct(d, 0.99),
        "max_ms": max(d),
        "over_16_7": sum(1 for x in d if x > 16.8),
        "hist": hist,
    }


def main():
    cfg = json.loads(sys.argv[1])
    term, case, runs = cfg["term"], cfg["case"], cfg["runs"]
    bdir, out = cfg["dir"], cfg["out"]
    results = []
    for i in range(runs):
        la = wait_quiet(cfg.get("max_load", 2.0))
        timing = os.path.join(bdir, "timing.txt")
        perf_out = os.path.join(bdir, "perf.txt")
        for p in (timing, perf_out):
            if os.path.exists(p):
                os.remove(p)
        env = dict(os.environ)
        env.update({"BENCH_CASE": case, "BENCH_OUT": timing, "BENCH_DIR": bdir,
                    "BENCH_LOG": os.path.join(bdir, "%s-%s-%d.log" % (term, case, i))})
        runner = os.path.join(bdir, "runner.sh")
        if term == "solidterm":
            env.update({"SHELL": runner, "SOLIDTERM_PERF": "1", "SOLIDTERM_PERF_OUT": perf_out,
                        "QT_FORCE_STDERR_LOGGING": "1", "QSG_INFO": "1"})
            if case == "latency":
                env["SOLIDTERM_LATENCYTEST"] = str(cfg.get("keys", 60))
            if case == "wheel":
                env["SOLIDTERM_WHEELTEST"] = str(cfg.get("notches", 600))
            if case == "flood":  # responsiveness during a seq flood: GUI timer + keys from a thread
                env["SOLIDTERM_PERF_GUITICK"] = "1"
                env["SOLIDTERM_FLOODKEYS"] = str(cfg.get("floodkeys", 60))
            argv = [cfg["bin"], "--standalone"]
        elif term == "tilix":
            argv = ["tilix", "--new-process", "--geometry=%s" % cfg["geometry"], "-e", runner]
        elif term == "xterm":
            argv = ["xterm", "-geometry", cfg["geometry"], "-e", runner]
        else:
            sys.exit("unknown term " + term)
        samples, wall, rc = run_once(argv, env, cfg.get("timeout", 300))
        rec = {"run": i, "load": la, "wall": wall, "rc": rc}
        tl = {}
        if os.path.exists(timing):
            with open(timing) as f:
                for line in f:
                    k, v = line.split(None, 1)
                    tl[k] = v.strip()
        rec["timing"] = tl
        if "t0" in tl and "t1" in tl:
            ta, tb = int(tl["t0"]) / 1e9, int(tl["t1"]) / 1e9
            rec["seconds"] = tb - ta
            rec["cpu"] = cpu_between(samples, ta, tb)
        frames, keys, meta, extra = parse_perf(perf_out)
        rec["meta"] = meta
        if frames:
            if "t0" in tl:  # the measured interval only (runner's wall clock → steady clock)
                off = meta.get("realtime_offset", 0)
                rec["frames"] = frame_stats(frames, int(tl["t0"]) - off,
                                            int(tl["t1"]) - off if "t1" in tl else None)
            else:
                rec["frames"] = frame_stats(frames)
        if keys:
            names = ["key", "ptywrite", "ptyread", "damage", "paint0", "paint1", "swap"]
            lat = []
            for ki, k in enumerate(keys):
                if all(k):
                    d = {n: (k[j] - k[0]) / 1e6 for j, n in enumerate(names)}
                    if ki in extra["KE"]:
                        d["keyevent"] = (extra["KE"][ki] - k[0]) / 1e6
                    lat.append(d)
            rec["latency"] = lat
            rec["keys_sent"] = len(keys)
        if extra["G"] and "t0" in tl and "t1" in tl:
            off = meta.get("realtime_offset", 0)
            rec["guitick"] = tick_stats(extra["G"], int(tl["t0"]) - off, int(tl["t1"]) - off)
        with open(env["BENCH_LOG"]) as f:
            log = f.read()
        for line in log.splitlines():
            if "RENDERER:" in line or "render loop" in line or "Created QRhi with backend" in line:
                rec.setdefault("gpu", []).append(line.strip())
        results.append(rec)
        print("  %s %s run %d: load %.2f wall %.2fs %s" % (term, case, i, la, wall,
              ("%.3fs" % rec["seconds"]) if "seconds" in rec else ""), file=sys.stderr)
        time.sleep(cfg.get("gap", 2))
    with open(out, "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
