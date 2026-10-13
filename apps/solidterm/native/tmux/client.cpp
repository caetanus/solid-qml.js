#include "client.h"

#include "layout.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace tmux {

namespace {

void signalFd(int fd)
{
    const uint64_t one = 1;
    ssize_t r;
    do {
        r = ::write(fd, &one, sizeof one);
    } while (r < 0 && errno == EINTR);
}

void drainFd(int fd)
{
    uint64_t v;
    while (::read(fd, &v, sizeof v) > 0) {}
}

void closeFd(int &fd)
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

void setNonBlock(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

} // namespace

Client::Client() = default;

Client::~Client()
{
    stop(0);
    closeFd(m_wakeFd);
    closeFd(m_guiFd);
    for (auto &kv : m_routes) {
        closeFd(kv.second.readFd);
        closeFd(kv.second.writeFd);
    }
}

bool Client::start(const std::string &binary, const std::vector<std::string> &args, std::string *error)
{
    if (m_thread.joinable())
        return false;
    int in[2], out[2], err[2];
    if (::pipe2(in, O_CLOEXEC) != 0)
        return false;
    if (::pipe2(out, O_CLOEXEC) != 0) {
        ::close(in[0]); ::close(in[1]);
        return false;
    }
    if (::pipe2(err, O_CLOEXEC) != 0) {
        ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);  // dup2 clears FD_CLOEXEC on the target
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    // Own session: a ^C or SIGHUP aimed at SolidTerm's controlling terminal must not reach tmux.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(binary.c_str()));
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    // Without TMUX: new-session / attach refuse to nest ("sessions should be nested with care").
    std::vector<char *> envp;
    for (char **e = environ; *e; ++e)
        if (std::strncmp(*e, "TMUX=", 5) != 0 && std::strncmp(*e, "TMUX_PANE=", 10) != 0)
            envp.push_back(*e);
    envp.push_back(nullptr);

    const int rc = posix_spawnp(&m_pid, binary.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (rc != 0) {
        if (error)
            *error = std::string("cannot run ") + binary + ": " + std::strerror(rc);
        ::close(in[1]); ::close(out[0]); ::close(err[0]);
        m_pid = -1;
        return false;
    }
    m_in = in[1];
    m_out = out[0];
    m_err = err[0];
    setNonBlock(m_in);
    setNonBlock(m_out);
    setNonBlock(m_err);
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_guiFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_thread = std::thread([this] { run(); });
    return true;
}

void Client::stop(int graceMs)
{
    if (m_thread.joinable()) {
        m_stop.store(true, std::memory_order_release);
        wake();
        m_thread.join();
    }
    // Our end of the command pipe closing is a detach for tmux too (EOF on the client's stdin).
    closeFd(m_in);
    closeFd(m_out);
    closeFd(m_err);
    if (m_pid > 0) {
        int status = 0;
        auto reaped = [&] { return ::waitpid(m_pid, &status, WNOHANG) == m_pid; };
        bool done = reaped();
        for (int waited = 0; !done && waited < graceMs; waited += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            done = reaped();
        }
        if (!done) {
            ::kill(m_pid, SIGTERM);
            for (int waited = 0; !done && waited < 500; waited += 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                done = reaped();
            }
        }
        if (!done) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, &status, 0);
        }
        m_pid = -1;
    }
}

void Client::wake()
{
    if (m_wakeFd >= 0)
        signalFd(m_wakeFd);
}

// ─── GUI side ───────────────────────────────────────────────────────────────────────────────────

void Client::takeEvents(std::vector<Event> &out)
{
    drainFd(m_guiFd);
    std::lock_guard<std::mutex> lock(m_mx);
    if (out.empty())
        out.swap(m_events);
    else {
        for (Event &e : m_events)
            out.push_back(std::move(e));
        m_events.clear();
    }
    m_posted = false;
}

