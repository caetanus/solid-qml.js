#!/usr/bin/env python3
"""SolidTerm stall soak: repeat bench throughput cases many times and capture state on any stall.

  scripts/solidterm-soak.py <solidterm-binary> <outdir> case:count [case:count ...]

Uses the bench runner (scripts/solidterm-bench.sh writes it; run `solidterm-bench.sh <bin>
--runner-only` once first) and the same environment as the bench (SOLIDTERM_PERF on). Each run
also sets SOLIDTERM_STALLWATCH: the app's watchdog (apps/solidterm/native/stallwatch.h) appends a
report when a pty session stops consuming its master fd (pending bytes and no read for 5 s). On
such a report, or when a run exceeds TIMEOUT seconds (default 120), the soak captures:
  - the watchdog report (pending bytes via FIONREAD, pause flag, notifier state, GUI heartbeat);
  - `gdb -p` backtraces of all threads (the app allows the attach via PR_SET_PTRACER_ANY);
  - /proc/<pid>/task/*/{stat,wchan} of the terminal and the state/wchan of every descendant
    (the shell, cat/seq);
then kills only the process it spawned. Run it against a headless sway on the real GPU (see
docs/evidence/solidterm/perf-2026-10-13.md); every spawned terminal quits by itself.
Env: TIMEOUT, MAX_LOAD (default 2.5), GAP (seconds between runs, default 1).
"""
import json
import os
import signal
import subprocess
import sys
import time

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


def wait_quiet(max_load):
    t0 = time.time()
    while time.time() - t0 < 300:
        if loadavg() < max_load and mem_used_frac() < 0.70:
            return
        time.sleep(3)


def descendants(pid):
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        try:
            with open("/proc/%d/task/%d/children" % (p, p)) as f:
                kids = [int(x) for x in f.read().split()]
        except OSError:
            kids = []
        out.extend(kids)
        todo.extend(kids)
    return out


def proc_line(pid):
    def rd(leaf):
        try:
            with open("/proc/%d/%s" % (pid, leaf)) as f:
                return f.read().strip()
        except OSError:
            return "?"
    stat = rd("stat")
    state = stat[stat.rfind(")") + 2] if ")" in stat else "?"
    return "pid %d state %s wchan %s cmd %s" % (pid, state, rd("wchan"), rd("cmdline").replace("\0", " "))


def capture(pid, outdir, tag):
    path = os.path.join(outdir, "stall-%s.txt" % tag)

    def switches():
        out = {}
        try:
            for tid in os.listdir("/proc/%d/task" % pid):
                with open("/proc/%d/task/%s/status" % (pid, tid)) as s:
                    for line in s:
                        if line.startswith(("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches")):
                            out[(tid, line.split(":")[0])] = int(line.split()[1])
        except OSError:
            pass
        return out
    sw0 = switches()
    time.sleep(2)
    sw1 = switches()
    with open(path, "w") as f:
        f.write("== context switches of each thread over 2 s (a thread that is not stuck keeps switching)\n")
        for (tid, kind), v in sorted(sw1.items()):
            if kind.startswith("voluntary"):
                f.write("tid %s voluntary +%d nonvoluntary +%d\n" % (
                    tid, v - sw0.get((tid, kind), v),
                    sw1.get((tid, "nonvoluntary_ctxt_switches"), 0) - sw0.get((tid, "nonvoluntary_ctxt_switches"), 0)))
        f.write("== terminal threads\n")
        try:
            for tid in sorted(os.listdir("/proc/%d/task" % pid), key=int):
                with open("/proc/%d/task/%s/stat" % (pid, tid)) as s:
                    st = s.read()
                with open("/proc/%d/task/%s/wchan" % (pid, tid)) as w:
                    wc = w.read()
                comm = st[st.index("(") + 1:st.rindex(")")]
                f.write("tid %s comm %s state %s wchan %s\n" % (tid, comm, st[st.rindex(")") + 2], wc))
        except OSError as e:
            f.write("(gone: %s)\n" % e)
        f.write("== descendants\n")
        for d in descendants(pid):
            f.write(proc_line(d) + "\n")
        f.write("== gdb\n")
        f.flush()
        try:
            genv = dict(os.environ)
            genv.setdefault("DEBUGINFOD_CACHE_PATH", os.path.join(ROOT, ".scratch", "debuginfod"))
            r = subprocess.run(["gdb", "-p", str(pid), "-batch", "-nx",
                                "-ex", "set pagination off", "-ex", "set debuginfod enabled on",
                                "-ex", "info threads",
                                "-ex", "thread apply all bt 40"],
                               capture_output=True, text=True, timeout=600, env=genv)
            f.write(r.stdout)
            f.write(r.stderr[-4000:])
        except Exception as e:  # noqa: BLE001 — diagnostics only
            f.write("gdb failed: %s\n" % e)
    return path


