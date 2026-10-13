// tmux control-mode protocol library (apps/solidterm/native/tmux/, docs/solidterm-tmux-control-mode.md
// phase 0), without Qt and without tmux:
//
//  - golden transcripts (tests/tmux/*.txt): control-client output written from tmux 3.7c's source
//    behaviour and checked against real 3.7c output (record-transcript.sh). Each is fed to the parser
//    in many chunkings — 1 byte, 2, 3, 7, 64, 4096 and seeded random cuts — so lines, %output payloads,
//    UTF-8 sequences and escape sequences are split everywhere; the event log must be identical for
//    every chunking and equal to the reviewed <name>.expected. "##send <n> <line>" directives in a
//    transcript register a sent command line of n commands with the correlator, so the log also
//    shows which of OUR commands each flags-1 block answers and which group members were dropped;
//  - the decoder, the notification fields, guards, the command quoting, and layouts (nested splits,
//    three-way splits, a real 3.7c floating pane, bad checksums, bad geometry, print round trips).
//
// `tmux_protocol_test --print <transcript>` prints the event log (to review a new .expected).
#include "../native/tmux/layout.h"
#include "../native/tmux/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef TMUX_TRANSCRIPTS
#define TMUX_TRANSCRIPTS "apps/solidterm/tests/tmux"
#endif

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void checkStr(const std::string &got, const std::string &want, const std::string &what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n  got:  '%s'\n  want: '%s'\n", what.c_str(), got.c_str(), want.c_str());
        ++g_failures;
    }
}

// Bytes → a readable, unambiguous form: printable ASCII as is (backslash doubled), the rest \xHH.
std::string show(const char *p, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == '\\')
            out += "\\\\";
        else if (c >= 0x20 && c < 0x7f)
            out += char(c);
        else {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            out += b;
        }
    }
    return out;
}
std::string show(std::string_view s) { return show(s.data(), s.size()); }

// ─── the transcript harness ─────────────────────────────────────────────────────────────────────

struct Recorder {
    std::vector<std::string> log;
    tmux::CommandQueue queue;
    uint64_t nextTag = 1;
    std::vector<std::string> sent; // tag → line

    void onOutput(int pane, char *data, size_t len)
    {
        log.push_back("output %" + std::to_string(pane) + " " + show(data, len));
    }
    void onBlockBegin(const tmux::BlockGuard &g)
    {
        log.push_back("begin " + std::to_string(g.number) + " flags=" + std::to_string(g.flags));
    }
    void onBlockLine(std::string_view l) { log.push_back("  | " + show(l)); }
    void onBlockEnd(const tmux::BlockGuard &g, bool error)
    {
        std::string e = (error ? "error " : "end ") + std::to_string(g.number);
        if (g.flags != 1) {
            log.push_back(e + " (not ours: ignored)");
            return;
        }
        tmux::Pending done;
        std::vector<tmux::Pending> dropped;
        if (!queue.complete(error, done, dropped)) {
            log.push_back(e + " DESYNC (nothing pending)");
            return;
        }
        e += " answers #" + std::to_string(done.tag);
        log.push_back(e);
        for (const tmux::Pending &d : dropped)
            log.push_back("  dropped #" + std::to_string(d.tag) + " (group member after the error)");
    }
    void onNotification(const tmux::Notification &n)
    {
        std::string s = "note ";
        s += n.type == tmux::Note::Unknown ? "UNKNOWN(" + std::string(n.name) + ")" : tmux::noteName(n.type);
        if (n.session >= 0)
            s += " $" + std::to_string(n.session);
        if (n.window >= 0)
            s += " @" + std::to_string(n.window);
        if (n.pane >= 0)
            s += " %" + std::to_string(n.pane);
        if (!n.text.empty())
            s += " [" + show(n.text) + "]";
        if (!n.text2.empty())
            s += " [" + show(n.text2) + "]";
        if (!n.text3.empty())
            s += " [" + show(n.text3) + "]";
        log.push_back(s);
    }
    void onStray(std::string_view l) { log.push_back("stray " + show(l)); }

    void send(int commands, const std::string &line)
    {
        std::vector<tmux::Pending> p(static_cast<size_t>(commands));
        for (tmux::Pending &e : p)
            e.tag = nextTag++;
        queue.pushGroup(p.data(), p.size());
        log.push_back("sent " + std::to_string(commands) + "x #" + std::to_string(p.front().tag) + ".. : " + line);
    }
};

