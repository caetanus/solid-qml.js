// TermWorker — the per-pane parser thread (docs/solidterm-parser-thread.md) — against a REAL pty and
// a real child, no Qt. The sink records what it is fed under the ScreenMutex, the way TerminalView's
// libvterm sink does; this file plays the GUI thread (it watches guiFd and acknowledges wakes).
//
//  1. flood: 20 MB arrive intact; GUI wakes are coalesced/throttled (≤ 1 per 4 ms);
//  2. pause/resume: while paused nothing is parsed (the child blocks on the full pty), after resume
//     the sequence 1..N arrives complete and in order;
//  3. output never blocks: 4 MB queued for a child that is not reading returns at once, and all of
//     it arrives once the child reads;
//  4. stop() mid-flood joins promptly and nothing is fed afterwards;
//  5. the child's exit is reported through the sink;
//  6. UTF-8: libvterm (0.3.3) turns a sequence split across two feeds into U+FFFD, so no feed may
//     start or end inside one — whether the split comes from a 4 KiB slice or from a read boundary
//     (a tmux %output line, or a program's write, can end anywhere).
#include "../native/termworker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

int g_failures = 0;
#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);        \
            std::fprintf(stderr, __VA_ARGS__);                                           \
            std::fprintf(stderr, "\n");                                                  \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Ctx {
    ScreenMutex mutex;
    std::string data;    // everything fed (under the lock)
    long long fed = 0;
    int feeds = 0;
    bool exited = false;
    bool keepData = true;
};

bool feed(void *user, const char *d, size_t n)
{
    auto *c = static_cast<Ctx *>(user);
    c->fed += (long long)n;
    ++c->feeds;
    if (c->keepData)
        c->data.append(d, n);
    return true;
}
void exited(void *user) { static_cast<Ctx *>(user)->exited = true; }
const TermWorkerSink kSink = { feed, exited, nullptr };

struct Child {
    pid_t pid = -1;
    int master = -1;
    explicit Child(const char *cmd)
    {
        struct winsize ws = {};
        ws.ws_row = 24;
        ws.ws_col = 80;
        pid = forkpty(&master, nullptr, nullptr, &ws);
        if (pid == 0) {
            ::setenv("TERM", "dumb", 1);
            ::execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
            ::_exit(127);
        }
        // Raw on our side of the line discipline is irrelevant; turn off echo so the test's own
        // writes do not come back mixed into the output.
        struct termios t;
        if (tcgetattr(master, &t) == 0) {
            t.c_lflag &= ~(ECHO | ICANON);
            t.c_oflag &= ~OPOST;
            tcsetattr(master, TCSANOW, &t);
        }
    }
    ~Child()
    {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        if (master >= 0)
            ::close(master);
    }
};

// Plays the GUI thread for `ms`: waits on guiFd, acknowledges, returns how many wakes it took, and
// stops early once `done` holds (checked under the lock).
template <typename Done>
int pumpGui(TermWorker &w, Ctx &c, double ms, Done done)
{
    int wakes = 0;
    const auto t0 = Clock::now();
    while (msSince(t0) < ms) {
        pollfd p { w.guiFd(), POLLIN, 0 };
        if (::poll(&p, 1, 20) > 0) {
            w.guiAcknowledge();
            ++wakes;
        }
        ScreenLock l(c.mutex);
        if (done())
            break;
    }
    return wakes;
}

void testFlood()
{
    Child ch("head -c 20000000 /dev/zero | tr '\\0' x; echo; echo FLOOD-END");
    Ctx c;
    c.keepData = false;
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &kSink, &c), "start");
    const auto t0 = Clock::now();
    long long seen = 0;
    const int wakes = pumpGui(w, c, 30000, [&] { seen = c.fed; return c.fed >= 20000000 + 11; });
    const double ms = msSince(t0);
    w.stop();
    CHECK(seen >= 20000000, "fed %lld", seen);
    CHECK(wakes <= int(ms / 4.0) + 2, "%d GUI wakes in %.0f ms (throttle is 1 per 4 ms)", wakes, ms);
    std::printf("flood: 20 MB in %.0f ms, %d feeds, %d GUI wakes\n", ms, c.feeds, wakes);
}

