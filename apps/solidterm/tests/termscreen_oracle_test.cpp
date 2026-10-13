// Conformance oracle for TermScreen (native/termscreen.h): the previous SolidTerm path — libvterm's
// VTermScreen plus EXACTLY the scrollback push/pop/limit logic TerminalView had (cbSbPushLine /
// cbSbPopLine before st-buffer) — stays compiled here as the reference model. A corpus of escape
// sequences, resizes and a seeded random stream are fed to both; after EVERY step the whole screen
// (chars, width, attributes, colours), the cursor, the line info and the scrollback (every line,
// every cell) must be identical, and so must what the view derives from the callbacks (scroll
// anchor offset, alt-screen flag, cursor visibility).
//
// No Qt. Usage: test-solidterm-termscreen-oracle [random-steps-per-seed]
#include "../native/scrollanchor.h"
#include "../native/scrollbackcells.h"
#include "../native/termscreen.h"

#include <vterm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

// ── reference model: the pre-st-buffer TerminalView data path ────────────────────────────────────
struct OldModel {
    VTerm *vt = nullptr;
    VTermScreen *screen = nullptr;
    std::deque<std::vector<VTermScreenCell>> sb; // QVector<SbLine> with takeFirst/append semantics
    int limit = 8000;
    int offset = 0;
    bool altScreen = false;
    bool cursorVisible = true;
    VTermPos cursor { 0, 0 };
};

int oldPush(int cols, const VTermScreenCell *cells, void *user)
{
    auto *m = static_cast<OldModel *>(user);
    const bool full = !m->sb.empty() && int(m->sb.size()) >= m->limit;
    std::vector<VTermScreenCell> line;
    if (full) {
        line = std::move(m->sb.front());
        m->sb.pop_front();
    }
    line.assign(cells, cells + cols);
    m->sb.push_back(std::move(line));
    if (int(m->sb.size()) > m->limit)
        m->sb.pop_front();
    m->offset = offsetAfterPush(m->offset, int(m->sb.size()));
    return 1;
}

int oldPop(int cols, VTermScreenCell *cells, void *user)
{
    auto *m = static_cast<OldModel *>(user);
    if (m->sb.empty())
        return 0;
    const std::vector<VTermScreenCell> line = std::move(m->sb.back());
    m->sb.pop_back();
    m->offset = clampOffset(m->offset, int(m->sb.size()));
    const int n = std::min(cols, int(line.size()));
    std::copy(line.begin(), line.begin() + n, cells);
    VTermColor fg, bg;
    vterm_state_get_default_colors(vterm_obtain_state(m->vt), &fg, &bg);
    fillBlankCells(cells, n, cols, fg, bg);
    return 1;
}

int oldMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *m = static_cast<OldModel *>(user);
    m->cursor = pos;
    m->cursorVisible = visible != 0;
    return 1;
}

int oldTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *m = static_cast<OldModel *>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE)
        m->cursorVisible = val->boolean;
    else if (prop == VTERM_PROP_ALTSCREEN)
        m->altScreen = val->boolean;
    return 1;
}

const VTermScreenCallbacks kOldCallbacks = {
    /*damage*/ nullptr, /*moverect*/ nullptr, /*movecursor*/ oldMoveCursor, /*settermprop*/ oldTermProp,
    /*bell*/ nullptr, /*resize*/ nullptr, /*sb_pushline*/ oldPush, /*sb_popline*/ oldPop, /*sb_clear*/ nullptr,
};

// ── new path: TermScreen + the same view-side bookkeeping TerminalView does in its hooks ─────────
struct NewModel {
    VTerm *vt = nullptr;
    TermScreen screen;
    int offset = 0;
    bool altScreen = false;
    bool cursorVisible = true;
    VTermPos cursor { 0, 0 };
};

int newMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *m = static_cast<NewModel *>(user);
    m->cursor = pos;
    m->cursorVisible = visible != 0;
    return 1;
}

int newTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *m = static_cast<NewModel *>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE)
        m->cursorVisible = val->boolean;
    else if (prop == VTERM_PROP_ALTSCREEN)
        m->altScreen = val->boolean;
    return 1;
}

void newPushed(int count, void *user)
{
    auto *m = static_cast<NewModel *>(user);
    m->offset = offsetAfterPush(m->offset, count);
}

