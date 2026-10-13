#include "ptysession.h"
#include "stallwatch.h"
#include "perftrace.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <dirent.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

// ─── stall watchdog (stallwatch.h; env-gated diagnostics) ───────────────────────────────────────
struct StallWatchAccess {
    static void report(FILE *f, PtySession *s, int64_t now, int64_t heartbeat)
    {
        int pending = -1;
        if (s->m_master >= 0)
            ::ioctl(s->m_master, FIONREAD, &pending);
        const int64_t last = s->m_swLastReadNs.load(std::memory_order_relaxed);
        std::fprintf(f, "session %p fd %d pid %lld pending %d paused %d notifier %s bytes %lld reads %lld "
                        "last_read_age_ms %lld gui_heartbeat_age_ms %lld\n",
                     static_cast<void *>(s), s->m_master, static_cast<long long>(s->m_pid), pending,
                     int(s->m_paused),
                     s->m_notifier ? (s->m_notifier->isEnabled() ? "enabled" : "DISABLED") : "none",
                     static_cast<long long>(s->m_swBytes.load()), static_cast<long long>(s->m_swReads.load()),
                     static_cast<long long>(last ? (now - last) / 1000000 : -1),
                     static_cast<long long>(heartbeat ? (now - heartbeat) / 1000000 : -1));
    }
    static int pending(PtySession *s)
    {
        int n = 0;
        if (s->m_master >= 0)
            ::ioctl(s->m_master, FIONREAD, &n);
        return n;
    }
    static int64_t bytes(PtySession *s) { return s->m_swBytes.load(std::memory_order_relaxed); }
    static int64_t reads(PtySession *s) { return s->m_swReads.load(std::memory_order_relaxed); }
    static bool stalled(PtySession *s, int64_t now, int64_t stallNs)
    {
        const int64_t last = s->m_swLastReadNs.load(std::memory_order_relaxed);
        if (!last || s->m_master < 0)
            return false; // nothing read yet: the shell has not started writing
        int pending = 0;
        ::ioctl(s->m_master, FIONREAD, &pending);
        const int64_t age = now - last;
        return (pending > 0 && age > stallNs) || age > 2 * stallNs;
    }
};

namespace StallWatch {
namespace {
std::mutex g_mutex;
std::vector<PtySession *> g_sessions;
std::atomic<int64_t> g_heartbeat { 0 };
const char *g_path = nullptr;
constexpr int64_t kStallNs = 5'000'000'000;

void dumpThreads(FILE *f)
{
    DIR *d = ::opendir("/proc/self/task");
    if (!d)
        return;
    while (dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        char path[320], comm[64] = {}, wchan[128] = {}, stat[512] = {};
        auto slurp = [&](const char *leaf, char *buf, size_t n) {
            std::snprintf(path, sizeof(path), "/proc/self/task/%s/%s", e->d_name, leaf);
            if (FILE *p = std::fopen(path, "r")) {
                const size_t got = std::fread(buf, 1, n - 1, p);
                buf[got] = 0;
                std::fclose(p);
                for (size_t i = 0; i < got; ++i)
                    if (buf[i] == '\n')
                        buf[i] = ' ';
            }
        };
        slurp("comm", comm, sizeof(comm));
        slurp("wchan", wchan, sizeof(wchan));
        slurp("stat", stat, sizeof(stat));
        const char *st = std::strrchr(stat, ')');
        std::fprintf(f, "thread %s comm %s state %c wchan %s\n", e->d_name, comm, st && st[1] ? st[2] : '?', wchan);
    }
    ::closedir(d);
}

// Per-session drain window: a session whose master fd held unread bytes at EVERY tick of the last
// kStallNs yet drained less than kSlowBytes in that time is starved (it still reads, just a trickle).
struct Window {
    PtySession *s;
    int64_t startNs, startBytes, startReads;
    bool alwaysPending;
};
constexpr int64_t kSlowBytes = 2 << 20;

void loop()
{
    bool reported = false;
    std::vector<Window> windows;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::lock_guard<std::mutex> lock(g_mutex);
        const int64_t now = nowNs();
        // A GUI thread that stopped turning its event loop (heartbeat older than the threshold) is a
        // stall too, even before any pty byte was read.
        const int64_t hb = g_heartbeat.load(std::memory_order_relaxed);
        bool any = hb && now - hb > kStallNs;
        const char *why = any ? "gui-heartbeat" : nullptr;
        for (PtySession *s : g_sessions) {
            if (StallWatchAccess::stalled(s, now, kStallNs)) {
                any = true;
                why = why ? why : "no-read";
            }
            Window *w = nullptr;
            for (Window &x : windows)
                if (x.s == s)
                    w = &x;
            const int64_t bytes = StallWatchAccess::bytes(s), reads = StallWatchAccess::reads(s);
            if (!w) {
                windows.push_back({ s, now, bytes, reads, true });
                continue;
            }
            w->alwaysPending = w->alwaysPending && StallWatchAccess::pending(s) > 0;
            if (now - w->startNs >= kStallNs) {
                if (w->alwaysPending && bytes - w->startBytes < kSlowBytes) {
                    any = true;
                    why = why ? why : "slow-drain";
                    if (FILE *f = std::fopen(g_path, "a")) {
                        std::fprintf(f, "window %.1f s: %lld bytes in %lld reads while bytes were always pending\n",
                                     double(now - w->startNs) / 1e9, static_cast<long long>(bytes - w->startBytes),
                                     static_cast<long long>(reads - w->startReads));
                        std::fclose(f);
                    }
                }
                *w = { s, now, bytes, reads, true };
            }
        }
        std::erase_if(windows, [](const Window &w) { return std::find(g_sessions.begin(), g_sessions.end(), w.s) == g_sessions.end(); });
        if (!any) {
            reported = false;
            continue;
        }
        if (reported)
            continue; // one report per stall episode
        reported = true;
        if (FILE *f = std::fopen(g_path, "a")) {
            std::fprintf(f, "STALL %s pid %d t %lld frames %d\n", why, int(::getpid()), static_cast<long long>(now),
                         PerfTrace::on ? PerfTrace::frameCount.load() : -1);
            for (PtySession *s : g_sessions)
                StallWatchAccess::report(f, s, now, g_heartbeat.load());
            dumpThreads(f);
            std::fprintf(f, "END\n");
            std::fclose(f);
        }
    }
}
} // namespace

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void init()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    g_path = std::getenv("SOLIDTERM_STALLWATCH");
    on = g_path && *g_path;
    if (!on)
        return;
    ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); // let the soak attach gdb on a stall
    std::thread(loop).detach();
}