void testPauseResume()
{
    constexpr int kN = 200000;
    Child ch("seq 1 200000; echo SEQ-END");
    Ctx c;
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &kSink, &c), "start");
    pumpGui(w, c, 5000, [&] { return c.fed > 10000; });
    long long atPause;
    {
        ScreenLock l(c.mutex);
        w.setPaused(true);
        atPause = c.fed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        ScreenLock l(c.mutex);
        CHECK(c.fed == atPause, "parsed while paused: %lld → %lld", atPause, c.fed);
    }
    // The child is blocked on the full pty, not finished.
    char wchan[64] = {};
    {
        std::string path = "/proc/" + std::to_string(ch.pid) + "/task/" + std::to_string(ch.pid) + "/children";
        FILE *f = std::fopen(path.c_str(), "r");
        int kid = 0;
        if (f) {
            if (std::fscanf(f, "%d", &kid) != 1)
                kid = 0;
            std::fclose(f);
        }
        if (kid > 0) {
            path = "/proc/" + std::to_string(kid) + "/wchan";
            if (FILE *g = std::fopen(path.c_str(), "r")) {
                const size_t got = std::fread(wchan, 1, sizeof wchan - 1, g);
                wchan[got] = 0;
                std::fclose(g);
            }
        }
        CHECK(kid > 0, "seq already finished while paused");
    }
    {
        ScreenLock l(c.mutex);
        w.setPaused(false);
    }
    pumpGui(w, c, 20000, [&] { return c.data.find("SEQ-END") != std::string::npos; });
    w.stop();
    // 1..N, complete and in order.
    int expect = 1;
    size_t pos = 0;
    bool ok = true;
    while (expect <= kN) {
        const size_t nl = c.data.find('\n', pos);
        if (nl == std::string::npos) {
            ok = false;
            break;
        }
        std::string line = c.data.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        pos = nl + 1;
        if (line.empty())
            continue;
        if (std::atoi(line.c_str()) != expect) {
            ok = false;
            std::fprintf(stderr, "  sequence broken: got '%s', want %d\n", line.c_str(), expect);
            break;
        }
        ++expect;
    }
    CHECK(ok, "sequence 1..%d after pause/resume (reached %d)", kN, expect - 1);
    std::printf("pause/resume: frozen at %lld bytes (child wchan while paused: %s), sequence %s\n", atPause,
                wchan[0] ? wchan : "?", ok ? "complete" : "BROKEN");
}

void testOutputNeverBlocks()
{
    // The child reads nothing for a second, then exactly 4 MB, then reports.
    Child ch("sleep 1; head -c 4000000 > /dev/null; echo GOT-ALL");
    Ctx c;
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &kSink, &c), "start");
    std::string payload(4000000, 'p');
    for (size_t i = 4095; i < payload.size(); i += 4096)
        payload[i] = '\n'; // keep lines short (canonical mode would cap a line)
    const auto t0 = Clock::now();
    {
        ScreenLock l(c.mutex);
        w.queueOutput(payload.data(), payload.size());
    }
    const double queueMs = msSince(t0);
    CHECK(queueMs < 200, "queueOutput blocked for %.0f ms", queueMs);
    pumpGui(w, c, 30000, [&] { return c.data.find("GOT-ALL") != std::string::npos; });
    w.stop();
    CHECK(c.data.find("GOT-ALL") != std::string::npos, "the child never received all 4 MB");
    std::printf("output: 4 MB queued in %.1f ms for a child not reading; delivered: %s\n", queueMs,
                c.data.find("GOT-ALL") != std::string::npos ? "yes" : "NO");
}