void newPopped(int count, void *user)
{
    auto *m = static_cast<NewModel *>(user);
    m->offset = clampOffset(m->offset, count);
}

const TermScreenHooks kNewHooks = { newMoveCursor, newTermProp, nullptr, nullptr, newPushed, newPopped };

void setDefaultColors(VTermState *state)
{
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 0xd4, 0xda, 0xe3);
    vterm_color_rgb(&bg, 0x16, 0x1a, 0x21);
    vterm_state_set_default_colors(state, &fg, &bg);
}

// Same order as TerminalView::ensureSession (before and after st-buffer).
void initOld(OldModel &m, int rows, int cols, int limit)
{
    m.limit = limit;
    m.vt = vterm_new(rows, cols);
    vterm_set_utf8(m.vt, 1);
    m.screen = vterm_obtain_screen(m.vt);
    vterm_screen_set_callbacks(m.screen, &kOldCallbacks, &m);
    vterm_screen_enable_altscreen(m.screen, 1);
    vterm_screen_set_damage_merge(m.screen, VTERM_DAMAGE_SCROLL);
    setDefaultColors(vterm_obtain_state(m.vt));
    vterm_screen_reset(m.screen, 1);
}

void initNew(NewModel &m, int rows, int cols, int limit)
{
    m.vt = TermScreen::newVTerm(rows, cols);
    vterm_set_utf8(m.vt, 1);
    m.screen.setScrollbackLimit(limit);
    m.screen.attach(m.vt, &kNewHooks, &m);
    setDefaultColors(m.screen.state());
    m.screen.reset(true);
}

// ── comparison ─────────────────────────────────────────────────────────────────────────────────
bool colorEq(const VTermColor &a, const VTermColor &b)
{
    if (a.type != b.type)
        return false;
    if (VTERM_COLOR_IS_INDEXED(&a))
        return a.indexed.idx == b.indexed.idx;
    return a.rgb.red == b.rgb.red && a.rgb.green == b.rgb.green && a.rgb.blue == b.rgb.blue;
}

// The right half of a wide glyph (chars[0] == -1) is compared by its marker only: libvterm writes
// just chars[0] there, so its chars[1..] are leftovers of whatever occupied that memory before —
// which depends on buffer recycling, not on the terminal's content. TermScreen clears them.
bool cellEq(const VTermScreenCell &a, const VTermScreenCell &b, std::string &why)
{
    const int maxChars = (a.chars[0] == kWideContinuation && b.chars[0] == kWideContinuation) ? 1 : VTERM_MAX_CHARS_PER_CELL;
    for (int i = 0; i < maxChars; ++i) {
        if (a.chars[i] != b.chars[i]) {
            why = "chars[" + std::to_string(i) + "] " + std::to_string(a.chars[i]) + " vs " + std::to_string(b.chars[i]);
            return false;
        }
        if (!a.chars[i])
            break;
    }
#define CMP(f)                                                                                     \
    if (a.f != b.f) {                                                                              \
        why = #f " " + std::to_string(int(a.f)) + " vs " + std::to_string(int(b.f));             \
        return false;                                                                              \
    }
    CMP(width) CMP(attrs.bold) CMP(attrs.underline) CMP(attrs.italic) CMP(attrs.blink) CMP(attrs.reverse)
    CMP(attrs.conceal) CMP(attrs.strike) CMP(attrs.font) CMP(attrs.dwl) CMP(attrs.dhl) CMP(attrs.small)
    CMP(attrs.baseline)
#undef CMP
    if (!colorEq(a.fg, b.fg)) {
        why = "fg";
        return false;
    }
    if (!colorEq(a.bg, b.bg)) {
        why = "bg";
        return false;
    }
    return true;
}

struct Stats {
    long steps = 0, cellsCompared = 0, sbLinesCompared = 0, maxSb = 0, resizes = 0;
};