void add(PtySession *s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessions.push_back(s);
}

void remove(PtySession *s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::erase(g_sessions, s);
}

void heartbeat()
{
    g_heartbeat.store(nowNs(), std::memory_order_relaxed);
}

} // namespace StallWatch

PtySession::PtySession(QObject *parent)
    : QObject(parent)
{
}

PtySession::~PtySession()
{
    if (Q_UNLIKELY(StallWatch::on))
        StallWatch::remove(this);
    if (m_pid > 0)
        ::kill(pid_t(m_pid), SIGHUP);
    if (m_master >= 0)
        ::close(m_master);
}

bool PtySession::start(int rows, int cols, const QString &program, const QString &cwd,
                       const QStringList &env, bool readHere)
{
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);

    // Everything the child needs is built BEFORE fork: between fork and exec only async-signal-safe
    // calls are allowed (this process has threads — a malloc there can deadlock on a held lock).
    QByteArray envShell;
    std::vector<QByteArray> envStore;
    if (!env.isEmpty()) {
        envStore.reserve(size_t(env.size()) + 2);
        for (const QString &e : env) {
            if (e.startsWith(QLatin1String("TERM=")) || e.startsWith(QLatin1String("COLORTERM=")))
                continue;
            if (e.startsWith(QLatin1String("SHELL=")))
                envShell = e.mid(6).toLocal8Bit();
            envStore.push_back(e.toLocal8Bit());
        }
        envStore.emplace_back("TERM=xterm-256color");
        envStore.emplace_back("COLORTERM=truecolor");
    }
    std::vector<char *> envp;
    if (!envStore.empty()) {
        envp.reserve(envStore.size() + 1);
        for (QByteArray &e : envStore)
            envp.push_back(e.data());
        envp.push_back(nullptr);
    }
    const QByteArray shell = !program.isEmpty() ? program.toLocal8Bit()
        : !envShell.isEmpty()                    ? envShell
        : qgetenv("SHELL").isEmpty()             ? QByteArray("/bin/bash")
                                                 : qgetenv("SHELL");
    // argv[0] with a leading dash = login shell (profile/rc load, like every terminal).
    QByteArray argv0 = QByteArray("-") + shell.mid(shell.lastIndexOf('/') + 1);
    char *argv[] = { argv0.data(), nullptr };
    const QByteArray cwdBytes = cwd.toLocal8Bit();

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0)
        return false;
    if (pid == 0) {
        // Child: the slave pty is already the controlling terminal (forkpty did login_tty).
        if (!cwdBytes.isEmpty())
            (void)::chdir(cwdBytes.constData()); // gone/unreadable → stay in the inherited cwd
        if (!envp.empty()) {
            ::execvpe(shell.constData(), argv, envp.data());
        } else {
            ::setenv("TERM", "xterm-256color", 1);
            ::setenv("COLORTERM", "truecolor", 1);
            ::execvp(shell.constData(), argv);
        }
        ::_exit(127);
    }

    m_master = master;
    m_pid = pid;
    StallWatch::init();
    if (Q_UNLIKELY(StallWatch::on)) {
        StallWatch::add(this);
        auto *hb = new QTimer(this); // GUI-thread heartbeat: tells a blocked GUI thread from a quiet one
        connect(hb, &QTimer::timeout, this, [] { StallWatch::heartbeat(); });
        hb->start(100);
    }
    if (!readHere)
        return true;
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    m_notifier->setEnabled(!m_paused); // honour a pause() requested before start() somehow raced in
    connect(m_notifier, &QSocketNotifier::activated, this, [this] { onReadable(); });
    return true;
}