void testStopMidFlood()
{
    Child ch("cat /dev/zero");
    Ctx c;
    c.keepData = false;
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &kSink, &c), "start");
    pumpGui(w, c, 5000, [&] { return c.fed > 1000000; });
    const auto t0 = Clock::now();
    w.stop();
    const double ms = msSince(t0);
    long long after;
    {
        ScreenLock l(c.mutex);
        after = c.fed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ScreenLock l(c.mutex);
    CHECK(ms < 100, "stop() took %.1f ms", ms);
    CHECK(c.fed == after, "fed after stop()");
    std::printf("stop mid-flood: joined in %.2f ms\n", ms);
}

void testExit()
{
    Child ch("echo bye");
    Ctx c;
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &kSink, &c), "start");
    pumpGui(w, c, 5000, [&] { return c.exited; });
    w.stop();
    CHECK(c.exited, "exit not reported");
    CHECK(c.data.find("bye") != std::string::npos, "output before exit lost");
}

} // namespace

bool cont(unsigned char c) { return (c & 0xc0) == 0x80; }

void testUtf8Boundaries()
{
    // 2-, 3- and 4-byte sequences, written in pieces that cut through them (separate reads), then a
    // long run that crosses many 4 KiB slices.
    Child ch("python3 -c \""
             "import sys,time\n"
             "o=sys.stdout.buffer\n"
             "u='a\\u00e9\\u20ac\\U0001F680'.encode()\n"
             "for k in range(1,len(u)):\n"
             " o.write(u[:k]);o.flush();time.sleep(0.02);o.write(u[k:]);o.flush();time.sleep(0.02)\n"
             "o.write(u*5000);o.write(b'UTF8-END');o.flush()\n"
             "\"");
    Ctx c;
    std::vector<std::string> feeds;
    struct Rec {
        Ctx *c;
        std::vector<std::string> *feeds;
    } rec { &c, &feeds };
    static const TermWorkerSink sink = {
        [](void *u, const char *d, size_t n) {
            auto *r = static_cast<Rec *>(u);
            r->c->data.append(d, n);
            r->c->fed += (long long)n;
            r->feeds->emplace_back(d, n);
            return true;
        },
        [](void *u) { static_cast<Rec *>(u)->c->exited = true; },
        nullptr,
    };
    TermWorker w;
    CHECK(w.start(ch.master, &c.mutex, &sink, &rec), "start");
    pumpGui(w, c, 20000, [&] { return c.data.find("UTF8-END") != std::string::npos; });
    w.stop();
    const std::string u = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x9a\x80";
    std::string want;
    for (size_t k = 1; k < u.size(); ++k)
        want += u;
    for (int k = 0; k < 5000; ++k)
        want += u;
    want += "UTF8-END";
    CHECK(c.data == want, "data intact (%zu bytes, want %zu)", c.data.size(), want.size());
    int bad = 0;
    for (const std::string &f : feeds) {
        if (f.empty())
            continue;
        const unsigned char first = static_cast<unsigned char>(f.front());
        size_t back = 0;
        while (back < 3 && back < f.size() && cont(static_cast<unsigned char>(f[f.size() - 1 - back])))
            ++back;
        const unsigned char lead = back < f.size() ? static_cast<unsigned char>(f[f.size() - 1 - back]) : 0;
        const size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (cont(first) || need > back + 1)
            ++bad;
    }
    CHECK(bad == 0, "%d of %zu feeds start or end inside a UTF-8 sequence", bad, feeds.size());
    std::printf("utf8: %zu feeds, %d split a sequence\n", feeds.size(), bad);
}

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    testFlood();
    testPauseResume();
    testOutputNeverBlocks();
    testStopMidFlood();
    testExit();
    testUtf8Boundaries();
    if (g_failures)
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
    else
        std::printf("termworker: all passed\n");
    return g_failures ? 1 : 0;
}