bool compare(OldModel &o, NewModel &n, const std::string &step, Stats &st)
{
    int orows, ocols;
    vterm_get_size(o.vt, &orows, &ocols);
    if (orows != n.screen.rows() || ocols != n.screen.cols()) {
        std::fprintf(stderr, "[%s] size %dx%d vs %dx%d\n", step.c_str(), ocols, orows, n.screen.cols(), n.screen.rows());
        return false;
    }
    VTermPos oc, nc;
    vterm_state_get_cursorpos(vterm_obtain_state(o.vt), &oc);
    vterm_state_get_cursorpos(n.screen.state(), &nc);
    if (oc.row != nc.row || oc.col != nc.col) {
        std::fprintf(stderr, "[%s] state cursor (%d,%d) vs (%d,%d)\n", step.c_str(), oc.row, oc.col, nc.row, nc.col);
        return false;
    }
    if (o.cursor.row != n.cursor.row || o.cursor.col != n.cursor.col || o.cursorVisible != n.cursorVisible) {
        std::fprintf(stderr, "[%s] movecursor view (%d,%d,%d) vs (%d,%d,%d)\n", step.c_str(), o.cursor.row,
                     o.cursor.col, o.cursorVisible, n.cursor.row, n.cursor.col, n.cursorVisible);
        return false;
    }
    if (o.altScreen != n.altScreen || o.altScreen != n.screen.isAltScreen()) {
        std::fprintf(stderr, "[%s] altscreen %d vs %d/%d\n", step.c_str(), o.altScreen, n.altScreen, n.screen.isAltScreen());
        return false;
    }
    std::string why;
    for (int r = 0; r < orows; ++r) {
        const VTermLineInfo *oi = vterm_state_get_lineinfo(vterm_obtain_state(o.vt), r);
        const VTermLineInfo *ni = vterm_state_get_lineinfo(n.screen.state(), r);
        if (oi->doublewidth != ni->doublewidth || oi->doubleheight != ni->doubleheight || oi->continuation != ni->continuation) {
            std::fprintf(stderr, "[%s] lineinfo row %d\n", step.c_str(), r);
            return false;
        }
        const TermScreen::Line line = n.screen.liveLine(r);
        for (int c = 0; c < ocols; ++c) {
            VTermScreenCell a {}, b {};
            vterm_screen_get_cell(o.screen, VTermPos { r, c }, &a);
            n.screen.cell(line, c, &b);
            ++st.cellsCompared;
            if (!cellEq(a, b, why)) {
                std::fprintf(stderr, "[%s] screen cell (%d,%d): %s\n", step.c_str(), r, c, why.c_str());
                return false;
            }
        }
    }
    if (int(o.sb.size()) != n.screen.sbCount()) {
        std::fprintf(stderr, "[%s] scrollback size %zu vs %d\n", step.c_str(), o.sb.size(), n.screen.sbCount());
        return false;
    }
    st.maxSb = std::max<long>(st.maxSb, long(o.sb.size()));
    for (int i = 0; i < int(o.sb.size()); ++i) {
        const auto &ol = o.sb[size_t(i)];
        const TermScreen::Line nl = n.screen.line(i);
        if (int(ol.size()) != nl.cols) {
            std::fprintf(stderr, "[%s] scrollback line %d: %zu vs %d cols\n", step.c_str(), i, ol.size(), nl.cols);
            return false;
        }
        for (int c = 0; c < nl.cols; ++c) {
            VTermScreenCell b {};
            n.screen.cell(nl, c, &b);
            if (!cellEq(ol[size_t(c)], b, why)) {
                std::fprintf(stderr, "[%s] scrollback line %d col %d: %s\n", step.c_str(), i, c, why.c_str());
                return false;
            }
        }
        ++st.sbLinesCompared;
    }
    if (o.offset != n.offset) {
        std::fprintf(stderr, "[%s] anchor offset %d vs %d\n", step.c_str(), o.offset, n.offset);
        return false;
    }
    ++st.steps;
    return true;
}

struct Pair {
    OldModel o;
    NewModel n;
    Stats st;
    bool ok = true;

