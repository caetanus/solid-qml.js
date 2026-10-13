#pragma once

// One tmux control client (docs/solidterm-tmux-control-mode.md 1.1, 1.5, 3.1, 4). Qt-free.
//
// `tmux -C …` is spawned with stdin/stdout/stderr on pipes (no tty: no echo, no \r\n, no canonical
// line limit, no DCS framing). A reader thread owns the control stream: it parses it in stream order
// and routes every pane's decoded %output into that pane's FEED PIPE, whose read end the pane's
// existing TermWorker reads exactly as it reads a pty master — TermWorker, TermScreen and the
// renderer are unchanged, and the panes keep parsing in parallel (one connection, N parser threads).
//
// The reader thread also does everything that must happen at a precise point of the stream:
//   - seeding (4.2-4.3): panes that existed at attach are "seeding" from the initial list-panes reply
//     on; their output is DROPPED (the capture will contain it) until the seed group's last block,
//     when the synthesised stream goes into the feed pipe and the pane goes live. Output from a pane
//     created later is complete from its start: it is held in the pipe until a view reads it;
//   - the resize fence (3.4): a %layout-change that changes a pane's size holds that pane's output
//     in user space until the GUI reports the new grid applied (ackPaneSize);
//   - %pause (1.6, pause-after): once the pane's held output has drained, one group resumes the pane
//     and re-snapshots it (refresh-client -A '%P:continue' ; display-message ; capture-pane);
//   - overflow: a full feed pipe (paused or slow view) never blocks the reader — the rest waits in a
//     per-pane buffer; past the overflow cap the pane is paused in tmux and reseeded once it drains.
// Everything else (notifications, replies the GUI asked for, exit) is queued for the GUI thread,
// signalled through an eventfd it watches (coalesced, like TermWorker's news).
//
// Only the reader thread writes to file descriptors (it blocks SIGPIPE for itself), so a dead tmux or
// a released pane can never kill the process.

#include "passthrough.h"
#include "protocol.h"
#include "seed.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tmux {

// Reader → GUI.
struct Event {
    enum Type : uint8_t { Notify, Reply, Exit, Log };
    Type type = Log;
    // Notify (the fields of the Notification, copied)
    Note note = Note::Unknown;
    int session = -1, window = -1, pane = -1;
    std::string text, text2, text3;
    // Reply
    uint16_t kind = 0;
    uint64_t tag = 0;
    bool error = false;
    bool dropped = false;                // a group member removed by an earlier %error (never ran)
    std::vector<std::string> lines;      // raw block lines (error text on %error)
    // Exit: `text` = the %exit reason (empty for a plain detach), text2 = "eof" when the stream ended
};

class Client {
public:
    // Command kinds. The GUI uses the low ones; Seed* and Reseed* are the reader's own.
    enum Kind : uint16_t {
        KIgnore = 0,        // errors are logged
        KUser,              // reply forwarded to the GUI
        KListWindows,       // forwarded
        KListPanes,         // the reader learns panes (the FIRST one starts seeding), then forwarded
        KDetach,
        KSeedProbe,         // display-message alone: alternate screen? how much history?
        KSeedState,         // display-message inside the capture group (the state AT the capture)
        KSeedHistory,
        KSeedNormal,
        KSeedScreen,
        KSeedPending,
        KReseedContinue,
        KPauseRequest,      // refresh-client -A '%P:pause' after an overflow
    };

    static constexpr size_t kOverflowCap = 8u << 20; // default, per pane (owner decision 1: capped buffer)
    void setOverflowCap(size_t bytes) { m_overflowCap.store(bytes ? bytes : kOverflowCap, std::memory_order_relaxed); }

    Client();
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Spawns `binary args…` with the environment minus TMUX/TMUX_PANE and starts the reader thread.
    bool start(const std::string &binary, const std::vector<std::string> &args, std::string *error);
    // Joins the reader; the child gets `graceMs` to exit (after a detach), then SIGTERM, then SIGKILL.
    void stop(int graceMs = 2000);
    bool started() const { return m_thread.joinable(); }
    // After stop(): how the tmux client process ended ("exited with status 0", "killed by signal 9");
    // empty while it has not been reaped.
    std::string exitStatusText() const { return m_exitText; }
    pid_t pid() const { return m_pid; }

    // GUI side ────────────────────────────────────────────────────────────────────────────────────
    int guiFd() const { return m_guiFd; }
    void takeEvents(std::vector<Event> &out); // call when guiFd is readable

    // Queue one command line (all its commands get `kind`/`tag`). False if the line is unsendable.
    bool send(const CommandLine &line, uint16_t kind, uint64_t tag = 0);
    void detach();