// Segments of protocol bytes and ##send directives, in order.
struct Step {
    bool send = false;
    int commands = 0;
    std::string text; // bytes to feed, or the sent line
};

bool loadTranscript(const std::string &path, std::vector<Step> &steps)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::string line;
    std::string pending;
    while (std::getline(in, line)) {
        if (line.rfind("##", 0) == 0) {
            if (line.rfind("##send ", 0) == 0) {
                if (!pending.empty())
                    steps.push_back({ false, 0, pending });
                pending.clear();
                const size_t sp = line.find(' ', 7);
                steps.push_back({ true, std::atoi(line.c_str() + 7), line.substr(sp + 1) });
            }
            continue; // "## comment"
        }
        pending += line;
        pending += '\n';
    }
    if (!pending.empty())
        steps.push_back({ false, 0, pending });
    return true;
}

// chunk 0 = random cuts seeded with `seed`.
std::vector<std::string> runTranscript(const std::vector<Step> &steps, size_t chunk, unsigned seed)
{
    Recorder rec;
    tmux::ControlParser<Recorder> parser(rec);
    std::mt19937 rng(seed);
    for (const Step &s : steps) {
        if (s.send) {
            rec.send(s.commands, s.text);
            continue;
        }
        std::string buf = s.text; // the parser decodes in place: feed a private copy
        size_t off = 0;
        while (off < buf.size()) {
            size_t n = chunk ? chunk : 1 + rng() % 97;
            if (n > buf.size() - off)
                n = buf.size() - off;
            parser.feed(buf.data() + off, n);
            off += n;
        }
    }
    if (parser.carried())
        rec.log.push_back("UNTERMINATED LINE (" + std::to_string(parser.carried()) + " bytes)");
    if (parser.inBlock())
        rec.log.push_back("OPEN BLOCK " + std::to_string(parser.openBlock().number));
    return rec.log;
}

