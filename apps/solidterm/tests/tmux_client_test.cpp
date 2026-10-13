// tmux::Client (apps/solidterm/native/tmux/client.h) end to end, without Qt: the real reader thread
// and feed pipes against a scripted FAKE tmux (tests/tmux/fake-tmux.py + scenario-basic.py), each
// pane's pipe parsed by a bare libvterm + TermScreen exactly as a TerminalView's worker would.
//
// Checks (docs/solidterm-tmux-control-mode.md 4.2-4.4, 3.4, 1.6, 5.1):
//  - seeding: a pane's first bytes are the synthesised seed (output racing the snapshot — after the
//    probe and right before the capture group — is dropped, so it shows exactly ONCE, from the
//    capture); output after the group is kept; history lands in the scrollback; SGR survives;
//    the cursor is where tmux had it; an alternate-screen pane comes back on the alternate screen
//    with its saved normal screen and history underneath;
//  - %output with backslashes, control bytes, UTF-8 and an escape sequence cut across lines;
//  - a new pane (created after attach) gets its whole output, held until it is opened;
//  - the resize fence: output after a size-changing %layout-change waits for ackPaneSize;
//  - %pause → one continue+snapshot group → reseed (marker row in the scrollback, fresh screen);
//  - detach-client → %exit → the process ends; the fake's log shows what we sent.
// With argv[1] = "--real <tmux binary> <scratch dir>" the same client attaches to a REAL tmux on a
// private socket instead (see runReal) — skipped (exit 77) when the binary is absent.
#include "../native/termscreen.h"
#include "../native/tmux/client.h"
#include "../native/tmux/layout.h"

#include <vterm.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef TMUX_TESTS_DIR
#define TMUX_TESTS_DIR "apps/solidterm/tests/tmux"
#endif

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    } else {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

// One pane "view": the parser a TerminalView would run, reading the pane's feed pipe.
struct View {
    TermScreen screen;
    VTerm *vt = nullptr;
    int fd = -1;
    int pane = -1;
    std::string fed;
    View(int p, int rows, int cols, int sb)
        : pane(p)
    {
        vt = TermScreen::newVTerm(rows, cols);
        vterm_set_utf8(vt, 1);
        screen.attach(vt, nullptr, nullptr);
        screen.reset(true);
        screen.setScrollbackLimit(sb);
    }
    ~View() { vterm_free(vt); }
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    // Reads like TermWorker: an incomplete UTF-8 sequence at the end of a read waits for the next
    // one (libvterm does not carry it across vterm_input_write calls).
    std::string held;
    void pump()
    {
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0)
                return;
            fed.append(buf, size_t(n));
            held.append(buf, size_t(n));
            size_t back = 0;
            while (back < 3 && back < held.size() && (static_cast<unsigned char>(held[held.size() - 1 - back]) & 0xc0) == 0x80)
                ++back;
            const unsigned char lead = back < held.size() ? static_cast<unsigned char>(held[held.size() - 1 - back]) : 0;
            const size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
            const size_t tail = need > back + 1 ? back + 1 : 0;
            vterm_input_write(vt, held.data(), held.size() - tail);
            held.erase(0, held.size() - tail);
        }
    }
    std::string row(int absRow) const
    {
        const TermScreen::Line l = screen.line(absRow);
        if (!l.cells)
            return "<none>";
        std::string out;
        for (int c = 0; c < l.cols; ++c) {
            const uint32_t ch = l.cells[c].chars[0];
            if (ch == kWideContinuation)
                continue;
            if (ch == 0)
                out += ' ';
            else if (ch < 0x80)
                out += char(ch);
            else if (ch < 0x800) {
                out += char(0xc0 | (ch >> 6));
                out += char(0x80 | (ch & 0x3f));
            } else if (ch < 0x10000) {
                out += char(0xe0 | (ch >> 12));
                out += char(0x80 | ((ch >> 6) & 0x3f));
                out += char(0x80 | (ch & 0x3f));
            } else {
                out += char(0xf0 | (ch >> 18));
                out += char(0x80 | ((ch >> 12) & 0x3f));
                out += char(0x80 | ((ch >> 6) & 0x3f));
                out += char(0x80 | (ch & 0x3f));
            }
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }
    // Scrollback + screen, one line per row.
    std::vector<std::string> all() const
    {
        std::vector<std::string> v;
        for (int r = 0; r < screen.sbCount() + screen.rows(); ++r)
            v.push_back(row(r));
        return v;
    }
    int count(const std::string &needle) const
    {
        int n = 0;
        for (const std::string &l : all())
            for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1))
                ++n;
        return n;
    }
    int findRow(const std::string &needle) const // absolute row, -1 none
    {
        const auto v = all();
        for (int r = 0; r < int(v.size()); ++r)
            if (v[size_t(r)].find(needle) != std::string::npos)
                return r;
        return -1;
    }
    std::string screenRow(int r) const { return row(screen.sbCount() + r); }
    VTermPos cursor() const
    {
        VTermPos p;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &p);
        return p;
    }
    void resize(int rows, int cols) { vterm_set_size(vt, rows, cols); }
    std::string dump() const
    {
        std::string s;
        for (const std::string &l : all())
            s += "  |" + l + "\n";
        return s;
    }
};