    // Lines of history a seed fetches (our scrollback limit).
    void setHistoryLimit(int lines) { m_historyLimit.store(lines < 0 ? 0 : lines, std::memory_order_relaxed); }

    // The read end of pane `pane`'s feed pipe (created on demand; owned by the client: hand it to a
    // TermWorker, never close it). -1 when the pipe cannot be created.
    int openPane(int pane);
    // The view is gone (its worker stopped): close the pipe and forget the pane.
    void releasePane(int pane);
    // The view applied a cols x rows grid: lifts a resize fence waiting for exactly that size.
    void ackPaneSize(int pane, int cols, int rows);

    // Tests: copy every byte written to a feed pipe (oracle checks), and read the copy.
    void setTee(bool on) { m_teeOn.store(on, std::memory_order_relaxed); }
    std::string tee(int pane);
    // Tests: panes whose output is currently held (seeding, fenced, overflow) and seed counters.
    int seedsCompleted() const { return m_seedsDone.load(std::memory_order_relaxed); }
    int reseedsCompleted() const { return m_reseedsDone.load(std::memory_order_relaxed); }

    // ControlParser handler (reader thread, m_mx held) ───────────────────────────────────────────
    void onOutput(int pane, char *data, size_t len);
    void onBlockBegin(const BlockGuard &g);
    void onBlockLine(std::string_view line);
    void onBlockEnd(const BlockGuard &g, bool error);
    void onNotification(const Notification &n);
    void onStray(std::string_view line);

private:
    struct Route {
        int readFd = -1, writeFd = -1;
        enum State : uint8_t { Seeding, Live } state = Live;
        int cols = -1, rows = -1;          // tmux's size as last seen in a list or layout
        bool fenced = false;               // hold output until the GUI applied fenceCols x fenceRows
        int fenceCols = 0, fenceRows = 0;
        bool paused = false;               // tmux paused it (%pause): reseed once `over` drains
        bool pauseRequested = false;       // we asked for the pause (overflow cap)
        bool reseedQueued = false;
        int seedTries = 0;
        bool reseed = false;               // the running snapshot group is a reseed
        bool seedFailed = false;           // a member of the running capture group failed
        PaneSnapshot snap;
        SeedCapture cap;
        std::string over;                  // overflow, from overHead on
        size_t overHead = 0;
        std::string tee;
        PassthroughUnwrapper unwrap;       // "ESC P tmux; … ESC \\" → the inner sequence (7)
        bool overPending() const { return overHead < over.size(); }
    };

    void run();
    void wake();
    void post(Event &&e);                  // m_mx held
    void log(std::string text);            // m_mx held
    void pushLine(const CommandLine &line, const Pending *entries, size_t n); // m_mx held
    Route &route(int pane);                // m_mx held: find or create (with its pipe)
    Route *findRoute(int pane);
    void write(Route &r, int pane, const char *data, size_t len); // m_mx held
    void flushRoute(Route &r, int pane);   // m_mx held
    void flushStdin();                     // m_mx held
    void startSeed(Route &r, int pane);    // display-message probe
    void startCapture(Route &r, int pane);
    void startReseed(Route &r, int pane);
    void finishSeed(Route &r, int pane, bool ok);
    void handleReply(const Pending &p, bool error, bool dropped);
    void applyLayoutSizes(std::string_view layout, std::string_view visible);

    pid_t m_pid = -1;
    std::string m_exitText;
    int m_in = -1, m_out = -1, m_err = -1; // child's stdin (we write), stdout, stderr (we read)
    int m_wakeFd = -1, m_guiFd = -1;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
    std::atomic<int> m_historyLimit { 8000 };
    std::atomic<size_t> m_overflowCap { kOverflowCap };
    std::atomic<bool> m_teeOn { false };
    std::atomic<int> m_seedsDone { 0 }, m_reseedsDone { 0 };

    std::mutex m_mx;                       // everything below
    ControlParser<Client> m_parser { *this };
    CommandQueue m_queue;
    std::vector<std::string> m_blockLines;
    std::string m_stdin;                   // command bytes not yet written
    size_t m_stdinHead = 0;
    std::unordered_map<int, Route> m_routes;
    bool m_listed = false;                 // the initial list-panes reply was processed
    std::vector<Event> m_events;
    bool m_posted = false;
    std::string m_exitReason;
    bool m_sawExit = false;
    bool m_outEof = false, m_errEof = false;
    std::string m_errCarry;
    std::string m_unwrapBefore;            // reused: bytes the unwrapper hands back
};

} // namespace tmux