    Pair(int rows, int cols, int limit)
    {
        initOld(o, rows, cols, limit);
        initNew(n, rows, cols, limit);
        ok = compare(o, n, "init", st);
    }
    ~Pair()
    {
        vterm_free(o.vt);
        vterm_free(n.vt);
    }
    bool feed(const std::string &name, const std::string &bytes)
    {
        if (!ok)
            return false;
        if (std::getenv("ORACLE_TRACE")) { // the step about to run, escaped (debugging a crash)
            std::string esc;
            for (unsigned char ch : bytes)
                esc += ch == 0x1b ? std::string("\\e") : ch < 0x20 ? "\\x" + std::to_string(ch) : std::string(1, char(ch));
            std::fprintf(stderr, "step %ld %s: %s\n", st.steps, name.c_str(), esc.c_str());
        }
        vterm_input_write(o.vt, bytes.data(), bytes.size());
        vterm_screen_flush_damage(o.screen);
        n.screen.beginBatch();
        vterm_input_write(n.vt, bytes.data(), bytes.size());
        n.screen.endBatch();
        return ok = compare(o, n, name, st);
    }
    bool resize(int rows, int cols)
    {
        if (!ok)
            return false;
        vterm_set_size(o.vt, rows, cols);
        vterm_screen_flush_damage(o.screen);
        vterm_set_size(n.vt, rows, cols);
        ++st.resizes;
        return ok = compare(o, n, "resize " + std::to_string(cols) + "x" + std::to_string(rows), st);
    }
    // The user scrolled back (TerminalView sets m_scrollOffset directly) — exercises the anchor.
    void scrollBack(int lines)
    {
        o.offset = clampOffset(o.offset + lines, int(o.sb.size()));
        n.offset = clampOffset(n.offset + lines, n.screen.sbCount());
    }
};

std::string lines(int from, int count, const char *fmt = "line %d\r\n")
{
    std::string s;
    char buf[64];
    for (int i = from; i < from + count; ++i) {
        std::snprintf(buf, sizeof buf, fmt, i);
        s += buf;
    }
    return s;
}