struct Harness {
    tmux::Client client;
    std::map<int, std::unique_ptr<View>> views;
    std::vector<tmux::Event> events;   // everything seen, in order
    bool sessionChanged = false, exited = false;
    std::vector<std::string> listPanes, listWindows;
    int sb = 2000;
    bool autoLayout = true;               // apply %layout-change sizes to the views, as the GUI does
    std::map<int, std::pair<int, int>> sizes; // pane → (cols, rows) from the latest layout

    void pump(int ms)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            pollfd p { client.guiFd(), POLLIN, 0 };
            ::poll(&p, 1, 5);
            std::vector<tmux::Event> ev;
            client.takeEvents(ev);
            for (tmux::Event &e : ev)
                handle(e);
            for (auto &kv : views)
                kv.second->pump();
        }
    }
    bool pumpUntil(const std::function<bool()> &cond, int ms)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            pump(10);
            if (cond())
                return true;
        }
        return cond();
    }
    void handle(tmux::Event &e)
    {
        if (e.type == tmux::Event::Notify && e.note == tmux::Note::SessionChanged)
            sessionChanged = true;
        if (e.type == tmux::Event::Exit)
            exited = true;
        if (e.type == tmux::Event::Reply && e.kind == tmux::Client::KListPanes)
            listPanes = e.lines;
        if (e.type == tmux::Event::Reply && e.kind == tmux::Client::KListWindows)
            listWindows = e.lines;
        if (e.type == tmux::Event::Notify && e.note == tmux::Note::LayoutChange) {
            tmux::Layout l, v;
            if (tmux::parseLayout(e.text, l) == tmux::LayoutError::None) {
                std::vector<const tmux::LayoutCell *> leaves, vis;
                tmux::layoutLeaves(l.root, leaves);
                if (tmux::parseLayout(e.text2, v) == tmux::LayoutError::None)
                    tmux::layoutLeaves(v.root, vis);
                for (const tmux::LayoutCell *c : leaves) {
                    std::pair<int, int> sz { int(c->sx), int(c->sy) };
                    for (const tmux::LayoutCell *z : vis)
                        if (z->pane == c->pane)
                            sz = { int(z->sx), int(z->sy) };
                    sizes[c->pane] = sz;
                    auto it = views.find(c->pane);
                    if (autoLayout && it != views.end()) {
                        it->second->resize(sz.second, sz.first);
                        client.ackPaneSize(c->pane, sz.first, sz.second);
                    }
                }
            }
        }
        if (e.type == tmux::Event::Log)
            std::fprintf(stderr, "  log: %s\n", e.text.c_str());
        events.push_back(e);
    }
    void sendLine(const char *cmd, uint16_t kind = tmux::Client::KIgnore)
    {
        // Test lines are written already quoted: split on spaces is not needed — send verbatim
        // through a one-command CommandLine.
        tmux::CommandLine l;
        l.cmd(cmd);
        client.send(l, kind);
    }
    void attachCommands(int cols, int rows)
    {
        tmux::CommandLine a, b, c, d;
        a.cmd("refresh-client").flag("-C").flag(std::to_string(cols) + "x" + std::to_string(rows));
        b.cmd("refresh-client").flag("-f").flag("pause-after=5");
        c.cmd("list-windows").flag("-F").arg("#{window_id} #{window_index} #{window_active} #{window_raw_flags} #{window_layout} #{window_visible_layout} #{window_name}");
        d.cmd("list-panes").flag("-s").flag("-F").arg("#{pane_id} #{window_id} #{pane_active} #{pane_width} #{pane_height} #{pane_current_path}");
        client.send(a, tmux::Client::KIgnore);
        client.send(b, tmux::Client::KIgnore);
        client.send(c, tmux::Client::KListWindows);
        client.send(d, tmux::Client::KListPanes);
    }
    void openFromList()
    {
        for (const std::string &line : listPanes) {
            std::istringstream ss(line);
            std::string id, win, active;
            int cols = 0, rows = 0;
            ss >> id >> win >> active >> cols >> rows;
            open(std::atoi(id.c_str() + 1), rows, cols);
        }
    }
    View &open(int pane, int rows, int cols)
    {
        auto v = std::make_unique<View>(pane, rows, cols, sb);
        v->fd = client.openPane(pane);
        client.ackPaneSize(pane, cols, rows);
        View &ref = *v;
        views[pane] = std::move(v);
        return ref;
    }
};

