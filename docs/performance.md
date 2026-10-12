# Performance: methodology and results

This page is the only source for performance numbers about solid-qml. Every claim elsewhere in the
docs, the README or the release notes is scoped to what is measured here and links to this page.
If a number is not on this page, it has not been measured.

## What we claim (and what we don't)

**The claim.** In our 1,000-row scrolling-list benchmark (js-framework-benchmark operations, one Linux
laptop, headless Chromium 153 as the web reference), solid-qml's **automatically virtualized** list
completes all six operations faster than the same Solid component in Chromium — 4–15× on the
synchronous time, and also to the next presented frame — and the same holds at 10,000 rows (6.8–16×).
It wins because it materializes only the visible rows; Chromium lays out every row. That is the
feature working as designed, not a faster per-row engine.

**Where native does NOT win (limits, measured below):**

- **Non-scrolling (eager) lists.** With `overflow-y: visible` every row is a live item. At 1,000
  rows native is **2.6× slower** to create and append, **1.4× slower** to clear and about even on
  remove; only swap is faster. At 10,000 rows native is slower on **all six** operations
  (create 1,033 vs 386 ms, append 128 vs 53 ms). A screen reader that is active also makes a
  scrolling list fall back to eager rows.
- **Per-row memory.** A materialized native row costs **~21 KB PSS vs ~11 KB** in Chromium's
  renderer (~12 KB counting every Chromium process). In a scrolling list native memory is flat in N
  (~3 KB per *logical* row: model data, JS handle, key, measured size).
- **"Update every 10th" without `key`.** Since C23, `<For>` matches by reference like the web, so
  the 100 new objects are 100 new rows. In an eager list that is **2.6× slower than the web at
  1,000 rows and 7.6× slower at 10,000** (15 ms / 462 ms); with `key="id"` native reuses the rows and
  wins (2.1 ms / 20 ms vs 5.9 / 61).
- **Sync vs presented frames.** `sync` excludes frame production. Native small operations reach a
  presented frame in ~6–8 ms (render-loop pacing over 0.4–2 ms of work) against ~16 ms for
  Chromium's 60 Hz frame clock — that gap is pacing, not work. For eager lists the native gap
  *widens* at the frame: clearing 1,000 rows is 7 ms sync but 24 ms to the frame (Chromium 7.6 ms).
- **No GPU presentation** was measured on either side (native null RHI; headless Chromium).

