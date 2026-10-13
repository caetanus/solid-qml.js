#pragma once

#include <vterm.h>

#include <cstdint>
#include <type_traits>
#include <vector>

// SolidTerm's own screen buffer on top of libvterm's STATE layer (docs/solidterm-screen-buffer.md).
//
// libvterm's VTermScreen keeps the screen as one flat cell array and memmoves ALL of it (~208 KB at
// 105x56) for every line that scrolls, then copies the row leaving the top cell by cell into the
// scrollback callback: 87% of a `seq` flood. TermScreen implements VTermStateCallbacks itself: the
// screen is a table of row handles, a scroll moves handles (never cells), and a row leaving the top
// is MOVED into the scrollback ring, whose evicted oldest row comes back as the new bottom row.
//
// The callbacks are a port of libvterm 0.3.3's screen.c (MIT), so the cell contents, the scrollback
// and the cursor are identical to the VTermScreen path — tests/termscreen_oracle_test.cpp keeps that
// path as a reference model and compares both after every step of an escape-sequence corpus.
//
// Qt-free on purpose (tested without a GUI). Zero abstraction overhead: POD cells, one contiguous
// allocation per row, host notifications through one plain function-pointer table.

// Same layout as libvterm's internal ScreenPen.
struct TermPen {
    VTermColor fg, bg;
    unsigned int bold : 1;
    unsigned int underline : 2;
    unsigned int italic : 1;
    unsigned int blink : 1;
    unsigned int reverse : 1;
    unsigned int conceal : 1;
    unsigned int strike : 1;
    unsigned int font : 4;
    unsigned int small : 1;
    unsigned int baseline : 2;
    unsigned int protected_cell : 1;
    unsigned int dwl : 1;
    unsigned int dhl : 2;
};

// One cell. chars[] ends at the first 0 (or runs to VTERM_MAX_CHARS_PER_CELL); chars[0] == 0 is an
// erased cell; chars[0] == kWideContinuation is the right half of a double-width glyph (its width is
// derived from that marker, as in vterm_screen_get_cell).
struct TermCell {
    uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
    TermPen pen;
};
static_assert(std::is_trivially_copyable_v<TermCell>, "cells are moved with memmove/memcpy");

inline constexpr uint32_t kWideContinuation = uint32_t(-1);

// A handle to one row's cells (one allocation of `cap` cells, `cols` of them in use). Moving a row
// (scroll, scrollback push) moves this handle, never the cells.
struct TermRow {
    TermCell *cells = nullptr;
    uint32_t cap = 0;
    uint32_t cols = 0;
};

// Host notifications. Any pointer may be null.
struct TermScreenHooks {
    int (*movecursor)(VTermPos pos, VTermPos oldPos, int visible, void *user);
    int (*settermprop)(VTermProp prop, VTermValue *val, void *user);
    int (*bell)(void *user);
    void (*damage)(void *user);              // content changed OUTSIDE a batch (inside one: no call)
    void (*sbPushed)(int sbCount, void *user); // one line entered the scrollback (count afterwards)
    void (*sbPopped)(int sbCount, void *user); // one line left it again (resize taller)
};

class TermScreen {
public:
    // A VTerm built with a calloc/free allocator: resize() reallocates libvterm's line-info arrays
    // (the VTermStateFields contract) and must use the allocator libvterm frees them with.
    static VTerm *newVTerm(int rows, int cols);

    TermScreen() = default;
    ~TermScreen();
    TermScreen(const TermScreen &) = delete;
    TermScreen &operator=(const TermScreen &) = delete;

    // Allocates the primary + alternate screens at the VTerm's size and installs the state callbacks
    // (what vterm_obtain_screen + vterm_screen_enable_altscreen(1) did). Never call
    // vterm_obtain_screen on this VTerm: it would replace these callbacks.
    void attach(VTerm *vt, const TermScreenHooks *hooks, void *user);
    bool isAttached() const { return m_state != nullptr; }
    VTermState *state() const { return m_state; }

    // vterm_screen_reset: libvterm state reset (hard: cursor home + erase) and everything dirty.
    void reset(bool hard);

    // Inside a batch no damage hook is called (the host repaints once at the end). endBatch()
    // returns whether the batch changed any content.
    void beginBatch()
    {
        m_batch = true;
        m_batchDamaged = false;
    }
    bool endBatch()
    {
        m_batch = false;
        return m_batchDamaged;
    }

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    bool isAltScreen() const { return m_buf == &m_alt; }
    bool globalReverse() const { return m_globalReverse; }

