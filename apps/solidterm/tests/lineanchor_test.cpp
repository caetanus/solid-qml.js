// Text anchoring of SolidTerm's selection / search matches / hover link (lineanchor.h), without Qt:
// a position captured as a LINE NUMBER (TermScreen::lineBase() + absolute row) must keep naming the
// same text while output scrolls it up — below the scrollback limit, AT the limit (where the ring
// evicts its oldest line per push and the plain absolute row of the old scheme drifted: the owner's
// "seleção não rola com a tela"), across a resize that pops lines back and pushes them again, a lower
// limit and a cleared scrollback — and must report the text as gone once it is evicted.
// Drives a real TermScreen over libvterm, like scrollback_test.cpp.
#include "../native/lineanchor.h"
#include "../native/scrollanchor.h"
#include "../native/termscreen.h"

#include <vterm.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template <typename A, typename B>
void checkEq(const A &got, const B &want, const char *what)
{
    if (!(got == want)) {
        std::fprintf(stderr, "FAIL: %s: got %lld, want %lld\n", what, (long long)got, (long long)want);
        ++g_failures;
    }
}

void checkStr(const std::string &got, const std::string &want, const char *what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s: got '%s', want '%s'\n", what, got.c_str(), want.c_str());
        ++g_failures;
    }
}

struct Term {
    TermScreen screen;
    VTerm *vt = nullptr;
    Term(int rows, int cols, int limit)
    {
        vt = TermScreen::newVTerm(rows, cols);
        vterm_set_utf8(vt, 1);
        screen.attach(vt, nullptr, nullptr);
        screen.reset(true);
        screen.setScrollbackLimit(limit);
    }
    ~Term() { vterm_free(vt); }
    Term(const Term &) = delete;
    Term &operator=(const Term &) = delete;