**What we do NOT claim:** anything about Electron (no Electron app was measured; headless Chromium
startup is not a packaged app's startup); "faster than the web" in general; anything about real-GPU
frame times, other machines, other distros, or list shapes other than this benchmark's row.

## Methodology

### The benchmark

One Solid component, [`examples/bench-rows.tsx`](../examples/bench-rows.tsx), runs unchanged on both
targets: natively through `solid-qml-loader` (TSX → QML + the C++ CSS engine) and in Chromium as a
production `vite build` of the same source with Solid's DOM renderer. Each row is a flex `<div>`
with two text children (an id and a label); every label is unique (`"pretty table 17"`), so neither
target can win by caching 10 repeated strings. Both targets load the same stylesheet,
[`examples/bench-rows.css`](../examples/bench-rows.css): a flex column that is either a scrolling
container (`overflow-y: auto; height: 100vh`) or a non-scrolling one (`overflow-y: visible`).

The six operations follow [js-framework-benchmark](https://github.com/krausest/js-framework-benchmark):

| op | what it does |
|---|---|
| create N rows | replace an empty list with N new rows (N = 1,000 or 10,000) |
| update every 10th | a new array in which every 10th row is a **new object** with `" !!!"` appended to the label |
| swap rows | swap rows 1 and 998 (clamped for small N) |
| remove one row | remove row 500 |
| append 1,000 rows | concatenate 1,000 new rows to the N existing ones |
| clear | replace the list with `[]` |

They run in that order, as one **round**; a process runs 11 rounds (round 0 = warm-up). The app
times itself with `performance.now()` around the state write and logs one line per operation.

**Row identity (`key`).** Since 0.3.0-preview.1 (release-plan C23), `<For>` matches rows by object
reference like Solid on the web: in "update every 10th" the 100 new objects are 100 new rows on
both targets. `<For each={…} key="id">` opts into reuse by id natively (the row is updated in
place). The web has no equivalent, so native is reported **without** `key` against the web (same
semantics), and **with** `key` as a separate native-only column.

### Containers: scrolling vs non-scrolling

Inside a scrolling container, the native `<For>` is **virtualized automatically** when the
transpiler can prove the row body is stateless (as it can for this benchmark): only the rows in the
viewport plus a margin are materialized, and offscreen rows are pooled. Chromium is not virtualized
(Solid renders every row into the DOM; Chromium culls painting, not layout or DOM work). So in a
scrolling list the two targets do **different amounts of work by design** — that is the point of
the feature, and the results say so explicitly. In a non-scrolling container native is eager (every
row is a live item) and the comparison is like for like. An active screen reader also makes native
fall back to eager rows (not measured here).

### Three timings per operation

| metric | native | web |
|---|---|---|
| **sync** | state write → returns; the CSS layout is synchronous, so the layout is settled | state write → returns, then a forced layout (`document.body.offsetHeight`) |
| **+tick** | until the next zero-delay timer turn (deferred deletions, queued work) | same |
| **+frame** (presented) | until the window's `frameSwapped` after the mutation; the runner forces a frame with `update()` so ops with no visible change still get one | double `requestAnimationFrame` (the 2nd callback runs at the start of the frame after the one that painted the mutation) |

`sync` is the number the project's gate has historically used. It is **not** what a user sees:
`+frame` includes render-loop pacing on both sides (Chromium aligns to its 60 Hz begin-frame
clock even headless — small ops land near 16.7 ms; the native null-RHI loop has its own pacing),
so presented-frame numbers are reported next to sync, never replaced by them, and are only
loosely comparable across targets.

### Renderer configuration

- **Native:** `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=rhi QSG_RHI_BACKEND=null`. The null RHI
  runs the full Qt Quick pipeline (polish, scene-graph sync, render-loop pacing, `frameSwapped`) but
  rasterizes nothing; Qt picks its **basic** (single-threaded) render loop here (logged via
  `QSG_INFO=1` and kept in the raw JSON). We do **not** use the offscreen default software renderer for timings: it
  pays costs a GPU never pays (e.g. `QRegion` unions when nodes are removed made a non-scrolling
  `clear` look 3.5× slower). Consequence: **GPU presentation is not measured** — no native number
  here includes real rasterization or a real swap.
- **Web:** Chromium `--headless=new` (flags recorded in the JSON), default (software) compositing,
  window 800×900, same as the native window.

### Memory per row

[`examples/bench-mem.tsx`](../examples/bench-mem.tsx) logs a marker on an empty list, mounts 2,000
rows with unique labels, and logs another marker; the runner reads PSS from
`/proc/<pid>/smaps_rollup` at the first marker and 3 s after mounting (steady state; earlier
measurements showed a threaded render loop still catching up at 0.8 s). KB/row = PSS growth / 2,000.

- Native: the single loader process (null RHI, as above).
- Web: the **renderer** process with the largest growth (the method used throughout the project's
  history); the JSON also records every Chromium process, and "all processes" adds the browser,
  GPU and utility processes' growth.
- In a scrolling list native materializes only a viewport of rows, so its KB/row there is the cost
  of the **model** (row data, JS handle, key, measured size), not of materialized rows. The
  non-scrolling run is the per-materialized-row cost.

### Startup

The same memory runs record the wall time from spawning the process to the app's `onMount` marker
reaching the runner. Native: loader start, QML engine creation, compiling the generated QML (no
qmlcachegen), first layout. Web: launching headless Chromium (browser + renderer processes) and
loading the page from a local HTTP server. **This is not an Electron comparison**: no Electron app
was measured, and headless Chromium startup is not a packaged Electron app's startup.

### Repetitions and statistics

- Rows benchmark: 2 processes × 11 rounds per configuration; round 0 of each process is a warm-up
  (kept in the raw data, reported separately as "cold") → **20 warm samples per operation**.
- Memory/startup: 10 processes per configuration.
- Reported: **median, p95 (nearest rank) and min**. Raw samples are committed.

### Load gating

The machine is a developer laptop shared with other work (other agents, the owner's apps). Before
every batch (one configuration) the runner waits — polling every 2 minutes — until the 1-minute
load average is < 3 **and** no `cc1plus`/`ninja`/`podman`/`ld`/`ldc2`/`rustc` process is running.
It records, per batch, the gate outcome, the load at start and end, the build processes seen and the
top CPU consumers (instantaneous, from `top`), plus the load at the start of every process run.
Measurements never overlap a build on purpose: before every process run the runner waits until no
build process is running, and it records whether one appeared during the run (reported below). If
the machine does not get quiet within the wait window the batch runs anyway and is marked
`timeout-noisy`; such numbers are reported as noisy, never as clean.

### Reproducing

```sh
meson setup build-bench --buildtype=release -Dqml_debug=false -Dsolidterm=disabled \
    -Dexamples=disabled -Daot=disabled -Dqml-css-engine:tests=disabled
ninja -C build-bench
node scripts/bench/run.mjs --build build-bench            # writes bench/results/<date>-<host>.*
node scripts/bench/report.mjs bench/results/<file>.json --doc docs/performance.md
```

Requirements: Node + `npm ci` (vite, tsx), a `chromium` binary on `PATH` (or `--chromium <path>`),
Linux `/proc`. The runner refuses a stale build (`ninja -n` must report no work). Options:
`--sizes`, `--rounds`, `--reps`, `--mem-reps`, `--targets`, `--max-wait-min`, `--no-gate`.

## Results

<!-- results:begin -->
Generated by `scripts/bench/report.mjs` from `bench/results/2026-10-12-numenor.json`, `bench/results/2026-10-12-numenor-rerun.json` (raw samples in the JSON and the CSVs next to it).

**Environment of this run**

| | |
|---|---|
| date | 2026-10-12T02:42:26.465Z |
| machine | Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz, 12 logical CPUs, 31 GiB RAM, governor `powersave` |
| OS | Manjaro Linux, Linux 6.1.187-2-MANJARO |
| Qt | 6.11.2 |
| Chromium | Chromium 153.0.8010.36 Arch Linux (headless=new) |
| Node | v26.8.1 |
| solid-qml | 6aebd9d0a2 (+ uncommitted bench changes), engine b5f17e9d7b |
| native build | `release` (-O3, qml_debug=false), libsolidqml sha256 bd01eab75b6e2645… |
| native renderer | QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=rhi QSG_RHI_BACKEND=null (nothing is rasterized) |
| samples | 2 processes × 10 warm rounds per op (+1 warm-up round each); memory/startup: 10 processes |

### 1,000 rows — scrolling list (`overflow-y: auto`) — native virtualizes

Synchronous time (ms; state write → returns, layout settled). Warm rounds.

| operation | native median | p95 | min | cold¹ | native `key="id"` median | p95 | web median | p95 | min | cold¹ | median ratio web/native |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| create N rows | 12.1 | 13.0 | 10.9 | 25.6 | 11.5 | 12.3 | 49.6 | 55.8 | 41.9 | 58.6 | **4.1× (native faster)** |
| update every 10th | 1.01 | 1.06 | 0.92 | 1.05 | 0.66 | 0.76 | 5.95 | 7.10 | 5.30 | 7.55 | **5.9× (native faster)** |
| swap rows 1↔998 | 0.45 | 0.86 | 0.36 | 0.46 | 0.42 | 0.48 | 1.80 | 1.90 | 1.60 | 1.90 | **4.0× (native faster)** |
| remove one row | 0.35 | 0.47 | 0.30 | 0.39 | 0.34 | 0.42 | 1.45 | 1.60 | 1.10 | 1.70 | **4.1× (native faster)** |
| append 1,000 rows | 2.52 | 4.52 | 2.18 | 2.64 | 2.65 | 2.81 | 38.9 | 54.1 | 33.5 | 42.0 | **15.4× (native faster)** |
| clear | 1.13 | 1.24 | 1.03 | 1.02 | 1.14 | 1.47 | 5.20 | 14.9 | 4.90 | 5.50 | **4.6× (native faster)** |

Until the next presented frame and until the next timer turn (ms; medians, p95 in parentheses).

| operation | native frame | web frame (double rAF) | native +tick | web +tick |
|---|---:|---:|---:|---:|
| create N rows | 14.9 (16.1) | 56.2 (64.0) | 14.9 (16.1) | 56.2 (62.5) |
| update every 10th | 6.76 (7.09) | 10.6 (16.4) | 1.05 (1.10) | 8.30 (10.4) |
| swap rows 1↔998 | 5.99 (6.27) | 16.1 (16.7) | 0.48 (1.72) | 3.70 (4.40) |
| remove one row | 5.56 (5.90) | 16.4 (16.5) | 0.39 (0.51) | 4.70 (5.10) |
| append 1,000 rows | 7.78 (9.70) | 45.3 (63.7) | 2.55 (4.55) | 45.2 (63.6) |
| clear | 6.11 (6.45) | 8.60 (16.8) | 1.57 (1.70) | 5.45 (15.1) |

### 1,000 rows — non-scrolling list (`overflow-y: visible`) — native is eager

Synchronous time (ms; state write → returns, layout settled). Warm rounds.

| operation | native median | p95 | min | cold¹ | native `key="id"` median | p95 | web median | p95 | min | cold¹ | median ratio web/native |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| create N rows | 97.8 | 109 | 94.2 | 131 | 96.5 | 103 | 37.0 | 40.7 | 32.1 | 43.2 | 2.6× slower |
| update every 10th | 15.1 | 17.1 | 14.4 | 14.7 | 2.07 | 3.99 | 5.85 | 6.40 | 5.20 | 7.50 | 2.6× slower |
| swap rows 1↔998 | 1.40 | 1.58 | 1.25 | 1.50 | 1.29 | 1.37 | 1.75 | 2.00 | 1.60 | 2.10 | **1.3× (native faster)** |
| remove one row | 1.56 | 1.76 | 1.45 | 1.48 | 1.46 | 1.60 | 1.50 | 1.70 | 1.20 | 1.90 | 1.0× slower |
| append 1,000 rows | 99.3 | 116 | 92.8 | 106 | 96.4 | 114 | 37.6 | 47.6 | 33.3 | 40.0 | 2.6× slower |
| clear | 7.03 | 7.60 | 6.66 | 6.55 | 6.88 | 7.34 | 5.10 | 5.70 | 4.80 | 5.40 | 1.4× slower |

Until the next presented frame and until the next timer turn (ms; medians, p95 in parentheses).

| operation | native frame | web frame (double rAF) | native +tick | web +tick |
|---|---:|---:|---:|---:|
| create N rows | 126 (142) | 43.0 (48.9) | 126 (142) | 43.0 (48.9) |
| update every 10th | 21.1 (25.3) | 10.1 (18.5) | 21.2 (25.4) | 8.25 (9.30) |
| swap rows 1↔998 | 7.78 (8.37) | 16.2 (16.8) | 1.43 (1.62) | 3.60 (4.80) |
| remove one row | 7.58 (8.01) | 16.3 (16.5) | 1.63 (1.82) | 4.50 (5.60) |
| append 1,000 rows | 128 (146) | 43.7 (58.4) | 128 (146) | 43.6 (58.3) |
| clear | 23.6 (25.3) | 7.55 (15.2) | 23.6 (25.3) | 5.30 (6.00) |

### 10,000 rows — scrolling list (`overflow-y: auto`) — native virtualizes

Synchronous time (ms; state write → returns, layout settled). Warm rounds.

| operation | native median | p95 | min | cold¹ | native `key="id"` median | p95 | web median | p95 | min | cold¹ | median ratio web/native |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| create N rows | 32.4 | 39.1 | 31.2 | 46.5 | 36.6 | 38.4 | 524 | 657 | 433 | 578 | **16.2× (native faster)** |
| update every 10th | 5.17 | 5.81 | 4.88 | 4.71 | 4.21 | 4.68 | 59.1 | 68.4 | 52.0 | 58.3 | **11.4× (native faster)** |
| swap rows 1↔998 | 1.97 | 3.16 | 1.79 | 2.13 | 1.87 | 2.16 | 13.4 | 18.4 | 12.2 | 13.8 | **6.8× (native faster)** |
| remove one row | 2.04 | 2.19 | 1.94 | 2.00 | 1.95 | 2.04 | 14.2 | 19.0 | 12.4 | 13.0 | **6.9× (native faster)** |
| append 1,000 rows | 3.88 | 4.30 | 3.70 | 3.55 | 4.33 | 4.81 | 51.3 | 59.0 | 44.9 | 48.8 | **13.2× (native faster)** |
| clear | 4.64 | 5.77 | 3.96 | 5.37 | 4.59 | 5.41 | 29.0 | 32.7 | 26.4 | 28.5 | **6.2× (native faster)** |

Until the next presented frame and until the next timer turn (ms; medians, p95 in parentheses).

| operation | native frame | web frame (double rAF) | native +tick | web +tick |
|---|---:|---:|---:|---:|
| create N rows | 38.5 (45.0) | 585 (720) | 38.5 (45.0) | 585 (720) |
| update every 10th | 10.7 (11.3) | 77.0 (85.3) | 5.21 (5.85) | 74.8 (85.3) |
| swap rows 1↔998 | 7.70 (8.54) | 25.1 (31.7) | 2.01 (3.20) | 25.1 (31.6) |
| remove one row | 7.34 (7.69) | 56.5 (68.5) | 2.07 (2.22) | 56.4 (68.5) |
| append 1,000 rows | 9.31 (9.54) | 68.1 (77.3) | 3.92 (4.34) | 68.0 (77.3) |
| clear | 7.65 (8.20) | 29.4 (33.0) | 5.06 (6.43) | 29.4 (33.0) |

### 10,000 rows — non-scrolling list (`overflow-y: visible`) — native is eager

Synchronous time (ms; state write → returns, layout settled). Warm rounds.

| operation | native median | p95 | min | cold¹ | native `key="id"` median | p95 | web median | p95 | min | cold¹ | median ratio web/native |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| create N rows | 1033 | 1233 | 1000 | 1082 | 1069 | 1230 | 386 | 1419 | 321 | 397 | 2.7× slower |
| update every 10th | 462 | 538 | 424 | 406 | 19.8 | 22.6 | 61.0 | 282 | 50.1 | 71.7 | 7.6× slower |
| swap rows 1↔998 | 16.6 | 23.4 | 14.3 | 15.5 | 17.0 | 23.6 | 14.4 | 46.8 | 12.2 | 13.4 | 1.2× slower |
| remove one row | 20.6 | 23.9 | 19.4 | 19.1 | 20.6 | 23.0 | 15.6 | 41.7 | 12.1 | 13.4 | 1.3× slower |
| append 1,000 rows | 128 | 149 | 123 | 132 | 131 | 150 | 52.5 | 173 | 44.4 | 46.7 | 2.4× slower |
| clear | 46.3 | 52.6 | 44.4 | 44.9 | 45.8 | 60.5 | 27.5 | 105 | 25.6 | 26.9 | 1.7× slower |

Until the next presented frame and until the next timer turn (ms; medians, p95 in parentheses).

| operation | native frame | web frame (double rAF) | native +tick | web +tick |
|---|---:|---:|---:|---:|
| create N rows | 1332 (1555) | 444 (1696) | 1332 (1555) | 444 (1695) |
| update every 10th | 554 (654) | 78.7 (357) | 554 (654) | 78.7 (357) |
| swap rows 1↔998 | 42.8 (58.2) | 25.4 (97.2) | 42.8 (58.2) | 25.4 (97.2) |
| remove one row | 50.4 (64.8) | 57.7 (201) | 50.4 (64.8) | 57.7 (201) |
| append 1,000 rows | 177 (219) | 68.4 (238) | 177 (219) | 68.3 (238) |
| clear | 189 (201) | 27.6 (131) | 189 (201) | 27.6 (128) |

¹ cold = round 0 of each process (first time the operation runs in that process), median across processes.

### Memory per row and startup

PSS growth from an empty list to 2,000 mounted rows (unique labels), 3 s after mounting. Native: the whole loader process. Web: the renderer process with the largest growth (the method used throughout the project); "all processes" adds the browser, GPU and utility processes' growth.

| target | list | KB/row median (min–max) | all processes KB/row | process PSS after, MB | startup → mounted, ms median (p95) | runs |
|---|---|---:|---:|---:|---:|---:|
| native | scrolling | 2.99 (2.69–3.28) | 2.99 | 23.1 | 49.0 (66.5) | 10 |
| web | scrolling | 14.7 (14.6–15.2) | 15.9 | 46.8 | 593 (713) | 10 |
| native | non-scrolling | 21.4 (21.4–21.4) | 21.4 | 58.6 | 48.1 (66.8) | 10 |
| web | non-scrolling | 11.3 (11.3–11.9) | 12.1 | 40.2 | 555 (694) | 10 |

### Machine load per batch

Gate: 1-minute load < 3 and no cc1plus/ninja/podman/ld/ldc2/rustc process. `timeout-noisy` = the machine did not get quiet within the wait window and the batch ran anyway; its numbers carry that load.

| batch | gate | waited, min | load1 at start → end | build processes at start | top CPU consumers at start (%) |
|---|---|---:|---|---|---|
| native auto N=1000 | quiet | 4 | 2.11 → 1.79 | none | chromium (renderer) 34 |
| native auto N=1000 key | quiet | 0 | 1.79 → 1.54 | none | gh 12 |
| web auto N=1000 | quiet | 0 | 1.54 → 1.47 | none | none ≥ 5% |
| native visible N=1000 | quiet | 2 | 0.78 → 0.77 | none | none ≥ 5% |
| native visible N=1000 key | quiet | 0 | 0.77 → 0.76 | none | chromium 19, chromium (renderer) 8 |
| web visible N=1000 | quiet | 0 | 0.76 → 0.96 | none | ai-memory 30 |
| native auto N=10000 | quiet | 0 | 0.96 → 0.82 | none | [kworker/3:2-events] 78 |
| native auto N=10000 key | quiet | 0 | 0.82 → 0.72 | none | chromium (renderer) 24 |
| web auto N=10000 | quiet | 0 | 0.72 → 1.02 | none | none ≥ 5% |
| native visible N=10000 | quiet | 0 | 0.94 → 1.27 | none | qml-whats-ldc2 16, chromium (renderer) 7 |
| native visible N=10000 key | quiet | 0 | 1.16 → 1.23 | none | none ≥ 5% |
| web visible N=10000 | quiet | 0 | 1.13 → 6.37 | none | test-layout-phase0 7 |
| mem native auto | quiet | 2 | 1.47 → 1.15 | none | [kcompactd0] 33 |
| mem web auto | quiet | 0 | 1.13 → 1.97 | none | none ≥ 5% |
| mem native visible | quiet | 0 | 1.97 → 0.97 | none | [kworker/3:1+pm] 84, QtWebEngineProcess (renderer) 75, qml-whats-ldc2 63 |
| mem web visible | quiet | 0 | 0.97 → 1.11 | none | none ≥ 5% |
| native auto N=1000 (re-measure, 2026-10-12T14:02Z) | quiet | 0 | 1.48 → 1.45 | none | python3 filter.py 100 |
| web auto N=1000 (re-measure, 2026-10-12T14:02Z) | quiet | 0 | 1.45 → 1.45 | none | python3 filter.py 99 |
| native auto N=10000 key (re-measure, 2026-10-12T14:02Z) | quiet | 0 | 1.45 → 1.45 | none | python3 filter.py 100, [kcompactd0] 63, [kworker/0:2-events] 54 |
| web auto N=10000 (re-measure, 2026-10-12T14:02Z) | quiet | 0 | 1.45 → 2.11 | none | python3 filter.py 100, chromium (renderer) 6 |

Process runs that overlapped a build process (checked before and after each run), EXCLUDED from the tables above and re-measured: **4** (native auto N=1000; web auto N=1000; native auto N=10000; web auto N=10000). 1-minute load at the start of each process run: median 1.20, max 3.28.

<!-- results:end -->

## Summary

| | scrolling list (native virtualized) | non-scrolling list (native eager) |
|---|---|---|
| 1,000 rows, sync | native faster on 6/6 (4.0–15×) | native slower on create/append/update¹ (2.6×) and clear (1.4×); remove even; swap 1.3× faster |
| 1,000 rows, presented frame | native faster on 6/6 | native slower on create/append (126–128 vs 43–44 ms), update¹ and clear; faster on swap/remove (pacing) |
| 10,000 rows, sync | native faster on 6/6 (6.8–16×) | native slower on 6/6 (1.2–7.6×) |
| PSS per row | ~3 KB per logical row (flat in N) vs ~15 KB | ~21 KB vs ~11 KB (renderer) / ~12 KB (all Chromium processes) |
| startup → app mounted | native ~48–49 ms vs ~555–593 ms headless Chromium launch + page load (not an Electron comparison) | same |

¹ "update every 10th" without `key` (reference identity, the default since C23); with `key="id"`
native is faster than the web (2.1 vs 5.9 ms at 1,000 rows).

**Data quality.** Every batch passed the load gate (1-minute load < 3, no build process); the
median load at the start of a process run was 1.2 (max 3.3). Four process runs saw a build start
while they ran; they are excluded from the tables and were re-measured in a second, gated session
(same build, same browser). Outliers worth knowing: the web eager 10,000-row batch has long p95
tails (create p95 1,419 ms vs median 386) and ended at load 6.4 — read its p95 column as noisy; its
medians agree with the cold round. The web numbers here (11–15 KB/row, create 1k ≈ 37–50 ms) are
close to, but not identical with, the 15.9 KB / 46 ms the project used as its reference in
2026-10-09: different Chromium build, flags and settle time — the project's gate should be re-based
on this page's numbers.

**History.** Earlier internal numbers (first round only, one process, offscreen software renderer)
are in the project's internal roadmap; this page supersedes them for any public statement.