bool Client::send(const CommandLine &line, uint16_t kind, uint64_t tag)
{
    static const bool trace = std::getenv("SOLIDTERM_TMUX_TRACE") != nullptr; // debugging aid
    if (trace)
        std::fprintf(stderr, "tmux> %s%s\n", line.str().c_str(), line.ok() ? "" : "  [NOT SENT: newline/NUL]");
    if (!line.ok() || line.commands() == 0)
        return false;
    std::vector<Pending> entries(line.commands());
    for (Pending &p : entries) {
        p.kind = kind;
        p.tag = tag;
    }
    {
        std::lock_guard<std::mutex> lock(m_mx);
        pushLine(line, entries.data(), entries.size());
    }
    wake(); // only the reader writes (SIGPIPE stays confined to it)
    return true;
}

void Client::detach()
{
    CommandLine l;
    l.cmd("detach-client");
    send(l, KDetach);
}

int Client::openPane(int pane)
{
    std::lock_guard<std::mutex> lock(m_mx);
    return route(pane).readFd;
}

void Client::releasePane(int pane)
{
    std::lock_guard<std::mutex> lock(m_mx);
    auto it = m_routes.find(pane);
    if (it == m_routes.end())
        return;
    closeFd(it->second.readFd);
    closeFd(it->second.writeFd);
    m_routes.erase(it);
}

void Client::ackPaneSize(int pane, int cols, int rows)
{
    bool lifted = false;
    {
        std::lock_guard<std::mutex> lock(m_mx);
        Route *r = findRoute(pane);
        if (r && r->fenced && r->fenceCols == cols && r->fenceRows == rows) {
            r->fenced = false;
            lifted = r->overPending() || r->paused;
        }
    }
    if (lifted)
        wake(); // the reader flushes what the fence held
}

std::string Client::tee(int pane)
{
    std::lock_guard<std::mutex> lock(m_mx);
    Route *r = findRoute(pane);
    return r ? r->tee : std::string();
}

// ─── reader thread ──────────────────────────────────────────────────────────────────────────────

void Client::post(Event &&e)
{
    m_events.push_back(std::move(e));
    if (!m_posted) {
        m_posted = true;
        signalFd(m_guiFd);
    }
}

void Client::log(std::string text)
{
    Event e;
    e.type = Event::Log;
    e.text = std::move(text);
    post(std::move(e));
}

void Client::pushLine(const CommandLine &line, const Pending *entries, size_t n)
{
    m_queue.pushGroup(entries, n);
    if (m_stdinHead == m_stdin.size()) {
        m_stdin.clear();
        m_stdinHead = 0;
    }
    m_stdin += line.str();
    m_stdin += '\n';
}