void PtySession::noteRead(size_t n)
{
    m_swLastReadNs.store(StallWatch::nowNs(), std::memory_order_relaxed);
    m_swBytes.fetch_add(int64_t(n), std::memory_order_relaxed);
    m_swReads.fetch_add(1, std::memory_order_relaxed);
}

void PtySession::markExited()
{
    if (m_pid <= 0)
        return;
    int status = 0;
    ::waitpid(pid_t(m_pid), &status, WNOHANG);
    m_pid = -1;
    emit finished();
}

void PtySession::resize(int rows, int cols)
{
    if (m_master < 0)
        return;
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(m_master, TIOCSWINSZ, &ws);
}

void PtySession::writeBytes(const QByteArray &bytes)
{
    if (m_master < 0 || bytes.isEmpty())
        return;
    qint64 off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(m_master, bytes.constData() + off, size_t(bytes.size() - off));
        if (n <= 0)
            break;
        off += n;
    }
}

void PtySession::pause()
{
    if (m_paused)
        return;
    m_paused = true;
    if (m_notifier)
        m_notifier->setEnabled(false);
}

void PtySession::resume()
{
    if (!m_paused)
        return;
    m_paused = false;
    if (m_notifier)
        m_notifier->setEnabled(true); // level-triggered: re-fires immediately if the kernel buffer has data
}

// One activation drains the master until the kernel has nothing more queued (FIONREAD == 0) or
// kDrainBudgetNs is spent, not just one read. A pty hands out at most ~4 KiB per read(2) under a
// flood, so one read per event-loop turn made throughput hostage to how often the GUI loop turns.
// On Wayland that rate can collapse to one turn per vsync: when a frame callback arrives while the
// GUI thread is blocked in Qt Quick's polishAndSync, QtWayland's queued doHandleFrameCallback runs
// after the sync and delivers the update request our next read left pending, so polishAndSync blocks
// again until the following vsync — every turn, a self-sustaining phase lock. Reading 4 KiB per
// 16.7 ms is 250 KB/s: the "terminal stopped consuming" stall of the 2026-10-13 bench
// (docs/evidence/solidterm/parser-thread-2026-10-13.md). The budget keeps one turn short enough for
// frames and input to stay on time.
void PtySession::onReadable()
{
    char buf[65536];
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const ssize_t n = ::read(m_master, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            return; // not EOF: the notifier fires again when there is data
        if (n <= 0) {
            // EOF/EIO: the shell exited. Reap and report.
            m_notifier->setEnabled(false);
            int status = 0;
            ::waitpid(pid_t(m_pid), &status, WNOHANG);
            m_pid = -1;
            emit finished();
            return;
        }
        if (Q_UNLIKELY(StallWatch::on))
            noteRead(size_t(n));
        emit bytesRead(QByteArray(buf, int(n)));
        if (m_paused || m_master < 0) // a handler paused reads (selection drag) — honour it at once
            return;
        int pending = 0;
        if (::ioctl(m_master, FIONREAD, &pending) != 0 || pending <= 0)
            return;
        if (std::chrono::steady_clock::now() - t0 >= kDrainBudget)
            return; // level-triggered: the notifier fires again on the next loop turn
    }
}