// ── fixed corpus: one scenario per feature named in the brief ─────────────────────────────────────
bool corpus(int limit)
{
    Pair p(12, 40, limit);
    const std::string E = "\x1b";
    bool ok = true;
    ok = ok && p.feed("plain lines + scroll into scrollback", lines(0, 30));
    p.scrollBack(5);
    ok = ok && p.feed("output while scrolled back (anchor)", lines(30, 7));
    ok = ok && p.feed("cursor movement CUP/CUU/CUD/CUF/CUB/CHA/VPA/HVP",
                      E + "[5;10HX" + E + "[2AY" + E + "[3BZ" + E + "[4Cq" + E + "[6Dr" + E + "[20Gs" + E + "[3dt" + E + "[7;7fu");
    ok = ok && p.feed("erase EL 0/1/2", E + "[4;20H" + E + "[K" + E + "[5;20H" + E + "[1K" + E + "[6;1H" + E + "[2K");
    ok = ok && p.feed("erase ED 0/1", E + "[8;15H" + E + "[J" + E + "[3;15H" + E + "[1J");
    ok = ok && p.feed("refill", E + "[H" + lines(100, 12, "row %d abcdefghijklmnopqrstuvwxyz\r\n"));
    ok = ok && p.feed("ED 2 + ED 3 (scrollback clear stays a no-op)", E + "[2J" + E + "[3J" + E + "[Hafter");
    ok = ok && p.feed("refill 2", E + "[H" + lines(200, 12, "r%d 0123456789012345678901234567890\r\n"));
    ok = ok && p.feed("ICH/DCH/ECH", E + "[3;5H" + E + "[4@" + E + "[4;5H" + E + "[3P" + E + "[5;5H" + E + "[6X");
    ok = ok && p.feed("IL/DL", E + "[4;1H" + E + "[2L" + E + "[7;1H" + E + "[3M");
    ok = ok && p.feed("DECSTBM region + scroll inside", E + "[3;8r" + E + "[8;1H" + lines(300, 9) + E + "[3;1H" + E + "M" + E + "M");
    ok = ok && p.feed("SU/SD in region", E + "[2S" + E + "[3T");
    ok = ok && p.feed("region at top (pushes to scrollback)", E + "[1;6r" + E + "[6;1H" + lines(400, 5) + E + "[r");
    ok = ok && p.feed("SU/SD full screen", E + "[3S" + E + "[2T" + E + "[50S");
    ok = ok && p.feed("wide chars + wrap at edge", E + "[H" + "日本語テキスト漢字" + E + "[1;39H" + "日本" + E + "[2;40H中");
    ok = ok && p.feed("emoji + combining marks", E + "[5;1H🚀🎉✅ e\xcc\x81 a\xcc\x83\xcc\x88 n\xcc\x83");
    ok = ok && p.feed("overwrite half of a wide char", E + "[1;2Hx" + E + "[1;5H" + E + "[1P");
    ok = ok && p.feed("SGR attrs", E + "[6;1H" + E + "[1mB" + E + "[3mI" + E + "[4mU" + E + "[4:3mC" + E + "[5mK" + E + "[7mR" + E + "[8mH" + E + "[9mS" + E + "[11mF" + E + "[73mP" + E + "[0m.");
    ok = ok && p.feed("SGR colours 16/256/rgb fg+bg", E + "[7;1H" + E + "[31;42mA" + E + "[91;102mB" + E + "[38;5;200;48;5;17mC" + E + "[38;2;1;2;3;48;2;250;251;252mD" + E + "[39;49mE" + E + "[0m");
    ok = ok && p.feed("erase with coloured pen", E + "[44m" + E + "[8;5H" + E + "[K" + E + "[9;1H" + E + "[2X" + E + "[0m");
    ok = ok && p.feed("scroll with coloured pen", E + "[45m" + E + "[12;1H\n\n" + E + "[0m");
    ok = ok && p.feed("DECAWM off/on", E + "[?7l" + E + "[10;35H0123456789abc" + E + "[?7h" + E + "[11;35H0123456789abc");
    ok = ok && p.feed("tabs, BS, CR, NEL, IND, RI at top", E + "[1;1H\ta\tb\x08\x08c\r" + E + "E" + E + "D" + E + "[1;1H" + E + "M" + E + "M");
    ok = ok && p.feed("DECSCA protected + DECSED/DECSEL", E + "[3;1H" + E + "[1\"qPROT" + E + "[0\"q free" + E + "[3;1H" + E + "[?2K" + E + "[?0J");
    ok = ok && p.feed("DECDWL/DECDHL", E + "[4;1H" + E + "#6wide line" + E + "[5;1H" + E + "#3top" + E + "[6;1H" + E + "#4bot" + E + "[7;1H" + E + "#5");
    ok = ok && p.feed("DECALN", E + "#8");
    ok = ok && p.feed("DECLRMM + DECSLRM + scroll inside margins", E + "[?69h" + E + "[5;30s" + E + "[2;10r" + E + "[10;5H\n\n\n" + E + "[3;5H" + E + "[2L" + E + "[2M" + E + "[3@" + E + "[2P" + E + "[s" + E + "[?69l" + E + "[r");
    ok = ok && p.feed("DECSCNM reverse on", E + "[?5h" + lines(500, 15));
    ok = ok && p.feed("DECSCNM reverse off", E + "[?5l" + lines(520, 3));
    ok = ok && p.feed("alt screen 1049 enter + draw + scroll", E + "[?1049h" + E + "[H" + lines(600, 20) + E + "[44mfoo");
    ok = ok && p.resize(10, 30);  // shrink while in alt screen
    ok = ok && p.resize(16, 50);  // grow while in alt screen
    ok = ok && p.feed("alt screen exit", E + "[0m" + E + "[?1049l");
    ok = ok && p.feed("alt screen 47 (no clear) + 1047", E + "[?47hX" + E + "[?47l" + E + "[?1047hY" + E + "[?1047l");
    ok = ok && p.feed("lines at new size", lines(700, 25));
    ok = ok && p.resize(16, 20);  // narrower: rows truncate
    ok = ok && p.feed("lines narrow", lines(800, 30, "narrow %d xxxxxxxxxxxxxxxxxxxx\r\n"));
    ok = ok && p.feed("wide-char lines into scrollback", lines(850, 20, "日本語 wide %d 中文\r\n"));
    ok = ok && p.resize(16, 60);  // wider (nothing comes back)
    ok = ok && p.resize(30, 60);  // taller: sb_popline round trip of narrow + wide-char lines
    ok = ok && p.feed("DECSCNM on, lines into scrollback", E + "[?5h" + E + "[7m" + lines(870, 40) + E + "[0m");
    ok = ok && p.resize(10, 60);  // shorter while reversed: pushes bake DECSCNM in
    ok = ok && p.resize(36, 60);  // taller while reversed: pops un-bake it
    ok = ok && p.feed("DECSCNM off", E + "[?5l");
    ok = ok && p.resize(8, 60);   // shorter: pushes to scrollback
    ok = ok && p.feed("wide char then shrink below it", E + "[8;1H日本語日本語" + E + "[8;58H日");
    ok = ok && p.resize(8, 9);
    ok = ok && p.resize(40, 70);
    ok = ok && p.feed("cursor hidden / shown", E + "[?25l" + "x" + E + "[?25h");
    ok = ok && p.feed("RIS", E + "c" + lines(900, 10));
    ok = ok && p.resize(12, 40);
    if (!ok)
        std::fprintf(stderr, "corpus(limit=%d) FAILED after %ld steps\n", limit, p.st.steps);
    else
        std::printf("corpus(limit=%d): %ld steps, %ld cells, %ld scrollback lines compared, max sb %ld, %ld resizes\n",
                    limit, p.st.steps, p.st.cellsCompared, p.st.sbLinesCompared, p.st.maxSb, p.st.resizes);
    return ok;
}

