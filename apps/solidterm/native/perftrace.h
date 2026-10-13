#pragma once

// Env-gated performance probes for the solidterm benchmark (scripts/solidterm-bench.sh). OFF unless
// SOLIDTERM_PERF is set: every probe site is `if (Q_UNLIKELY(PerfTrace::on))` around a store into a
// preallocated array — one predictable branch on a static bool when off, no allocation, no lock.
//
// Two record kinds:
//  - per-frame: QQuickWindow::frameSwapped timestamps (render thread) + updatePaintNode duration, so
//    the bench can build a frame-time histogram and count frames that miss the refresh budget;
//  - per-keypress latency stages (SOLIDTERM_PERF=latency): keyPressEvent → pty write → first pty
//    read after the write → vterm damage → updatePaintNode → frameSwapped. One key is in flight at a
//    time (the test hook spaces them), so each stage keeps the FIRST timestamp after the previous one.
//
// Everything is dumped as plain text by dump() at quit (path from SOLIDTERM_PERF_OUT).

#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace PerfTrace {

inline bool on = false;

inline int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── frames ───────────────────────────────────────────────────────────────────────────────────
constexpr int kMaxFrames = 1 << 18;
struct Frame { int64_t swapNs; };
inline Frame *frames = nullptr;           // allocated by init() only when on
inline std::atomic<int> frameCount { 0 };
inline std::atomic<int64_t> paintNsTotal { 0 }; // summed updatePaintNode time
inline std::atomic<int> paintCount { 0 };
inline std::atomic<int64_t> paintNsMax { 0 };
inline std::atomic<int64_t> ptyReads { 0 }, ptyBytes { 0 }; // the reading thread (GUI or parser)

// ── latency (one key in flight) ──────────────────────────────────────────────────────────────
enum Stage { Key, PtyWrite, PtyRead, Damage, PaintBegin, PaintEnd, Swap, StageCount };
constexpr int kMaxKeys = 4096;
inline int64_t (*keys)[StageCount] = nullptr;
inline std::atomic<int> keyCount { 0 };   // index of the key in flight = keyCount - 1
inline std::atomic<int> keyStage { StageCount }; // next stage awaited for the in-flight key

inline void init(bool enable)
{
    on = enable;
    if (!on)
        return;
    frames = new Frame[kMaxFrames];
    keys = new int64_t[kMaxKeys][StageCount]();
}

// Stage `s` happened now: recorded only if it is the stage the in-flight key is waiting for.
inline void stage(Stage s)
{
    if (s == Key) {
        const int k = keyCount.load(std::memory_order_relaxed);
        if (k >= kMaxKeys)
            return;
        keys[k][Key] = nowNs();
        keyCount.store(k + 1, std::memory_order_release);
        keyStage.store(PtyWrite, std::memory_order_release);
        return;
    }
    int expect = s;
    if (keyStage.load(std::memory_order_acquire) != expect)
        return;
    const int k = keyCount.load(std::memory_order_acquire) - 1;
    if (k < 0)
        return;
    keys[k][s] = nowNs();
    keyStage.compare_exchange_strong(expect, s + 1 == StageCount ? int(StageCount) : s + 1);
}

inline void frameSwapped()
{
    const int i = frameCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxFrames)
        frames[i].swapNs = nowNs();
    stage(Swap);
}

inline void paint(int64_t ns)
{
    paintNsTotal.fetch_add(ns, std::memory_order_relaxed);
    paintCount.fetch_add(1, std::memory_order_relaxed);
    int64_t m = paintNsMax.load(std::memory_order_relaxed);
    while (ns > m && !paintNsMax.compare_exchange_weak(m, ns)) {}
}

inline void dump(const char *path)
{
    if (!on || !path || !*path)
        return;
    FILE *f = std::fopen(path, "w");
    if (!f)
        return;
    const int n = qMin(frameCount.load(), kMaxFrames);
    const int64_t offset = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count() - nowNs();
    std::fprintf(f, "realtime_offset %lld\n", static_cast<long long>(offset)); // steady → wall clock
    std::fprintf(f, "pty_reads %lld\npty_bytes %lld\n", static_cast<long long>(ptyReads.load()),
                 static_cast<long long>(ptyBytes.load()));
    std::fprintf(f, "paint_count %d\npaint_ns_total %lld\npaint_ns_max %lld\nframes %d\n", paintCount.load(),
                 static_cast<long long>(paintNsTotal.load()), static_cast<long long>(paintNsMax.load()), n);
    for (int i = 0; i < n; ++i)
        std::fprintf(f, "F %lld\n", static_cast<long long>(frames[i].swapNs));
    const int kn = qMin(keyCount.load(), kMaxKeys);
    for (int k = 0; k < kn; ++k) {
        std::fprintf(f, "K");
        for (int s = 0; s < StageCount; ++s)
            std::fprintf(f, " %lld", static_cast<long long>(keys[k][s]));
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

} // namespace PerfTrace