    // ── reader API ───────────────────────────────────────────────────────────────────────────────
    // Absolute rows: [0, sbCount) is the scrollback (oldest first), [sbCount, sbCount + rows) the
    // live screen. A Line is a contiguous cell array read in place; cells past `cols` read as a
    // value-initialised cell (what TerminalView's readers always did for a narrower scrollback row).
    struct Line {
        const TermCell *cells = nullptr;
        int cols = 0;
        bool live = false;    // live rows apply DECSCNM at read time (scrollback rows have it baked in)
    };
    int sbCount() const { return m_sbCount; }
    // Lines ever pushed into the scrollback (also those dropped at once with no scrollback, or
    // evicted later). A scrolled-back view anchors against it across threads: the renderer and the
    // GUI each apply the pushes since the count the GUI last saw (docs/solidterm-parser-thread.md).
    uint64_t pushedTotal() const { return m_pushedTotal; }
    // Lines that have left the absolute row space for good: evicted from a full ring, dropped at
    // once with no scrollback, trimmed by a lower limit, or cleared. absRow 0 is line number
    // lineBase(), so lineBase() + absRow names the same text for as long as it is retained —
    // selection, search matches and the hover link anchor to it (lineanchor.h). Never decreases.
    uint64_t lineBase() const { return m_lineBase; }
    Line line(int absRow) const
    {
        if (absRow < 0)
            return {};
        if (absRow < m_sbCount) {
            const TermRow &r = sbAt(absRow);
            return { r.cells, int(r.cols), false };
        }
        const int live = absRow - m_sbCount;
        if (live >= m_rows)
            return {};
        return { (*m_buf)[size_t(live)].cells, m_cols, true };
    }
    Line liveLine(int row) const { return { (*m_buf)[size_t(row)].cells, m_cols, true }; }
    // Display width of the cell at `col` (2 when its right neighbour is a wide continuation).
    static int widthAt(const TermCell *cells, int cols, int col)
    {
        return (col < cols - 1 && cells[col + 1].chars[0] == kWideContinuation) ? 2 : 1;
    }
    bool reverseAt(const Line &l, int col) const { return l.cells[col].pen.reverse ^ (l.live && m_globalReverse); }
    // The libvterm view of one cell — exactly what vterm_screen_get_cell returned (or, for a
    // scrollback row, what sb_pushline stored). Past the line's end: a value-initialised cell.
    void cell(const Line &l, int col, VTermScreenCell *out) const;

    // ── scrollback ─────────────────────────────────────────────────────────────────────────────
    int scrollbackLimit() const { return m_sbLimit; }
    void setScrollbackLimit(int limit); // trims the oldest rows at once when lowered
    void clearScrollback();

    // ── damage ───────────────────────────────────────────────────────────────────────────────────
    // Per visible row: set when the row's content changed since clearDirty(). A frame-level flag
    // says whether any did. (The renderer still rebuilds every row; per-row reuse is proposal P4.)
    bool anyDirty() const { return m_anyDirty; }
    bool rowDirty(int row) const { return m_dirty[size_t(row)] != 0; }
    void clearDirty();

private:
    // VTermStateCallbacks (file-static table in termscreen.cpp).
    static int cbPutglyph(VTermGlyphInfo *info, VTermPos pos, void *user);
    static int cbMovecursor(VTermPos pos, VTermPos oldPos, int visible, void *user);
    static int cbScrollrect(VTermRect rect, int downward, int rightward, void *user);
    static int cbErase(VTermRect rect, int selective, void *user);
    static int cbSetpenattr(VTermAttr attr, VTermValue *val, void *user);
    static int cbSettermprop(VTermProp prop, VTermValue *val, void *user);
    static int cbBell(void *user);
    static int cbResize(int rows, int cols, VTermStateFields *fields, void *user);
    static int cbSetlineinfo(int row, const VTermLineInfo *newInfo, const VTermLineInfo *oldInfo, void *user);
    static int cbSbClear(void *user);
    friend struct TermScreenCallbackTable;

    using Table = std::vector<TermRow>;

    TermCell *cellPtr(int row, int col) const
    {
        if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
            return nullptr;
        return (*m_buf)[size_t(row)].cells + col;
    }
    void clearCell(TermCell &c) const { c.chars[0] = 0; c.pen = m_pen; }
    void eraseInternal(VTermRect rect, bool selective);
    void moveRect(VTermRect dest, VTermRect src);
    void resizeBuffer(Table &table, bool primary, int newRows, int newCols, bool active, VTermStateFields *fields);
    void allocTable(Table &table, int rows, int cols);
    static void freeTable(Table &table);

    // Scrollback ring.
    const TermRow &sbAt(int i) const
    {
        size_t idx = m_sbHead + size_t(i);
        if (idx >= m_sb.size())
            idx -= m_sb.size();
        return m_sb[idx];
    }
    TermRow pushRow(TermRow row);           // takes the row; returns a buffer to reuse (may be empty)
    void pushRowCopy(const TermCell *cells, int cols);
    bool popToScreenCells(int cols, VTermScreenCell *cells);
    void bakeReverse(TermRow &row) const;
    static void ensureCap(TermRow &row, int cols);

    void markRow(int row)
    {
        m_dirty[size_t(row)] = 1;
        markAny();
    }
    void markRows(int from, int to); // [from, to)
    void markAll() { markRows(0, m_rows); }
    void markAny()
    {
        m_anyDirty = true;
        if (m_batch)
            m_batchDamaged = true;
        else if (m_hooks && m_hooks->damage)
            m_hooks->damage(m_user);
    }

    VTerm *m_vt = nullptr;
    VTermState *m_state = nullptr;
    const TermScreenHooks *m_hooks = nullptr;
    void *m_user = nullptr;

    int m_rows = 0, m_cols = 0;
    Table m_primary, m_alt;
    Table *m_buf = &m_primary;    // the active screen
    TermPen m_pen {};             // zero until libvterm's first resetpen (as in screen.c)
    bool m_globalReverse = false; // DECSCNM
    bool m_reflow = false;        // VTermScreen's default; SolidTerm never enabled it
    bool m_batch = false;
    bool m_batchDamaged = false;

    std::vector<TermRow> m_sb;    // ring; grows by push_back until m_sbLimit, then rotates
    size_t m_sbHead = 0;          // oldest row
    int m_sbCount = 0;
    int m_sbLimit = 8000;
    uint64_t m_pushedTotal = 0;
    uint64_t m_lineBase = 0;      // see lineBase()

    std::vector<TermRow> m_scratchRows;        // handles leaving a scroll region
    std::vector<VTermScreenCell> m_sbCells;    // sb_popline scratch (resize only)

    std::vector<uint8_t> m_dirty;
    bool m_anyDirty = true;
};