// ── seeded random stream ──────────────────────────────────────────────────────────────────────────
std::string randomChunk(std::mt19937 &rng)
{
    auto r = [&](int n) { return int(rng() % unsigned(n)); };
    const std::string E = "\x1b";
    static const char *const kText[] = {
        "hello", "日本", "🚀", "e\xcc\x81", "a\xcc\x83\xcc\x88", "─│┼", "ação", "x", "  ", "中文字", "\t", "\x08",
    };
    std::string s;
    switch (r(30)) {
    case 0: case 1: case 2: case 3: case 4: case 5: {
        const int n = 1 + r(30);
        for (int i = 0; i < n; ++i)
            s += char('!' + r(90));
        break;
    }
    case 6: case 7: s += "\r\n"; break;
    case 8: s += "\n"; break;
    case 9: s += kText[r(12)]; break;
    case 10: s += E + "[" + std::to_string(1 + r(45)) + ";" + std::to_string(1 + r(75)) + "H"; break;
    case 11: s += E + "[" + std::to_string(r(4)) + "J"; break;
    case 12: s += E + "[" + std::to_string(r(3)) + "K"; break;
    case 13: s += E + "[" + std::to_string(1 + r(6)) + "@MLPXST"[r(7)]; break;
    case 14: { // DECSTBM (sometimes reset)
        if (r(3) == 0) {
            s += E + "[r";
        } else {
            const int a = 1 + r(20), b = a + 1 + r(20);
            s += E + "[" + std::to_string(a) + ";" + std::to_string(b) + "r";
        }
        break;
    }
    case 15: { // SGR
        static const char *const kSgr[] = { "0", "1", "3", "4", "5", "7", "9", "22", "27", "31", "42", "93",
                                            "104", "38;5;123", "48;5;45", "38;2;10;200;30", "48;2;1;1;1", "39", "49", "4:3", "8" };
        s += E + "[" + kSgr[r(21)] + "m";
        break;
    }
    case 16: s += E + "M"; break;           // RI
    case 17: s += E + "D"; break;           // IND
    case 18: s += E + "E"; break;           // NEL
    case 19: s += E + (r(2) ? "[?7h" : "[?7l"); break;
    case 20: s += E + (r(2) ? "[?6h" : "[?6l"); break; // origin mode
    case 21: { // alt screen modes
        static const char *const kAlt[] = { "[?1049h", "[?1049l", "[?47h", "[?47l", "[?1047h", "[?1047l" };
        s += E + kAlt[r(6)];
        break;
    }
    case 22: s += E + (r(4) == 0 ? "[?5h" : "[?5l"); break; // DECSCNM
    case 23: s += E + "#" + "3456"[r(4)]; break;            // DECDHL/DECSWL/DECDWL
    case 24: s += E + (r(2) ? "[1\"q" : "[0\"q"); break;     // DECSCA
    case 25: s += E + "[?" + std::to_string(r(3)) + "Jdummy" + E + "[?" + std::to_string(r(3)) + "K"; break;
    case 26: s += E + (r(2) ? "[?69h" : "[?69l") + E + "[" + std::to_string(1 + r(10)) + ";" + std::to_string(11 + r(60)) + "s"; break;
    case 27: s += E + "[" + std::to_string(1 + r(5)) + "ABCDEFG"[r(7)]; break;
    case 28: s += E + (r(2) ? "7" : "8"); break; // DECSC/DECRC
    case 29: s += E + "[?25" + (r(2) ? "h" : "l"); break;
    }
    return s;
}