    void write(const std::string &s) { vterm_input_write(vt, s.data(), s.size()); }
    void lines(const char *prefix, int from, int count)
    {
        for (int i = from; i < from + count; ++i)
            write(std::string(prefix) + std::to_string(i) + "\r\n");
    }
    std::string absRowText(int64_t absRow) const
    {
        if (absRow < 0)
            return "<evicted>";
        const TermScreen::Line l = screen.line(int(absRow));
        if (!l.cells)
            return "<none>";
        std::string out;
        for (int c = 0; c < l.cols; ++c) {
            const uint32_t ch = l.cells[c].chars[0];
            out += (ch && ch < 0x80) ? char(ch) : ' ';
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }
    std::string lineText(LineNo line) const { return absRowText(absRowOfLine(screen.lineBase(), line)); }
    // The line number of the last row (scrollback or live) reading exactly `text`.
    LineNo find(const std::string &text) const
    {
        for (int64_t r = int64_t(screen.sbCount()) + screen.rows() - 1; r >= 0; --r)
            if (absRowText(r) == text)
                return lineOfAbsRow(screen.lineBase(), int(r));
        return -1;
    }
    LineNo liveTop() const { return viewTopLine(screen.lineBase(), screen.sbCount(), 0); }
};

// The pure helpers on their own.
void pureMapping()
{
    checkEq(lineOfAbsRow(100, 5), 105, "lineOfAbsRow");
    checkEq(absRowOfLine(100, 105), 5, "absRowOfLine");
    checkEq(absRowOfLine(100, 99), -1, "absRowOfLine of an evicted line is negative");
    checkEq(viewTopLine(100, 20, 0), 120, "live view top = base + sbCount");
    checkEq(viewTopLine(100, 20, 7), 113, "scrolled-back view top");
    checkEq(viewRowOfLine(113, 115), 2, "viewRowOfLine");

    LinePoint from { 10, 4 };
    const LinePoint to { 12, 3 };
    check(clipEvicted(from, to, 10) == Eviction::Kept && from == LinePoint { 10, 4 }, "clip: nothing evicted");
    check(clipEvicted(from, to, 11) == Eviction::Clipped && from == LinePoint { 11, 0 }, "clip: start evicted → oldest line, col 0");
    LinePoint from2 { 10, 4 };
    check(clipEvicted(from2, to, 13) == Eviction::Gone, "clip: wholly evicted → gone");
    check(lineBefore({ 3, 9 }, { 4, 0 }) && lineBefore({ 4, 1 }, { 4, 2 }) && !lineBefore({ 4, 2 }, { 4, 2 }), "lineBefore");
}

// Below the limit: the line number follows its text, and its live view row moves up one per push.
void belowLimit()
{
    Term t(5, 20, 1000);
    t.lines("a", 0, 10);
    t.write("MARK\r\n");
    t.lines("b", 0, 2);
    const LineNo mark = t.find("MARK");
    check(mark >= 0, "below: marker found");
    const int64_t row0 = viewRowOfLine(t.liveTop(), mark);
    const uint64_t pushed0 = t.screen.pushedTotal();
    t.lines("c", 0, 2); // stays on screen: 2 pushes
    checkStr(t.lineText(mark), "MARK", "below: same text after 2 pushes");
    checkEq(viewRowOfLine(t.liveTop(), mark), row0 - int64_t(t.screen.pushedTotal() - pushed0), "below: moved up one row per push");
    checkEq(t.screen.lineBase(), uint64_t(0), "below: nothing evicted yet");
}

// AT the limit: the ring evicts one line per push. The line number still names the marker; the
// old scheme (a stored absolute row) now reads another line — the reported bug, kept as a check
// that this test really exercises the drift.
void atLimit()
{
    Term t(5, 20, 20);
    t.lines("fill", 0, 60); // the ring is full
    t.write("MARK\r\n");
    check(t.screen.sbCount() == 20, "at limit: ring full");
    const LineNo mark = t.find("MARK");
    const int64_t oldAbsRow = absRowOfLine(t.screen.lineBase(), mark);
    const int64_t row0 = viewRowOfLine(t.liveTop(), mark);
    t.lines("more", 0, 7);
    checkStr(t.lineText(mark), "MARK", "at limit: line number still reads the marker");
    check(t.absRowText(oldAbsRow) != "MARK", "at limit: the old absolute-row anchor drifted (the bug)");
    checkEq(viewRowOfLine(t.liveTop(), mark), row0 - 7, "at limit: highlight row moved up by 7");

    // Scrolled back at the limit: the view's top line is pinned (scrollanchor.h) while it exists.
    int offset = 5;
    const LineNo top = viewTopLine(t.screen.lineBase(), t.screen.sbCount(), offset);
    const std::string topText = t.lineText(top);
    for (int i = 0; i < 3; ++i) {
        t.lines("x", i, 1);
        offset = offsetAfterPush(offset, t.screen.sbCount());
    }
    checkEq(viewTopLine(t.screen.lineBase(), t.screen.sbCount(), offset), top, "at limit: scrolled-back view keeps its top line");
    checkStr(t.lineText(top), topText, "at limit: scrolled-back top text unchanged");

    // A span from the marker to a later line: evicting the marker's line clips it, evicting both clears it.
    const LineNo later = t.find("more6");
    LinePoint from { mark, 1 };
    const LinePoint to { later, 2 };
    for (int guard = 0; guard < 100 && t.screen.lineBase() <= uint64_t(mark); ++guard)
        t.lines("evict", 0, 1);
    check(t.screen.lineBase() <= uint64_t(later), "at limit: later line still retained");
    check(clipEvicted(from, to, t.screen.lineBase()) == Eviction::Clipped, "at limit: marker evicted → clipped");
    checkEq(from.line, oldestLine(t.screen.lineBase()), "at limit: clipped to the oldest line");
    checkStr(t.lineText(mark), "<evicted>", "at limit: evicted marker reads as gone");
    t.lines("flood", 0, 40);
    LinePoint f2 { mark, 1 };
    check(clipEvicted(f2, to, t.screen.lineBase()) == Eviction::Gone, "at limit: whole span evicted → cleared");
}

// Resize: taller pops scrollback lines back to the screen, shorter pushes them again; no limit at all
// drops each pushed line at once — the numbers hold in every case.
void resizeAndLimits()
{
    Term t(5, 20, 50);
    t.lines("r", 0, 12);
    t.write("MARK\r\n");
    t.lines("s", 0, 6); // the marker is in the scrollback now
    const LineNo mark = t.find("MARK");
    check(absRowOfLine(t.screen.lineBase(), mark) < t.screen.sbCount(), "resize: marker in scrollback");
    vterm_set_size(t.vt, 15, 20); // taller: lines come back from the scrollback
    checkStr(t.lineText(mark), "MARK", "resize taller: same line number");
    vterm_set_size(t.vt, 4, 20); // shorter: pushed again
    checkStr(t.lineText(mark), "MARK", "resize shorter: same line number");

    // A lower limit trims the oldest lines: retained ones keep their numbers.
    const LineNo s5 = t.find("s5");
    t.screen.setScrollbackLimit(3);
    checkStr(t.lineText(s5), "s5", "lower limit: retained line keeps its number");
    checkStr(t.lineText(mark), "<evicted>", "lower limit: trimmed marker is gone");

    // Clearing the scrollback evicts it all; the live rows keep their numbers.
    check(t.screen.sbCount() > 0, "clear scrollback: setup has scrollback");
    const LineNo oldest = lineOfAbsRow(t.screen.lineBase(), 0);
    const LineNo live = t.liveTop() + 1;
    const std::string liveText = t.lineText(live);
    t.screen.clearScrollback();
    checkStr(t.lineText(live), liveText, "clear scrollback: live line keeps its number");
    checkStr(t.lineText(oldest), "<evicted>", "clear scrollback: scrollback lines evicted");

    // No scrollback at all (limit 0): every push is an eviction.
    Term z(5, 20, 0);
    z.lines("z", 0, 3);
    z.write("MARK\r\n");
    const LineNo zm = z.find("MARK");
    z.lines("y", 0, 2);
    checkStr(z.lineText(zm), "MARK", "limit 0: line number follows the text");
    z.lines("y", 2, 5);
    checkStr(z.lineText(zm), "<evicted>", "limit 0: scrolled off → gone");
}

// The alternate screen pushes nothing (libvterm only pushes from the primary screen): its line
// numbers are plain screen rows, and leaving it brings the primary text back at the same numbers.
void altScreen()
{
    Term t(5, 20, 50);
    t.lines("p", 0, 8);
    t.write("MARK");
    const LineNo mark = t.find("MARK");
    const uint64_t base = t.screen.lineBase();
    const int sb = t.screen.sbCount();
    t.write("\x1b[?1049h");
    t.lines("alt", 0, 20); // scrolls the alt screen
    checkEq(t.screen.lineBase(), base, "alt: no eviction");
    checkEq(t.screen.sbCount(), sb, "alt: no pushes");
    t.write("\x1b[?1049l");
    checkStr(t.lineText(mark), "MARK", "alt: primary text back at its number");
}

} // namespace

int main()
{
    pureMapping();
    belowLimit();
    atLimit();
    resizeAndLimits();
    altScreen();
    std::puts(g_failures ? "lineanchor FAILED" : "lineanchor ok");
    return g_failures ? 1 : 0;
}