std::string joined(const std::vector<std::string> &log)
{
    std::string out;
    for (const std::string &l : log) {
        out += l;
        out += '\n';
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testTranscript(const std::string &name)
{
    const std::string dir = TMUX_TRANSCRIPTS;
    std::vector<Step> steps;
    if (!loadTranscript(dir + "/" + name + ".txt", steps)) {
        check(false, "transcript " + name + " readable");
        return;
    }
    const std::string want = readFile(dir + "/" + name + ".expected");
    check(!want.empty(), name + ".expected readable");
    const size_t chunks[] = { 4096, 1, 2, 3, 7, 64, 0, 0, 0, 0 };
    unsigned seed = 1;
    for (const size_t c : chunks) {
        const std::string got = joined(runTranscript(steps, c, seed++));
        if (got != want) {
            std::fprintf(stderr, "FAIL: transcript %s, chunk %zu (0 = random seed %u): event log differs\n--- got\n%s--- want\n%s",
                         name.c_str(), c, seed - 1, got.c_str(), want.c_str());
            ++g_failures;
            return;
        }
    }
}

// ─── unit checks ────────────────────────────────────────────────────────────────────────────────

void testDecode()
{
    checkStr(tmux::decoded("plain"), "plain", "decode: no escapes");
    checkStr(tmux::decoded("a\\134b"), "a\\b", "decode: \\134 is a backslash");
    checkStr(tmux::decoded("\\033[1m\\015\\012"), "\x1b[1m\r\n", "decode: ESC CR LF");
    checkStr(tmux::decoded("x\\000y"), std::string("x\0y", 3), "decode: NUL");
    checkStr(tmux::decoded("cap\\\\ture"), "cap\\ture", "decode: capture-pane -C writes \\\\");
    checkStr(tmux::decoded("\\134\\134"), "\\\\", "decode: two encoded backslashes");
    checkStr(tmux::decoded("end\\"), "end\\", "decode: a lone trailing backslash is kept");
    checkStr(tmux::decoded("\\08x"), "\\08x", "decode: \\08 is not octal: kept");
    checkStr(tmux::decoded("\\03"), "\\03", "decode: a short escape at the end is kept");
    checkStr(tmux::decoded("\xc3\xa9\x7f\xff"), "\xc3\xa9\x7f\xff", "decode: 0x7f and 0x80+ pass as is");
    checkStr(tmux::decoded(""), "", "decode: empty");
}

void testGuardsAndIds()
{
    tmux::BlockGuard g;
    check(tmux::parseGuard("1791899855 308 1", g) && g.number == 308 && g.flags == 1 && g.time == 1791899855,
          "guard: three fields");
    check(!tmux::parseGuard("1791899855 308", g), "guard: two fields rejected");
    check(!tmux::parseGuard("17x 308 1", g), "guard: junk rejected");
    check(tmux::parseGuard("1 2 0 extra", g) && g.flags == 0, "guard: trailing fields tolerated");
    check(tmux::parseId("%12", '%') == 12 && tmux::parseId("@3", '@') == 3 && tmux::parseId("$0", '$') == 0, "ids");
    check(tmux::parseId("%", '%') == -1 && tmux::parseId("@3", '%') == -1 && tmux::parseId("%1x", '%') == -1, "bad ids");
}

void testNotifications()
{
    tmux::Notification n;
    tmux::parseNotification("%layout-change @1 41a3,80x24,0,0[80x12,0,0,3,80x11,0,13,4] 41a3,80x24,0,0[80x12,0,0,3,80x11,0,13,4] *", n);
    check(n.type == tmux::Note::LayoutChange && n.window == 1 && n.text3 == "*", "layout-change fields");
    tmux::parseNotification("%layout-change @0 b25d,80x24,0,0,0 b25d,80x24,0,0,0 ", n);
    check(n.type == tmux::Note::LayoutChange && n.text3.empty() && n.text2 == "b25d,80x24,0,0,0",
          "layout-change with empty flags");
    tmux::parseNotification("%window-renamed @2 ren x", n);
    check(n.type == tmux::Note::WindowRenamed && n.window == 2 && n.text == "ren x", "window-renamed keeps spaces");
    tmux::parseNotification("%session-renamed $4 my session", n);
    check(n.session == 4 && n.text == "my session", "session-renamed with id (the code)");
    tmux::parseNotification("%session-renamed plain", n);
    check(n.session == -1 && n.text == "plain", "session-renamed without id (the man page)");
    tmux::parseNotification("%client-session-changed /dev/pts/3 $1 other one", n);
    check(n.text == "/dev/pts/3" && n.session == 1 && n.text2 == "other one", "client-session-changed");
    tmux::parseNotification("%exit", n);
    check(n.type == tmux::Note::Exit && n.text.empty(), "exit without reason");
    tmux::parseNotification("%exit server exited unexpectedly", n);
    check(n.text == "server exited unexpectedly", "exit reason");
    tmux::parseNotification("%subscription-changed cmd $0 @1 1 %2 : vim x", n);
    check(n.type == tmux::Note::SubscriptionChanged && n.text == "cmd" && n.text2 == "vim x", "subscription-changed");
    tmux::parseNotification("%future-thing a b", n);
    check(n.type == tmux::Note::Unknown && n.name == "future-thing", "unknown notifications keep their name");
}

void testQuoting()
{
    checkStr(tmux::quoted("plain"), "'plain'", "quote: plain");
    checkStr(tmux::quoted("it's"), "'it'\"'\"'s'", "quote: a single quote (verified against 3.7c: it's)");
    checkStr(tmux::quoted("$HOME ~ \\n #{pane_id} ; x"), "'$HOME ~ \\n #{pane_id} ; x'", "quote: $ ~ \\ ; literal");
    checkStr(tmux::quoted(""), "''", "quote: empty");

    tmux::CommandLine c;
    c.cmd("display-message").flag("-p").pane("-t", 3).arg("#{pane_width} x");
    c.cmd("capture-pane").flag("-p").pane("-t", 3).flag("-e").flag("-C").number("-S", -2000).flagArg("-E", "-");
    checkStr(c.str(), "display-message -p -t '%3' '#{pane_width} x' ; capture-pane -p -t '%3' -e -C -S -2000 -E '-'",
             "command line: two commands, one group");
    check(c.ok() && c.commands() == 2, "command line: ok, 2 commands");

    tmux::CommandLine s;
    s.cmd("kill-session").session("-t", 0);
    checkStr(s.str(), "kill-session -t '$0'", "session targets are quoted ($0 would be a variable)");

    tmux::CommandLine bad;
    bad.cmd("rename-window").window("-t", 1).arg("two\nlines");
    check(!bad.ok(), "a newline in an argument makes the line unsendable");
    tmux::CommandLine nul;
    nul.cmd("rename-window").arg(std::string_view("a\0b", 3));
    check(!nul.ok(), "a NUL in an argument makes the line unsendable");
}

void testCorrelator()
{
    tmux::CommandQueue q;
    tmux::Pending a[3];
    for (int i = 0; i < 3; ++i)
        a[i].tag = uint64_t(i + 1);
    q.pushGroup(a, 3);
    tmux::Pending b;
    b.tag = 4;
    q.pushGroup(&b, 1);
    tmux::Pending done;
    std::vector<tmux::Pending> dropped;
    check(q.complete(false, done, dropped) && done.tag == 1 && dropped.empty(), "queue: first answers #1");
    check(q.complete(true, done, dropped) && done.tag == 2 && dropped.size() == 1 && dropped[0].tag == 3,
          "queue: an error in a group drops its remaining members");
    dropped.clear();
    check(q.complete(true, done, dropped) && done.tag == 4 && dropped.empty(),
          "queue: an error on the last member drops nothing of the next group");
    check(!q.complete(false, done, dropped), "queue: a block with nothing pending is a desync");
}

std::string layoutOk(const std::string &s, tmux::Layout &l)
{
    return tmux::layoutErrorName(tmux::parseLayout(s, l));
}

void testLayouts()
{
    tmux::Layout l;
    // Real tmux 3.7c output (recorded): one pane; two windows' worth of nested splits.
    checkStr(layoutOk("b260,80x24,0,0,3", l), "ok", "layout: single pane");
    check(l.root.type == tmux::LayoutCell::Pane && l.root.pane == 3 && l.root.sx == 80 && l.root.sy == 24, "single pane fields");
    checkStr(tmux::printLayout(l), "b260,80x24,0,0,3", "single pane round trip");

    const std::string nested = "d67e,80x24,0,0{40x24,0,0,0,39x24,41,0[39x12,41,0,1,39x11,41,13,2]}";
    checkStr(layoutOk(nested, l), "ok", "layout: {} with a nested []");
    check(l.root.type == tmux::LayoutCell::LeftRight && l.root.children.size() == 2, "nested: root is left-right of 2");
    check(l.root.children[1].type == tmux::LayoutCell::TopBottom && l.root.children[1].children.size() == 2,
          "nested: right child is top-bottom of 2");
    {
        std::vector<const tmux::LayoutCell *> leaves;
        tmux::layoutLeaves(l.root, leaves);
        check(leaves.size() == 3 && leaves[0]->pane == 0 && leaves[1]->pane == 1 && leaves[2]->pane == 2
                  && leaves[2]->yoff == 13 && leaves[2]->sy == 11 && leaves[1]->xoff == 41,
              "nested: leaves in order with geometry");
    }
    checkStr(tmux::printLayout(l), nested, "nested round trip");
    checkStr(layoutOk("41a3,80x24,0,0[80x12,0,0,3,80x11,0,13,4]", l), "ok", "layout: top-bottom (recorded)");
    checkStr(layoutOk("b25d,80x24,0,0,0", l), "ok", "layout: zoomed visible layout (recorded)");

    // Three-way split and deeper nesting, checksums computed by printLayout and verified by parse.
    {
        tmux::Layout t;
        t.root.type = tmux::LayoutCell::LeftRight;
        t.root.sx = 80; t.root.sy = 24;
        unsigned x = 0;
        const unsigned widths[3] = { 26, 26, 26 };
        for (int k = 0; k < 3; ++k) {
            tmux::LayoutCell c;
            c.sx = widths[k]; c.sy = 24; c.xoff = x; c.yoff = 0; c.pane = 10 + k;
            x += widths[k] + 1;
            t.root.children.push_back(c);
        }
        // split the middle one top/bottom, then its bottom left/right again
        tmux::LayoutCell &mid = t.root.children[1];
        mid.type = tmux::LayoutCell::TopBottom;
        mid.pane = -1;
        tmux::LayoutCell top { tmux::LayoutCell::Pane, 26, 12, 27, 0, 11, {} };
        tmux::LayoutCell bot { tmux::LayoutCell::LeftRight, 26, 11, 27, 13, -1, {} };
        bot.children.push_back({ tmux::LayoutCell::Pane, 13, 11, 27, 13, 20, {} });
        bot.children.push_back({ tmux::LayoutCell::Pane, 12, 11, 41, 13, 21, {} });
        mid.children = { top, bot };
        check(tmux::layoutCheck(t.root), "built three-way layout passes layout_check");
        const std::string s = tmux::printLayout(t);
        checkStr(s.substr(5), "80x24,0,0{26x24,0,0,10,26x24,27,0[26x12,27,0,11,26x11,27,13{13x11,27,13,20,12x11,41,13,21}],26x24,54,0,12}",
                 "three-way body");
        checkStr(layoutOk(s, l), "ok", "three-way parses back");
        checkStr(tmux::printLayout(l), s, "three-way round trip");
        std::vector<const tmux::LayoutCell *> leaves;
        tmux::layoutLeaves(l.root, leaves);
        check(leaves.size() == 5 && leaves[2]->pane == 20 && leaves[3]->pane == 21 && leaves[4]->pane == 12,
              "three-way leaves in order");
    }

    // tmux 3.7c floating pane (recorded): listed in <…> AND as an extra tiled child.
    const std::string floating =
        "d79a,80x24,0,0{40x24,0,0,0,39x24,41,0[39x12,41,0,1,39x11,41,13,2],40x6,4,2,7}<40x6,4,2,7>";
    checkStr(layoutOk(floating, l), "ok", "layout: real 3.7c floating pane");
    check(l.floating.size() == 1 && l.floating[0].pane == 7, "floating: pane 7 in <…>");
    {
        std::vector<const tmux::LayoutCell *> leaves;
        tmux::layoutLeaves(l.root, leaves);
        check(leaves.size() == 3 && leaves[0]->pane == 0 && leaves[2]->pane == 2,
              "floating: the tiled tree no longer holds pane 7");
    }

    // Errors.
    checkStr(layoutOk("d67f,80x24,0,0{40x24,0,0,0,39x24,41,0[39x12,41,0,1,39x11,41,13,2]}", l), "checksum mismatch",
             "layout: one-off checksum");
    checkStr(layoutOk("d67e,80x24,0,0{40x24,0,0,0,39x24,41,0[39x12,41,0,1,39x11,41,13,2]", l), "syntax error",
             "layout: unclosed brace");
    checkStr(layoutOk("zz7e,80x24,0,0,1", l), "syntax error", "layout: bad checksum digits");
    checkStr(layoutOk("80x24,0,0,1", l), "syntax error", "layout: no checksum");
    {
        // Geometry: widths that do not add up (checksum right, so only layout_check rejects it).
        const std::string body = "80x24,0,0{40x24,0,0,0,40x24,41,0,1}";
        char head[8];
        std::snprintf(head, sizeof head, "%04x,", tmux::layoutChecksum(body));
        checkStr(layoutOk(head + body, l), "inconsistent geometry", "layout: widths + border != parent");
    }
    // Checksum vectors: tmux's own strings (recorded) — the parse above already proves them; also
    // the known value of the empty body.
    check(tmux::layoutChecksum("") == 0, "checksum of nothing");
}

// Every layout string in the REAL recording (list-windows replies and %layout-change) parses: the
// checksum and geometry code agree with what tmux 3.7c actually computed.
void testRecordedLayouts()
{
    std::ifstream in(std::string(TMUX_TRANSCRIPTS) + "/real-3.7c-session.txt");
    std::string line;
    int seen = 0;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string w;
        while (words >> w) {
            if (w.size() > 6 && w[4] == ',' && w.find('x') != std::string::npos
                && w.find_first_not_of("0123456789abcdef") == 4) {
                tmux::Layout l;
                checkStr(tmux::layoutErrorName(tmux::parseLayout(w, l)), "ok", "recorded layout " + w);
                checkStr(tmux::printLayout(l), w, "recorded layout prints back identically: " + w);
                ++seen;
            }
        }
    }
    check(seen >= 10, "the recording holds layouts (" + std::to_string(seen) + ")");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--print") {
        std::vector<Step> steps;
        if (!loadTranscript(argv[2], steps))
            return 2;
        std::fputs(joined(runTranscript(steps, 4096, 1)).c_str(), stdout);
        return 0;
    }
    testDecode();
    testGuardsAndIds();
    testNotifications();
    testQuoting();
    testCorrelator();
    testLayouts();
    testRecordedLayouts();
    for (const char *t : { "attach", "output-tricky", "blocks", "notifications", "real-3.7c-session" })
        testTranscript(t);
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("tmux protocol: all checks passed");
    return 0;
}