def run_once(binary, case, bdir, outdir, idx, timeout):
    timing = os.path.join(bdir, "soak-timing.txt")
    perf_out = os.path.join(bdir, "soak-perf.txt")
    watch = os.path.join(outdir, "watch-%s-%d.txt" % (case, idx))
    for p in (timing, perf_out, watch):
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ)
    env.update({"BENCH_CASE": case, "BENCH_OUT": timing, "BENCH_DIR": bdir,
                "BENCH_GEN": os.path.join(ROOT, "scripts", "solidterm-bench-gen.py"),
                "SHELL": os.path.join(bdir, "runner.sh"), "SOLIDTERM_PERF": "1",
                "SOLIDTERM_PERF_OUT": perf_out, "SOLIDTERM_STALLWATCH": watch,
                "QT_FORCE_STDERR_LOGGING": "1"})
    log = open(os.path.join(outdir, "last.log"), "w")
    p = subprocess.Popen([binary, "--standalone"], env=env, stdout=subprocess.DEVNULL, stderr=log)
    t0 = time.time()
    rec = {"case": case, "run": idx}
    while p.poll() is None:
        stalled = os.path.exists(watch) and os.path.getsize(watch) > 0
        if stalled or time.time() - t0 > timeout:
            time.sleep(0.5)  # let the watchdog finish writing
            tag = "%s-%d" % (case, idx)
            rec["stall"] = capture(p.pid, outdir, tag)
            rec["stall_kind"] = "watchdog" if stalled else "timeout"
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
            break
        time.sleep(0.1)
    rec["wall"] = time.time() - t0
    rec["rc"] = p.returncode
    if os.path.exists(timing):
        tl = dict(line.split(None, 1) for line in open(timing) if line.strip())
        if "t0" in tl and "t1" in tl:
            rec["seconds"] = (int(tl["t1"]) - int(tl["t0"])) / 1e9
    if os.path.exists(watch) and os.path.getsize(watch) > 0:
        rec["watch"] = watch
    elif os.path.exists(watch):
        os.remove(watch)
    return rec


def main():
    binary, outdir = os.path.realpath(sys.argv[1]), sys.argv[2]
    plan = []
    for arg in sys.argv[3:]:
        c, n = arg.split(":")
        plan.append((c, int(n)))
    os.makedirs(outdir, exist_ok=True)
    bdir = os.environ.get("BENCH_DIR", os.path.join(ROOT, ".scratch", "bench"))
    timeout = float(os.environ.get("TIMEOUT", "120"))
    results = []
    # Interleave the cases (mixed, seq, region, mixed, ...) so a slow drift hits all of them.
    queue = []
    left = dict(plan)
    while any(left.values()):
        for c, _ in plan:
            if left[c]:
                queue.append(c)
                left[c] -= 1
    counts = {}
    for case in queue:
        wait_quiet(float(os.environ.get("MAX_LOAD", "2.5")))
        i = counts.get(case, 0)
        counts[case] = i + 1
        rec = run_once(binary, case, bdir, outdir, i, timeout)
        results.append(rec)
        print("%s %d: %s wall %.1f%s" % (case, i, ("%.3fs" % rec["seconds"]) if "seconds" in rec else "NO TIMING",
                                          rec["wall"], ("  STALL " + rec["stall"]) if "stall" in rec else ""),
              file=sys.stderr, flush=True)
        with open(os.path.join(outdir, "results.json"), "w") as f:
            json.dump(results, f, indent=1)
        time.sleep(float(os.environ.get("GAP", "1")))
    stalls = [r for r in results if "stall" in r or "seconds" not in r]
    print("runs %d, stalls/no-timing %d" % (len(results), len(stalls)), file=sys.stderr)


if __name__ == "__main__":
    main()