std::string readAll(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int runFake(const std::string &scratch)
{
    const std::string dir = TMUX_TESTS_DIR;
    const std::string log = scratch + "/fake-tmux-client-test.log";
    ::unlink(log.c_str());
    ::setenv("FAKE_TMUX_SCENARIO", (dir + "/scenario-basic.py").c_str(), 1);
    ::setenv("FAKE_TMUX_LOG", log.c_str(), 1);

    Harness h;
    h.client.setTee(true);
    h.client.setHistoryLimit(h.sb);
    std::string err;
    check(h.client.start(dir + "/fake-tmux.py", { "-C", "new-session", "-A", "-s", "work", "-x", "80", "-y", "24" }, &err),
          "spawn the fake tmux " + err);
    check(h.pumpUntil([&] { return h.sessionChanged; }, 5000), "%session-changed arrives");
    h.attachCommands(80, 24);
    check(h.pumpUntil([&] { return !h.listPanes.empty(); }, 5000), "list-panes answers");
    check(h.listWindows.size() == 2, "two windows listed");
    h.openFromList();
    check(h.views.size() == 4, "four panes opened");
    check(h.pumpUntil([&] { return h.client.seedsCompleted() == 4 && h.views[1]->count("after-seed") == 1; }, 5000),
          "all four panes seeded, %1's output after the seed group arrives");
    h.pump(100);

    View &p1 = *h.views[1];
    check(p1.fed.rfind("\x1b" "c", 0) == 0, "pane 1: the very first bytes fed are the seed (RIS): nothing leaked before it");
    check(p1.count("probe-gap") == 1, "output between the probe and the capture group shows once (from the capture)");
    check(p1.count("raced") == 1, "output just before the capture group's %begin shows once (from the capture)");
    check(p1.findRow("raced") < p1.findRow("after-seed"), "output after the group follows the seeded screen");
    check(p1.screen.sbCount() >= 30 && p1.findRow("hist-00") >= 0 && p1.findRow("hist-00") < p1.screen.sbCount(),
          "pane 1: the 30 history lines are in the scrollback");
    check(p1.findRow("hist-29") + 1 == p1.findRow("screen-1-top"), "history runs straight into the screen");
    {
        const int r = p1.findRow("red bold");
        const TermScreen::Line l = p1.screen.line(r);
        check(r >= 0 && l.cells && l.cells[0].pen.bold && VTERM_COLOR_IS_INDEXED(&l.cells[0].pen.fg)
                  && l.cells[0].pen.fg.indexed.idx == 1 && !l.cells[9].pen.bold,
              "SGR from capture-pane -e survives the seed (bold red, then plain)");
    }
    {
        View &p3 = *h.views[3];
        check(p3.screen.isAltScreen(), "pane 3 (vim-like) is seeded onto the alternate screen");
        check(p3.screenRow(0) == "ALT-TOP" && p3.screenRow(2) == "ALT-3", "pane 3: alternate screen rows in place");
        const VTermPos c = p3.cursor();
        check(c.row == 2 && c.col == 5, "pane 3: cursor where tmux had it");
        vterm_input_write(p3.vt, "\x1b[?1049l", 8);
        check(p3.screenRow(0) == "normal-under-alt" && p3.findRow("h3-old") >= 0 && p3.findRow("h3-old") < p3.screen.sbCount(),
              "pane 3: leaving the alternate screen shows the saved normal screen, history above it");
    }
    {
        View &p2 = *h.views[2];
        check(p2.screenRow(0) == "two: caf\xc3\xa9 back\\slash", "pane 2: UTF-8 and a backslash survive capture -C");
    }

    // step 1: tricky %output
    h.sendLine("display-message -p 'step 1'", tmux::Client::KUser);
    check(h.pumpUntil([&] { return h.views[2]->count("end") >= 1; }, 3000), "step 1 output arrives");
    {
        View &p2 = *h.views[2];
        const int r = p2.findRow("C:\\Users");
        const std::string line = r >= 0 ? p2.row(r) : "";
        check(line.find("C:\\Users\\x") == 0, "backslashes decoded");
        check(line.find("caf\xc3\xa9 \xf0\x9f\x9a\x80 red end") != std::string::npos,
              "UTF-8 cut across %output lines and an escape cut after ESC[ reassemble: '" + line + "'");
        const size_t at = line.find("red");
        int col = 0;
        for (size_t i = 0; i < at; ++i)
            col += (static_cast<unsigned char>(line[i]) & 0xc0) != 0x80;
        const TermScreen::Line l = p2.screen.line(r);
        // the rocket is a wide glyph: one extra cell
        check(l.cells && VTERM_COLOR_IS_INDEXED(&l.cells[col + 1].pen.fg) && l.cells[col + 1].pen.fg.indexed.idx == 196,
              "the split SGR applied (red 196)");
    }

    // step 2: new pane 5 appears; its output is held until it is opened, whole.
    h.sendLine("display-message -p 'step 2'", tmux::Client::KUser);
    h.pump(300);
    {
        check(h.sizes.count(5) && h.sizes[5] == std::make_pair(39, 12), "pane 5's size comes from the new layout");
        View &p5 = h.open(5, h.sizes[5].second, h.sizes[5].first);
        check(h.pumpUntil([&] { return p5.count("new pane five") == 1; }, 3000),
              "a pane created after attach: output held until opened, then delivered whole");
        check(p5.fed.rfind("\x1b" "c", 0) != 0, "a new pane is not seeded");
        h.client.releasePane(3);
        h.views.erase(3);
    }

    // step 4: %pause → continue + snapshot → reseed
    h.sendLine("display-message -p 'step 4'", tmux::Client::KUser);
    check(h.pumpUntil([&] { return h.client.reseedsCompleted() == 1; }, 3000), "a %pause is answered by one reseed");
    h.pump(100);
    {
        View &p1b = *h.views[1];
        // pane 1 was re-laid out by step 2 (now 39 wide, 11 high): resize our parser like the GUI.
        const int mark = p1b.findRow("-- output skipped");
        check(mark >= 0 && mark < p1b.screen.sbCount(), "the reseed marker row went into the scrollback");
        check(p1b.screenRow(0) == "after-pause-1" && p1b.screenRow(1) == "after-pause-2", "the fresh screen is shown");
        check(p1b.findRow("after-seed") >= 0 && p1b.findRow("after-seed") < mark, "older output stays above the marker");
        const std::string log = readAll(scratch + "/fake-tmux-client-test.log");
        check(log.find("refresh-client -A '%1:continue' ; display-message -p -t '%1'") != std::string::npos,
              "continue and snapshot went out as ONE group");
    }

    // resize fence: refresh-client -C 100x30 → %layout-change for every window → %1 prints after.
    {
        View &p1c = *h.views[1];
        h.autoLayout = false; // play the GUI by hand: late, then stale, then right
        tmux::CommandLine l;
        l.cmd("refresh-client").flag("-C").flag("100x30");
        h.client.send(l, tmux::Client::KIgnore);
        h.pump(400);
        check(p1c.count("after-resize") == 0, "output after a size change is held (fenced) until the grid is applied");
        // The new layout of @1 at 100x30: {[%1,%5],%2} → %1 is 49x14.
        h.client.ackPaneSize(1, 40, 10); // a stale size does not lift it
        h.pump(200);
        check(p1c.count("after-resize") == 0, "a stale ack does not lift the fence");
        p1c.resize(14, 49);
        h.client.ackPaneSize(1, 49, 14);
        check(h.pumpUntil([&] { return p1c.count("after-resize") == 1; }, 2000), "the matching ack releases it");
    }

    // detach
    h.client.detach();
    check(h.pumpUntil([&] { return h.exited; }, 3000), "detach-client ends the client (%exit, EOF)");
    h.client.stop(2000);
    const std::string sent = readAll(log);
    check(sent.find("detach-client") != std::string::npos, "the fake received detach-client");
    if (g_failures)
        std::fprintf(stderr, "pane 1:\n%s", h.views[1]->dump().c_str());
    return g_failures ? 1 : 0;
}

// Against a REAL tmux on a private socket (-S, never the user's server; killed at the end).
int runReal(const std::string &tmuxBin, const std::string &scratch)
{
    if (::access(tmuxBin.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "SKIP: no tmux at %s\n", tmuxBin.c_str());
        return 77;
    }
    ::mkdir(scratch.c_str(), 0700);
    const std::string sock = scratch + "/real-client-test.sock";
    const std::string wrapper = scratch + "/tmux-wrapper.sh";
    {
        std::ofstream w(wrapper);
        w << "#!/bin/sh\nexec env -i PATH=/usr/bin:/bin HOME=/ TERM=xterm-256color " << tmuxBin << " -S '" << sock
          << "' -f /dev/null \"$@\"\n";
    }
    ::chmod(wrapper.c_str(), 0700);
    auto sh = [&](const std::string &args) {
        const std::string cmd = wrapper + " " + args + " >/dev/null 2>&1";
        return std::system(cmd.c_str());
    };
    sh("kill-server");
    // A session with a split and some known content; panes run /bin/sh with a fixed prompt.
    sh("start-server \\; set -g default-shell /bin/sh \\; set -g default-command \"env -i PS1='$ ' TERM=tmux-256color /bin/sh\" "
       "\\; set -g history-limit 5000 \\; new-session -d -s real -n main -x 80 -y 24");
    sh("split-window -h -t real:0");
    sh("send-keys -t real:0.0 \"seq 1 60; printf 'tab\\\\there back\\\\\\\\slash \\\\033[1mbold\\\\033[0m caf\\\\303\\\\251\\\\n'\" Enter");
    sh("send-keys -t real:0.1 \"printf 'right pane\\\\n'\" Enter");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    Harness h;
    h.client.setHistoryLimit(h.sb);
    std::string err;
    check(h.client.start(wrapper, { "-C", "attach-session", "-t", "real" }, &err), "spawn real tmux -C " + err);
    check(h.pumpUntil([&] { return h.sessionChanged; }, 5000), "real: %session-changed");
    h.attachCommands(80, 24);
    check(h.pumpUntil([&] { return !h.listPanes.empty(); }, 5000), "real: list-panes answers");
    check(h.listWindows.size() == 1, "real: one window");
    if (!h.listWindows.empty()) {
        std::istringstream ss(h.listWindows[0]);
        std::string id, idx, active, flags, layout;
        ss >> id >> idx >> active >> flags >> layout;
        tmux::Layout l;
        check(tmux::parseLayout(layout, l) == tmux::LayoutError::None && l.root.type == tmux::LayoutCell::LeftRight
                  && l.root.children.size() == 2,
              "real: the window layout parses (checksum ok) as a left-right split: " + layout);
    }
    h.openFromList();
    check(h.views.size() == 2, "real: two panes");
    check(h.pumpUntil([&] { return h.client.seedsCompleted() == 2; }, 5000), "real: both panes seeded");
    h.pump(300);
    if (h.views.size() == 2) {
        View &left = *h.views.begin()->second;
        View &right = *std::next(h.views.begin())->second;
        check(left.findRow("1") >= 0 && left.count("60") >= 1, "real: seq output is back (history + screen)");
        const int r = left.findRow("slash bold"); // the printf's OUTPUT (the echoed command reads "\\033[1m")
        const std::string line = r >= 0 ? left.row(r) : "";
        check(line.rfind("tab", 0) == 0 && line.find("     here back\\slash bold caf\xc3\xa9") != std::string::npos,
              "real: TAB, backslash, SGR and UTF-8 round-trip through capture-pane -C: '" + line + "'");
        if (r >= 0) {
            const TermScreen::Line l = left.screen.line(r);
            const size_t at = line.find("bold");
            check(l.cells && l.cells[at].pen.bold && !l.cells[at + 5].pen.bold, "real: bold attribute restored");
        }
        check(right.count("right pane") >= 1, "real: the second pane is seeded too");
        // Live output after attach.
        sh("send-keys -t real:0.1 \"printf 'live-%s\\\\n' after\" Enter");
        check(h.pumpUntil([&] { return right.count("live-after") >= 1; }, 3000), "real: live %output reaches the pane");
        // A split from outside: %layout-change, then the new pane's output (unseeded, complete).
        sh("split-window -v -t real:0.1");
        check(h.pumpUntil([&] {
                  for (const tmux::Event &e : h.events)
                      if (e.type == tmux::Event::Notify && e.note == tmux::Note::LayoutChange) {
                          tmux::Layout l;
                          if (tmux::parseLayout(e.text, l) == tmux::LayoutError::None) {
                              std::vector<const tmux::LayoutCell *> leaves;
                              tmux::layoutLeaves(l.root, leaves);
                              if (leaves.size() == 3)
                                  return true;
                          }
                      }
                  return false;
              }, 3000),
              "real: an outside split arrives as a %layout-change with three panes");
    }
    h.client.detach();
    check(h.pumpUntil([&] { return h.exited; }, 3000), "real: detach-client ends the client");
    h.client.stop(2000);
    sh("kill-server");
    ::unlink(wrapper.c_str());
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    // argv: <scratch dir> | --real <tmux> <scratch dir>
    if (argc >= 4 && std::string(argv[1]) == "--real")
        return runReal(argv[2], argv[3]);
    const std::string scratch = argc >= 2 ? argv[1] : ".";
    ::mkdir(scratch.c_str(), 0700);
    return runFake(scratch);
}