bool randomStream(unsigned seed, int steps, int limit)
{
    std::mt19937 rng(seed);
    Pair p(20 + int(rng() % 20), 30 + int(rng() % 60), limit);
    for (int i = 0; i < steps && p.ok; ++i) {
        const int pick = int(rng() % 400);
        if (pick == 0) {
            // libvterm 0.3.3 bug, shared by both paths (state.c, not the screen): on_resize clamps
            // the scroll region's bottom but not its top, so a region whose top is past the new
            // height makes the next linefeed memmove line info with a NEGATIVE size (crash). Reset
            // DECSTBM first so the random stream does not walk into it.
            p.feed("DECSTBM reset before resize", "\x1b[r");
            p.resize(3 + int(rng() % 45), 5 + int(rng() % 90));
            // Same family: a cursor saved (DECSC / 1049) at the old size is restored unclamped by
            // DECRC, and the next erase past the edge crashes libvterm's screen.c (TermScreen clamps;
            // the reference cannot). Re-save inside the new screen.
            p.feed("DECSC after resize", "\x1b" "7");
        } else if (pick == 1) {
            p.scrollBack(int(rng() % 20));
        } else if (pick == 2) {
            p.feed("RIS", "\x1b" "c");
        } else {
            std::string chunk;
            const int n = 1 + int(rng() % 8);
            for (int k = 0; k < n; ++k)
                chunk += randomChunk(rng);
            p.feed("random#" + std::to_string(i), chunk);
        }
    }
    if (!p.ok)
        std::fprintf(stderr, "random(seed=%u, limit=%d) FAILED at step %ld\n", seed, limit, p.st.steps);
    else
        std::printf("random(seed=%u, limit=%d): %ld steps, %ld cells, %ld scrollback lines compared, max sb %ld, %ld resizes\n",
                    seed, limit, p.st.steps, p.st.cellsCompared, p.st.sbLinesCompared, p.st.maxSb, p.st.resizes);
    return p.ok;
}

} // namespace

int main(int argc, char **argv)
{
    const int steps = argc > 1 ? std::atoi(argv[1]) : 6000;
    bool ok = true;
    // Default limit, a tiny limit (eviction + recycling on every push), and no scrollback at all.
    if (const char *file = std::getenv("ORACLE_FILE")) { // replay a file (e.g. the bench's mixed.txt)
        FILE *f = std::fopen(file, "rb");
        if (!f)
            return 2;
        std::mt19937 chunkRng { unsigned(steps) };
        Pair p(56, 105, 8000);
        std::vector<char> buf(65536);
        long chunks = 0;
        while (p.ok) {
            const size_t want = 1 + chunkRng() % buf.size(); // random chunk boundaries (pty reads vary)
            const size_t got = std::fread(buf.data(), 1, want, f);
            if (got == 0)
                break;
            // compare only every 64 chunks (a full compare per chunk would take hours on 100 MB)
            vterm_input_write(p.o.vt, buf.data(), got);
            vterm_screen_flush_damage(p.o.screen);
            p.n.screen.beginBatch();
            vterm_input_write(p.n.vt, buf.data(), got);
            p.n.screen.endBatch();
            if (++chunks % 64 == 0)
                p.ok = compare(p.o, p.n, "file chunk " + std::to_string(chunks), p.st);
        }
        std::fclose(f);
        p.ok = p.ok && compare(p.o, p.n, "file end", p.st);
        std::printf("file %s: %ld chunks, %ld compares, %s\n", file, chunks, p.st.steps, p.ok ? "ok" : "FAILED");
        return p.ok ? 0 : 1;
    }
    if (const char *only = std::getenv("ORACLE_ONLY")) { // "seed:limit" — rerun one stream (debugging)
        unsigned seed = 0;
        int limit = 0;
        std::sscanf(only, "%u:%d", &seed, &limit);
        return randomStream(seed, steps, limit) ? 0 : 1;
    }
    for (int limit : { 8000, 7, 0 })
        ok = corpus(limit) && ok;
    for (unsigned seed : { 1u, 2u, 3u, 42u, 2026u })
        for (int limit : { 8000, 25, 0 })
            ok = randomStream(seed, steps, limit) && ok;
    std::puts(ok ? "termscreen oracle ok" : "termscreen oracle FAILED");
    return ok ? 0 : 1;
}
