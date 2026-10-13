#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Parser thread of one terminal session (docs/solidterm-parser-thread.md). Qt-free.
//
// The worker owns the READ side of the pty master: a poll(2) loop that reads up to 64 KiB and hands
// it to the session's sink in slices, each slice under the session's ScreenMutex (the sink runs
// libvterm, whose callbacks write the TermScreen). Anything that touches the VTerm or the TermScreen
// from another thread (GUI readers, the renderer's snapshot copy, keyboard input) takes the same
// mutex. News for the GUI is signalled through an eventfd the GUI watches, coalesced (one wake in
// flight) and throttled (at most one wake per kGuiInterval). Bytes for the pty (keys, paste, libvterm
// responses) are queued and written non-blocking: no thread ever blocks in write(2).

// One mutex per session, plus hand-off to waiting readers: the worker re-locks after every slice,
// and a plain std::mutex lets it barge back in ahead of a sleeping reader indefinitely.
class ScreenMutex {
public:
    // GUI / render thread.
    void lock()
    {
        m_waiting.fetch_add(1, std::memory_order_acq_rel);
        m_mutex.lock();
        m_waiting.fetch_sub(1, std::memory_order_release);
    }
    // Worker.
    void lockWriter() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }
    // Worker, after unlock(): let a waiting reader take the mutex before locking again.
    void yieldToReaders() const
    {
        while (m_waiting.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
    }

private:
    std::mutex m_mutex;
    std::atomic<int> m_waiting { 0 };
};

class ScreenLock {
public:
    explicit ScreenLock(ScreenMutex &m) : m_m(m) { m_m.lock(); }
    ~ScreenLock() { m_m.unlock(); }
    ScreenLock(const ScreenLock &) = delete;
    ScreenLock &operator=(const ScreenLock &) = delete;

private:
    ScreenMutex &m_m;
};

// What the worker calls, always with the ScreenMutex held. Plain function pointers (no
// std::function), the same shape as libvterm's callback tables.
struct TermWorkerSink {
    // Parse `len` bytes. Returns true when there is news the GUI has not taken yet.
    bool (*feed)(void *user, const char *data, size_t len);
    // The master reported EOF/EIO: the shell exited. Record it as news.
    void (*exited)(void *user);
    // A read happened (perf probes only; may be null).
    void (*readProbe)(void *user, size_t len);
};

class TermWorker {
public:
    static constexpr size_t kReadSize = 64 * 1024;
    // Measured (seq / mixed, 3 runs each): 4 / 16 / 64 KiB slices give the same throughput, while the
    // renderer's average wait for the lock grows 0.15 → 0.37 → 0.48 ms in seq.
    static constexpr size_t kSlice = 4 * 1024;
    static constexpr int64_t kGuiIntervalNs = 4'000'000;

    TermWorker() = default;
    ~TermWorker();
    TermWorker(const TermWorker &) = delete;
    TermWorker &operator=(const TermWorker &) = delete;

    // Sets O_NONBLOCK on masterFd and starts the thread, reading at once or (startPaused) only after
    // setPaused(false). The fd stays owned by the caller and must outlive stop().
    bool start(int masterFd, ScreenMutex *mutex, const TermWorkerSink *sink, void *user, bool startPaused = false);
    // Wakes the worker, joins it. Idempotent. After it returns the worker touches nothing.
    void stop();
    bool running() const { return m_thread.joinable(); }

    // The eventfd the GUI thread watches for news (valid after start()).
    int guiFd() const { return m_guiFd; }
    // GUI: call when guiFd fires, BEFORE taking the lock to apply the news (so news recorded after
    // the apply posts a fresh wake instead of being lost).
    void guiAcknowledge();

    // ── caller holds the ScreenMutex ──────────────────────────────────────────────────────────────
    // Selection drag: stop reading/parsing (the rest of the current read is kept), or resume.
    void setPaused(bool paused);
    bool paused() const { return m_paused.load(std::memory_order_relaxed); }
    // Bytes for the pty: appended, then written as far as the kernel takes them right now; the
    // worker writes the rest when the master is writable again.
    void queueOutput(const char *data, size_t len);

private:
    void run();
    void wake();
    void flushOutput();                    // lock held
    bool parseCarry();                     // returns whether news arrived
    void postGui();

    int m_master = -1;
    int m_wakeFd = -1;                     // GUI → worker (stop, resume, output pending)
    int m_guiFd = -1;                      // worker → GUI (news)
    ScreenMutex *m_mutex = nullptr;
    const TermWorkerSink *m_sink = nullptr;
    void *m_user = nullptr;
    std::thread m_thread;

    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_paused { false };  // written under the lock; also read lock-free by the poll setup
    std::atomic<bool> m_posted { false };  // a GUI wake is in flight
    std::atomic<bool> m_outPending { false };

    // Worker only.
    std::vector<char> m_buf;               // read buffer (kReadSize)
    size_t m_carryOff = 0, m_carryLen = 0; // unparsed part of m_buf
    // libvterm (0.3.3) does not carry an incomplete UTF-8 sequence from one vterm_input_write to the
    // next: a sequence split across two feeds becomes U+FFFD twice. So every slice ends on a UTF-8
    // boundary, and the incomplete tail of a read waits at m_buf[m_tailOff, +m_tailLen) to be moved
    // to the front of the buffer and completed by the next read.
    size_t m_tailOff = 0, m_tailLen = 0;
    int64_t m_lastPostNs = 0;
    bool m_newsPending = false;
    bool m_eof = false;

    // Under the lock.
    std::vector<char> m_out;
    size_t m_outHead = 0;
};