void Client::flushStdin()
{
    while (m_in >= 0 && m_stdinHead < m_stdin.size()) {
        const ssize_t w = ::write(m_in, m_stdin.data() + m_stdinHead, m_stdin.size() - m_stdinHead);
        if (w > 0) {
            m_stdinHead += size_t(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
            return;
        m_stdinHead = m_stdin.size(); // EPIPE: tmux is gone; its exit arrives as EOF
    }
    m_stdin.clear();
    m_stdinHead = 0;
}

Client::Route *Client::findRoute(int pane)
{
    auto it = m_routes.find(pane);
    return it == m_routes.end() ? nullptr : &it->second;
}

Client::Route &Client::route(int pane)
{
    auto it = m_routes.find(pane);
    if (it != m_routes.end())
        return it->second;
    Route &r = m_routes[pane];
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        r.readFd = fds[0];
        r.writeFd = fds[1];
        ::fcntl(r.writeFd, F_SETPIPE_SZ, 1 << 20); // fewer trips through the overflow (best effort)
    }
    return r;
}

void Client::write(Route &r, int pane, const char *data, size_t len)
{
    if (len == 0)
        return;
    if (m_teeOn.load(std::memory_order_relaxed))
        r.tee.append(data, len);
    if (!r.fenced && !r.overPending() && r.writeFd >= 0) {
        ssize_t w;
        do {
            w = ::write(r.writeFd, data, len);
        } while (w < 0 && errno == EINTR);
        if (w == ssize_t(len))
            return;
        if (w < 0) {
            if (errno != EAGAIN)
                return; // the pane was released under us: nothing reads it any more
            w = 0;
        }
        data += w;
        len -= size_t(w);
    }
    if (r.overHead > (1u << 20) && r.overHead * 2 > r.over.size()) {
        r.over.erase(0, r.overHead);
        r.overHead = 0;
    }
    r.over.append(data, len);
    if (r.over.size() - r.overHead > kOverflowCap && !r.pauseRequested && !r.paused) {
        // A view that stays paused (selection) or a pane nobody shows: stop tmux sending it; it is
        // reseeded once what we hold has drained.
        r.pauseRequested = true;
        CommandLine l;
        l.cmd("refresh-client").flag("-A").arg("%" + std::to_string(pane) + ":pause");
        Pending p;
        p.kind = KPauseRequest;
        p.pane = pane;
        pushLine(l, &p, 1);
    }
}

void Client::flushRoute(Route &r, int pane)
{
    while (!r.fenced && r.overPending() && r.writeFd >= 0) {
        const ssize_t w = ::write(r.writeFd, r.over.data() + r.overHead, r.over.size() - r.overHead);
        if (w > 0) {
            r.overHead += size_t(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
            return;
        r.overHead = r.over.size(); // released
    }
    if (!r.overPending()) {
        r.over.clear();
        r.overHead = 0;
        if (r.paused && !r.reseedQueued && !r.fenced)
            startReseed(r, pane);
    }
}

void Client::onOutput(int pane, char *data, size_t len)
{
    if (!m_listed)
        return; // before the initial list: every pane that exists is seeded, the capture holds this
    Route &r = route(pane);
    if (r.state == Route::Seeding)
        return; // contained in the capture that is on its way (4.3, rule 1)
    write(r, pane, data, len);
}

void Client::onBlockBegin(const BlockGuard &)
{
    m_blockLines.clear();
}

void Client::onBlockLine(std::string_view line)
{
    m_blockLines.emplace_back(line);
}

void Client::onBlockEnd(const BlockGuard &g, bool error)
{
    if (g.flags != 1)
        return; // the argv command, a hook, the config: not an answer to us
    Pending done;
    std::vector<Pending> dropped;
    if (!m_queue.complete(error, done, dropped)) {
        log("tmux: a reply with no command pending (block " + std::to_string(g.number) + ")");
        return;
    }
    handleReply(done, error, false);
    m_blockLines.clear();
    for (const Pending &d : dropped)
        handleReply(d, true, true);
}

void Client::onStray(std::string_view line)
{
    log("tmux: " + std::string(line));
}

void Client::onNotification(const Notification &n)
{
    switch (n.type) {
    case Note::Pause:
        if (Route *r = findRoute(n.pane)) {
            r->paused = true;
            if (!r->overPending() && !r->fenced && !r->reseedQueued)
                startReseed(*r, n.pane);
        }
        break;
    case Note::Continue:
        if (Route *r = findRoute(n.pane))
            r->paused = false;
        break;
    case Note::LayoutChange:
        applyLayoutSizes(n.text, n.text2);
        break;
    case Note::Exit:
        m_sawExit = true;
        m_exitReason.assign(n.text);
        break;
    default:
        break;
    }
    Event e;
    e.type = Event::Notify;
    e.note = n.type;
    e.session = n.session;
    e.window = n.window;
    e.pane = n.pane;
    e.text.assign(n.type == Note::Unknown ? n.line : n.text);
    e.text2.assign(n.text2);
    e.text3.assign(n.text3);
    post(std::move(e));
}

void Client::applyLayoutSizes(std::string_view layout, std::string_view visible)
{
    Layout l, v;
    if (parseLayout(layout, l) != LayoutError::None)
        return; // the GUI reports it
    const bool hasVisible = parseLayout(visible, v) == LayoutError::None;
    std::vector<const LayoutCell *> leaves, vis;
    layoutLeaves(l.root, leaves);
    if (hasVisible)
        layoutLeaves(v.root, vis);
    for (const LayoutCell *leaf : leaves) {
        int cols = int(leaf->sx), rows = int(leaf->sy);
        for (const LayoutCell *z : vis) // zoomed: the visible layout holds the pane at full size
            if (z->pane == leaf->pane) {
                cols = int(z->sx);
                rows = int(z->sy);
            }
        Route *r = findRoute(leaf->pane);
        if (!r)
            continue;
        if (r->cols >= 0 && (r->cols != cols || r->rows != rows)) {
            // Output after this point was produced for the new size (the program got SIGWINCH):
            // hold it until the view's parser has the new grid (3.4).
            r->fenced = true;
            r->fenceCols = cols;
            r->fenceRows = rows;
        }
        r->cols = cols;
        r->rows = rows;
    }
}

void Client::startSeed(Route &r, int pane)
{
    r.state = Route::Seeding;
    r.reseed = false;
    r.cap = SeedCapture {};
    CommandLine l;
    l.cmd("display-message").flag("-p").pane("-t", pane).arg(snapshotFormat());
    Pending p;
    p.kind = KSeedProbe;
    p.pane = pane;
    pushLine(l, &p, 1);
}

void Client::startCapture(Route &r, int pane)
{
    const long limit = m_historyLimit.load(std::memory_order_relaxed);
    const long n = r.snap.historySize < limit ? r.snap.historySize : limit;
    CommandLine l;
    std::vector<Pending> e;
    auto add = [&](uint16_t kind) {
        Pending p;
        p.kind = kind;
        p.pane = pane;
        e.push_back(p);
    };
    l.cmd("display-message").flag("-p").pane("-t", pane).arg(snapshotFormat());
    add(KSeedState);
    if (!r.snap.alternate) {
        // History + screen, wrapped lines joined: fed back at this width they re-wrap into the same
        // rows with the same wrap flags (4.2).
        l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-e").flag("-C").flag("-J").number("-S", -n).flagArg("-E", "-");
        add(KSeedNormal);
    } else {
        if (n > 0) {
            l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-e").flag("-C").flag("-J").number("-S", -n).number("-E", -1);
            add(KSeedHistory);
        }
        l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-a").flag("-q").flag("-e").flag("-C").flag("-N");
        add(KSeedNormal);
        l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-e").flag("-C").flag("-N").number("-S", 0).flagArg("-E", "-");
        add(KSeedScreen);
    }
    l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-P").flag("-C");
    add(KSeedPending);
    r.seedFailed = false;
    pushLine(l, e.data(), e.size());
}

void Client::startReseed(Route &r, int pane)
{
    r.state = Route::Seeding;
    r.reseed = true;
    r.reseedQueued = true;
    r.cap = SeedCapture {};
    // One group: `continue` moves tmux's offset to the pane's current position at the same instant
    // the capture reads the grid, so the new stream starts exactly where the snapshot ends (4.4).
    CommandLine l;
    std::vector<Pending> e(4);
    l.cmd("refresh-client").flag("-A").arg("%" + std::to_string(pane) + ":continue");
    e[0].kind = KReseedContinue;
    l.cmd("display-message").flag("-p").pane("-t", pane).arg(snapshotFormat());
    e[1].kind = KSeedState;
    l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-e").flag("-C").flag("-N");
    e[2].kind = KSeedScreen;
    l.cmd("capture-pane").flag("-p").pane("-t", pane).flag("-P").flag("-C");
    e[3].kind = KSeedPending;
    for (Pending &p : e)
        p.pane = pane;
    r.seedFailed = false;
    pushLine(l, e.data(), e.size());
}

void Client::finishSeed(Route &r, int pane, bool ok)
{
    if (!ok) {
        if (++r.seedTries < 2) {
            if (r.reseed)
                startReseed(r, pane);
            else
                startSeed(r, pane);
            return;
        }
        log("tmux: could not snapshot pane %" + std::to_string(pane) + "; showing its output from now on");
        r.state = Route::Live;
        r.reseed = r.reseedQueued = r.paused = r.pauseRequested = false;
        r.seedTries = 0;
        return;
    }
    std::string bytes;
    if (r.reseed)
        buildReseed(bytes, r.snap, r.cap);
    else
        buildSeed(bytes, r.snap, r.cap);
    r.state = Route::Live;
    write(r, pane, bytes.data(), bytes.size());
    if (r.reseed)
        m_reseedsDone.fetch_add(1, std::memory_order_relaxed);
    else
        m_seedsDone.fetch_add(1, std::memory_order_relaxed);
    r.reseed = r.reseedQueued = r.paused = r.pauseRequested = false;
    r.seedTries = 0;
    r.cap = SeedCapture {};
}

void Client::handleReply(const Pending &p, bool error, bool dropped)
{
    switch (p.kind) {
    case KListPanes:
        if (!error) {
            // "%P @W active cols rows path": the panes (and sizes) as of this point of the stream.
            for (const std::string &line : m_blockLines) {
                std::string_view rest = line;
                std::string_view f[5];
                for (std::string_view &w : f) {
                    const size_t sp = rest.find(' ');
                    w = rest.substr(0, sp);
                    rest = sp == std::string_view::npos ? std::string_view() : rest.substr(sp + 1);
                }
                const int pane = parseId(f[0], '%');
                if (pane < 0)
                    continue;
                Route &r = route(pane);
                const int cols = std::atoi(std::string(f[3]).c_str()), rows = std::atoi(std::string(f[4]).c_str());
                if (cols > 0 && rows > 0 && r.cols < 0) {
                    r.cols = cols;
                    r.rows = rows;
                }
                if (!m_listed)
                    startSeed(r, pane); // existed at attach: its past is only in tmux's grid
            }
            m_listed = true;
        }
        break;
    case KSeedProbe: {
        Route *r = findRoute(p.pane);
        if (!r)
            return; // released meanwhile
        if (error || m_blockLines.empty() || !parseSnapshot(m_blockLines.front(), r->snap)) {
            finishSeed(*r, p.pane, false);
            break;
        }
        startCapture(*r, p.pane);
        return;
    }
    case KReseedContinue:
    case KSeedState:
    case KSeedHistory:
    case KSeedNormal:
    case KSeedScreen:
    case KSeedPending: {
        Route *r = findRoute(p.pane);
        if (!r)
            return;
        if (error || dropped) {
            r->seedFailed = true;
        } else if (p.kind == KSeedState) {
            const bool wasAlt = r->snap.alternate;
            if (!m_blockLines.empty() && parseSnapshot(m_blockLines.front(), r->snap)) {
                if (!r->reseed && r->snap.alternate != wasAlt)
                    r->seedFailed = true; // the screen flipped between probe and capture: probe again
            } else {
                r->seedFailed = true;
            }
        } else if (p.kind == KSeedHistory) {
            r->cap.history = std::move(m_blockLines);
        } else if (p.kind == KSeedNormal) {
            r->cap.normal = std::move(m_blockLines);
        } else if (p.kind == KSeedScreen) {
            r->cap.screen = std::move(m_blockLines);
        } else if (p.kind == KSeedPending) {
            r->cap.pending = std::move(m_blockLines);
        }
        m_blockLines.clear();
        if (p.lastInGroup) {
            finishSeed(*r, p.pane, !r->seedFailed);
        }
        return;
    }
    case KPauseRequest:
    case KIgnore:
        if (error && !dropped)
            log("tmux: command failed: " + (m_blockLines.empty() ? std::string() : m_blockLines.front()));
        return;
    default:
        break;
    }
    if (p.kind == KIgnore)
        return;
    Event e;
    e.type = Event::Reply;
    e.kind = p.kind;
    e.tag = p.tag;
    e.error = error;
    e.dropped = dropped;
    if (!dropped)
        e.lines = m_blockLines;
    post(std::move(e));
}

void Client::run()
{
    ::pthread_setname_np(::pthread_self(), "st-tmux");
    sigset_t pipeSig;
    sigemptyset(&pipeSig);
    sigaddset(&pipeSig, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &pipeSig, nullptr); // writes get EPIPE instead of killing us
    std::vector<char> buf(64 * 1024);
    std::vector<pollfd> fds;
    std::vector<int> fdPane;
    bool exitPosted = false;
    while (!m_stop.load(std::memory_order_acquire)) {
        fds.clear();
        fdPane.clear();
        {
            std::lock_guard<std::mutex> lock(m_mx);
            flushStdin();
            for (auto &kv : m_routes)
                if (kv.second.overPending() && !kv.second.fenced)
                    flushRoute(kv.second, kv.first);
            flushStdin(); // a reseed queued by a drained route
            fds.push_back({ m_wakeFd, POLLIN, 0 });
            fdPane.push_back(-1);
            if (!m_outEof) {
                fds.push_back({ m_out, POLLIN, 0 });
                fdPane.push_back(-2);
            }
            if (!m_errEof) {
                fds.push_back({ m_err, POLLIN, 0 });
                fdPane.push_back(-3);
            }
            if (m_in >= 0 && m_stdinHead < m_stdin.size()) {
                fds.push_back({ m_in, POLLOUT, 0 });
                fdPane.push_back(-4);
            }
            for (auto &kv : m_routes)
                if (kv.second.overPending() && !kv.second.fenced && kv.second.writeFd >= 0) {
                    fds.push_back({ kv.second.writeFd, POLLOUT, 0 });
                    fdPane.push_back(kv.first);
                }
            if (m_outEof && m_errEof && !exitPosted) {
                exitPosted = true;
                Event e;
                e.type = Event::Exit;
                e.text = m_exitReason;
                e.text2 = "eof";
                post(std::move(e));
            }
        }
        const int n = ::poll(fds.data(), nfds_t(fds.size()), -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (size_t k = 0; k < fds.size(); ++k) {
            const short re = fds[k].revents;
            if (!re)
                continue;
            const int who = fdPane[k];
            if (who == -1) {
                drainFd(m_wakeFd);
            } else if (who == -2) {
                const ssize_t got = ::read(m_out, buf.data(), buf.size());
                if (got > 0) {
                    std::lock_guard<std::mutex> lock(m_mx);
                    m_parser.feed(buf.data(), size_t(got));
                    flushStdin(); // commands the stream triggered (seeds, reseeds)
                } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                    std::lock_guard<std::mutex> lock(m_mx);
                    m_outEof = true;
                }
            } else if (who == -3) {
                const ssize_t got = ::read(m_err, buf.data(), buf.size());
                std::lock_guard<std::mutex> lock(m_mx);
                if (got > 0) {
                    m_errCarry.append(buf.data(), size_t(got));
                    size_t nl;
                    while ((nl = m_errCarry.find('\n')) != std::string::npos) {
                        log("tmux stderr: " + m_errCarry.substr(0, nl));
                        m_errCarry.erase(0, nl + 1);
                    }
                } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                    if (!m_errCarry.empty())
                        log("tmux stderr: " + m_errCarry);
                    m_errCarry.clear();
                    m_errEof = true;
                }
            } else if (who == -4) {
                std::lock_guard<std::mutex> lock(m_mx);
                flushStdin();
            } else {
                std::lock_guard<std::mutex> lock(m_mx);
                if (Route *r = findRoute(who))
                    flushRoute(*r, who);
            }
        }
    }
}

} // namespace tmux
