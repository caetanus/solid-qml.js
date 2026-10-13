#include "termscreen.h"

#include "scrollbackcells.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

static_assert(VTERM_MAX_CHARS_PER_CELL >= 2);

// Port of libvterm 0.3.3 src/screen.c (MIT, (c) Paul Evans) onto TermScreen's row table. Where the
// code mirrors a libvterm function, that function's name is in the comment so the two can be read
// side by side; deviations are called out.

namespace {

void *allocZeroed(size_t size, void *) { return std::calloc(1, size); }
void allocFree(void *ptr, void *) { std::free(ptr); }
VTermAllocatorFunctions kAllocator = { allocZeroed, allocFree };

TermCell *newCells(int cols) { return static_cast<TermCell *>(std::calloc(size_t(std::max(cols, 1)), sizeof(TermCell))); }

} // namespace

struct TermScreenCallbackTable {
    static const VTermStateCallbacks table;
};

const VTermStateCallbacks TermScreenCallbackTable::table = {
    /*putglyph*/ TermScreen::cbPutglyph,
    /*movecursor*/ TermScreen::cbMovecursor,
    /*scrollrect*/ TermScreen::cbScrollrect,
    /*moverect*/ nullptr, // never needed: scrollrect always handles the scroll itself
    /*erase*/ TermScreen::cbErase,
    /*initpen*/ nullptr,  // screen.c has none either
    /*setpenattr*/ TermScreen::cbSetpenattr,
    /*settermprop*/ TermScreen::cbSettermprop,
    /*bell*/ TermScreen::cbBell,
    /*resize*/ TermScreen::cbResize,
    /*setlineinfo*/ TermScreen::cbSetlineinfo,
    /*sb_clear*/ TermScreen::cbSbClear,
};

VTerm *TermScreen::newVTerm(int rows, int cols)
{
    VTermBuilder b {};
    b.rows = rows;
    b.cols = cols;
    b.allocator = &kAllocator;
    return vterm_build(&b);
}

TermScreen::~TermScreen()
{
    freeTable(m_primary);
    freeTable(m_alt);
    for (TermRow &r : m_sb)
        std::free(r.cells);
}

void TermScreen::allocTable(Table &table, int rows, int cols)
{
    // alloc_buffer: every cell cleared with the CURRENT pen (zero before libvterm's first resetpen).
    table.resize(size_t(rows));
    for (TermRow &r : table) {
        r.cells = newCells(cols);
        r.cap = uint32_t(cols);
        r.cols = uint32_t(cols);
        for (int c = 0; c < cols; ++c)
            clearCell(r.cells[c]);
    }
}

void TermScreen::freeTable(Table &table)
{
    for (TermRow &r : table)
        std::free(r.cells);
    table.clear();
}

void TermScreen::attach(VTerm *vt, const TermScreenHooks *hooks, void *user)
{
    m_vt = vt;
    m_state = vterm_obtain_state(vt);
    m_hooks = hooks;
    m_user = user;
    vterm_get_size(vt, &m_rows, &m_cols);
    allocTable(m_primary, m_rows, m_cols);
    m_buf = &m_primary;
    vterm_state_set_callbacks(m_state, &TermScreenCallbackTable::table, this);
    allocTable(m_alt, m_rows, m_cols); // vterm_screen_enable_altscreen(1)
    m_dirty.assign(size_t(m_rows), 1);
    m_anyDirty = true;
}

void TermScreen::reset(bool hard)
{
    vterm_state_reset(m_state, hard ? 1 : 0);
    markAll();
}

void TermScreen::markRows(int from, int to)
{
    from = std::max(from, 0);
    to = std::min(to, m_rows);
    if (from < to)
        std::memset(m_dirty.data() + from, 1, size_t(to - from));
    markAny();
}

void TermScreen::clearDirty()
{
    std::fill(m_dirty.begin(), m_dirty.end(), 0);
    m_anyDirty = false;
}

// vterm_screen_get_cell (+ what sb_pushline stored for a scrollback row).
void TermScreen::cell(const Line &l, int col, VTermScreenCell *out) const
{
    *out = VTermScreenCell {};
    if (!l.cells || col < 0 || col >= l.cols)
        return;
    const TermCell &c = l.cells[col];
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; ++i) {
        out->chars[i] = c.chars[i];
        if (!c.chars[i])
            break;
    }
    out->attrs.bold = c.pen.bold;
    out->attrs.underline = c.pen.underline;
    out->attrs.italic = c.pen.italic;
    out->attrs.blink = c.pen.blink;
    out->attrs.reverse = c.pen.reverse ^ (l.live && m_globalReverse);
    out->attrs.conceal = c.pen.conceal;
    out->attrs.strike = c.pen.strike;
    out->attrs.font = c.pen.font;
    out->attrs.small = c.pen.small;
    out->attrs.baseline = c.pen.baseline;
    out->attrs.dwl = c.pen.dwl;
    out->attrs.dhl = c.pen.dhl;
    out->fg = c.pen.fg;
    out->bg = c.pen.bg;
    out->width = char(widthAt(l.cells, l.cols, col));
}

// ─── scrollback ring ─────────────────────────────────────────────────────────────────────────────

void TermScreen::ensureCap(TermRow &row, int cols)
{
    if (row.cells && int(row.cap) >= cols)
        return;
    std::free(row.cells);
    row.cells = newCells(cols);
    row.cap = uint32_t(cols);
}

void TermScreen::bakeReverse(TermRow &row) const
{
    // sb_pushline stored vterm_screen_get_cell's view, which XORs DECSCNM in.
    if (!m_globalReverse)
        return;
    for (uint32_t c = 0; c < row.cols; ++c)
        row.cells[c].pen.reverse ^= 1;
}

// The row enters the scrollback as the newest line; returns a buffer the caller reuses for the row
// that replaces it on screen (the evicted oldest line once the ring is full), or an empty handle.
TermRow TermScreen::pushRow(TermRow row)
{
    bakeReverse(row);
    ++m_pushedTotal;
    TermRow out;
    if (m_sbLimit <= 0) {
        out = row; // no scrollback: the line is dropped at once, its buffer reused
    } else if (size_t(m_sbCount) < m_sb.size()) {
        size_t idx = m_sbHead + size_t(m_sbCount);
        if (idx >= m_sb.size())
            idx -= m_sb.size();
        m_sb[idx] = row;
        ++m_sbCount;
    } else if (m_sb.size() < size_t(m_sbLimit)) {
        if (m_sbHead != 0) { // linearise before growing (only after pops/limit changes)
            std::rotate(m_sb.begin(), m_sb.begin() + ptrdiff_t(m_sbHead), m_sb.end());
            m_sbHead = 0;
        }
        m_sb.push_back(row);
        ++m_sbCount;
    } else {
        out = m_sb[m_sbHead];
        m_sb[m_sbHead] = row;
        if (++m_sbHead == m_sb.size())
            m_sbHead = 0;
    }
    if (m_hooks && m_hooks->sbPushed)
        m_hooks->sbPushed(m_sbCount, m_user);
    return out;
}

void TermScreen::pushRowCopy(const TermCell *cells, int cols)
{
    TermRow row;
    row.cells = newCells(cols);
    row.cap = uint32_t(cols);
    row.cols = uint32_t(cols);
    std::memcpy(row.cells, cells, size_t(cols) * sizeof(TermCell));
    std::free(pushRow(row).cells);
}

// What TerminalView's cbSbPopLine handed to libvterm's sb_popline: the newest line's cells, cut or
// padded with real blanks (scrollbackcells.h) to `cols`.
bool TermScreen::popToScreenCells(int cols, VTermScreenCell *cells)
{
    if (m_sbCount == 0)
        return false;
    size_t idx = m_sbHead + size_t(m_sbCount - 1);
    if (idx >= m_sb.size())
        idx -= m_sb.size();
    TermRow row = m_sb[idx];
    m_sb[idx] = TermRow {};
    --m_sbCount;
    if (m_hooks && m_hooks->sbPopped)
        m_hooks->sbPopped(m_sbCount, m_user);
    const Line l { row.cells, int(row.cols), false };
    const int n = std::min(cols, int(row.cols));
    for (int c = 0; c < n; ++c)
        cell(l, c, &cells[c]);
    VTermColor fg, bg;
    vterm_state_get_default_colors(m_state, &fg, &bg);
    fillBlankCells(cells, n, cols, fg, bg);
    std::free(row.cells);
    return true;
}

void TermScreen::setScrollbackLimit(int limit)
{
    limit = std::max(0, limit);
    if (limit == m_sbLimit)
        return;
    // Linearise (oldest first), drop the oldest beyond the new limit.
    std::vector<TermRow> lin;
    lin.reserve(size_t(m_sbCount));
    for (int i = 0; i < m_sbCount; ++i)
        lin.push_back(sbAt(i));
    const int drop = std::max(0, m_sbCount - limit);
    for (int i = 0; i < drop; ++i)
        std::free(lin[size_t(i)].cells);
    lin.erase(lin.begin(), lin.begin() + drop);
    m_sb = std::move(lin);
    m_sbHead = 0;
    m_sbCount = int(m_sb.size());
    m_sbLimit = limit;
    if (drop > 0 && m_hooks && m_hooks->sbPopped)
        m_hooks->sbPopped(m_sbCount, m_user);
}

void TermScreen::clearScrollback()
{
    for (TermRow &r : m_sb)
        std::free(r.cells);
    m_sb.clear();
    m_sbHead = 0;
    m_sbCount = 0;
}

// ─── state callbacks ─────────────────────────────────────────────────────────────────────────────

// putglyph
int TermScreen::cbPutglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    TermCell *cell = s->cellPtr(pos.row, pos.col);
    if (!cell)
        return 0;
    int i;
    for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++) {
        cell->chars[i] = info->chars[i];
        cell->pen = s->m_pen;
    }
    if (i < VTERM_MAX_CHARS_PER_CELL)
        cell->chars[i] = 0;
    for (int col = 1; col < info->width; col++) {
        if (TermCell *c = s->cellPtr(pos.row, pos.col + col)) { // libvterm dereferences unchecked
            c->chars[0] = kWideContinuation;
            // Deviation: libvterm writes only chars[0], so the marker cell keeps whatever chars[1..]
            // that memory held before, and vterm_screen_get_cell copies those leftovers out (they
            // show up when an orphaned right half is drawn or selected). Ours is clean.
            c->chars[1] = 0;
        }
    }
    cell->pen.protected_cell = info->protected_cell;
    cell->pen.dwl = info->dwl;
    cell->pen.dhl = info->dhl;
    s->markRow(pos.row);
    return 1;
}

int TermScreen::cbMovecursor(VTermPos pos, VTermPos oldPos, int visible, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    if (s->m_hooks && s->m_hooks->movecursor)
        return s->m_hooks->movecursor(pos, oldPos, visible, s->m_user);
    return 0;
}

// Deviation from libvterm: every rect is clamped to the screen. libvterm 0.3.3's state can hand out
// rects past the edge after a resize (a DECRC restoring a cursor saved at the old size, then an
// erase): screen.c then dereferences getcell() == NULL and crashes. Valid rects are unchanged.
static bool clampRect(VTermRect &r, int rows, int cols)
{
    r.start_row = std::max(r.start_row, 0);
    r.start_col = std::max(r.start_col, 0);
    r.end_row = std::min(r.end_row, rows);
    r.end_col = std::min(r.end_col, cols);
    return r.start_row < r.end_row && r.start_col < r.end_col;
}

// n copies of `cell`: the first by value, the rest by doubling memcpy (a few large copies). A plain
// per-element loop / std::fill reloaded the template's bitfield word every cell (the compiler cannot
// prove the destination does not alias it) and stalled on store forwarding: ~4.6 ns per cell, 69% of
// a `seq` flood.
static void fillCells(TermCell *dst, int n, const TermCell &cell)
{
    if (n <= 0)
        return;
    std::memcpy(dst, &cell, sizeof(TermCell));
    int done = 1;
    while (done < n) {
        const int chunk = std::min(done, n - done);
        std::memcpy(dst + done, dst, size_t(chunk) * sizeof(TermCell));
        done += chunk;
    }
}

// erase_internal
void TermScreen::eraseInternal(VTermRect rect, bool selective)
{
    if (!clampRect(rect, m_rows, m_cols))
        return;
    for (int row = rect.start_row; row < m_rows && row < rect.end_row; row++) {
        const VTermLineInfo *info = vterm_state_get_lineinfo(m_state, row);
        // The erased cell: chars[0] = 0, pen = only fg/bg (things like reverse stay reset) + the
        // line's DWL/DHL. Written as one whole-cell copy (the hot path: every scrolled line erases
        // its incoming row) — chars[1..] become 0 instead of keeping leftovers; nothing reads past
        // chars[0] == 0, so the content is the same as libvterm's.
        TermCell blank {};
        blank.pen.fg = m_pen.fg;
        blank.pen.bg = m_pen.bg;
        blank.pen.dwl = info->doublewidth;
        blank.pen.dhl = info->doubleheight;
        TermCell *cells = (*m_buf)[size_t(row)].cells;
        if (!selective) {
            fillCells(cells + rect.start_col, rect.end_col - rect.start_col, blank);
            continue;
        }
        for (int col = rect.start_col; col < rect.end_col; col++) {
            if (!cells[col].pen.protected_cell)
                cells[col] = blank;
        }
    }
}

int TermScreen::cbErase(VTermRect rect, int selective, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    s->eraseInternal(rect, selective != 0);
    s->markRows(rect.start_row, rect.end_row);
    return 1;
}

// moverect_internal. Full-width vertical moves (every linefeed at the bottom, IL/DL, SU/SD, RI,
// DECSTBM regions) move ROW HANDLES; the rows that fall out of the region are reused for the rows
// coming in (then erased by the caller). Rows leaving the top of the primary screen are MOVED into
// the scrollback, and the buffer the ring hands back takes their place.
void TermScreen::moveRect(VTermRect dest, VTermRect src)
{
    const int downward = src.start_row - dest.start_row;
    const bool fullWidth = dest.start_col == 0 && dest.end_col == m_cols && src.start_col == 0 && src.end_col == m_cols;
    const bool pushTop = dest.start_row == 0 && dest.start_col == 0 && dest.end_col == m_cols && m_buf == &m_primary;

    if (fullWidth && downward != 0) {
        Table &t = *m_buf;
        // Region = [top, bottom) — dest ∪ src.
        const int top = std::min(dest.start_row, src.start_row);
        const int bottom = std::max(dest.end_row, src.end_row);
        const int n = std::abs(downward);
        const int keep = bottom - top - n;
        m_scratchRows.clear();
        if (downward > 0) {
            // Rows [top, top+n) leave; [top+n, bottom) move up to [top, bottom-n).
            for (int r = top; r < top + n; ++r) {
                TermRow row = t[size_t(r)];
                if (pushTop) { // top == 0 here
                    TermRow back = pushRow(row);
                    ensureCap(back, m_cols);
                    row = back;
                }
                row.cols = uint32_t(m_cols);
                m_scratchRows.push_back(row);
            }
            std::memmove(&t[size_t(top)], &t[size_t(top + n)], size_t(keep) * sizeof(TermRow));
            std::memcpy(&t[size_t(bottom - n)], m_scratchRows.data(), size_t(n) * sizeof(TermRow));
        } else {
            // Rows [bottom-n, bottom) leave; [top, bottom-n) move down to [top+n, bottom).
            m_scratchRows.assign(t.begin() + (bottom - n), t.begin() + bottom);
            std::memmove(&t[size_t(top + n)], &t[size_t(top)], size_t(keep) * sizeof(TermRow));
            std::memcpy(&t[size_t(top)], m_scratchRows.data(), size_t(n) * sizeof(TermRow));
        }
        return;
    }

    // Partial width (ICH/DCH, DECSLRM margins): cells move within/between rows, as in libvterm.
    if (pushTop) {
        for (int row = 0; row < src.start_row; row++)
            pushRowCopy((*m_buf)[size_t(row)].cells, m_cols);
    }
    const int cols = src.end_col - src.start_col;
    int initRow, testRow, incRow;
    if (downward < 0) {
        initRow = dest.end_row - 1;
        testRow = dest.start_row - 1;
        incRow = -1;
    } else {
        initRow = dest.start_row;
        testRow = dest.end_row;
        incRow = +1;
    }
    for (int row = initRow; row != testRow; row += incRow)
        std::memmove(cellPtr(row, dest.start_col), cellPtr(row + downward, src.start_col),
                     size_t(cols) * sizeof(TermCell));
}

// scrollrect (VTERM_DAMAGE_SCROLL path) = vterm_scroll_rect(moverect_internal, erase_internal).
int TermScreen::cbScrollrect(VTermRect rect, int downward, int rightward, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    if (!clampRect(rect, s->m_rows, s->m_cols))
        return 1;
    if (std::abs(downward) >= rect.end_row - rect.start_row || std::abs(rightward) >= rect.end_col - rect.start_col) {
        s->eraseInternal(rect, false); // scrolled more than the area: just erase the lot
        s->markRows(rect.start_row, rect.end_row);
        return 1;
    }
    VTermRect src, dest;
    if (rightward >= 0) {
        dest.start_col = rect.start_col;
        dest.end_col = rect.end_col - rightward;
        src.start_col = rect.start_col + rightward;
        src.end_col = rect.end_col;
    } else {
        const int leftward = -rightward;
        dest.start_col = rect.start_col + leftward;
        dest.end_col = rect.end_col;
        src.start_col = rect.start_col;
        src.end_col = rect.end_col - leftward;
    }
    if (downward >= 0) {
        dest.start_row = rect.start_row;
        dest.end_row = rect.end_row - downward;
        src.start_row = rect.start_row + downward;
        src.end_row = rect.end_row;
    } else {
        const int upward = -downward;
        dest.start_row = rect.start_row + upward;
        dest.end_row = rect.end_row;
        src.start_row = rect.start_row;
        src.end_row = rect.end_row - upward;
    }
    s->moveRect(dest, src);

    VTermRect erase = rect;
    if (downward > 0)
        erase.start_row = rect.end_row - downward;
    else if (downward < 0)
        erase.end_row = rect.start_row - downward;
    if (rightward > 0)
        erase.start_col = rect.end_col - rightward;
    else if (rightward < 0)
        erase.end_col = rect.start_col - rightward;
    s->eraseInternal(erase, false);
    s->markRows(rect.start_row, rect.end_row);
    return 1;
}

// setpenattr
int TermScreen::cbSetpenattr(VTermAttr attr, VTermValue *val, void *user)
{
    TermPen &pen = static_cast<TermScreen *>(user)->m_pen;
    switch (attr) {
    case VTERM_ATTR_BOLD: pen.bold = val->boolean; return 1;
    case VTERM_ATTR_UNDERLINE: pen.underline = unsigned(val->number); return 1;
    case VTERM_ATTR_ITALIC: pen.italic = val->boolean; return 1;
    case VTERM_ATTR_BLINK: pen.blink = val->boolean; return 1;
    case VTERM_ATTR_REVERSE: pen.reverse = val->boolean; return 1;
    case VTERM_ATTR_CONCEAL: pen.conceal = val->boolean; return 1;
    case VTERM_ATTR_STRIKE: pen.strike = val->boolean; return 1;
    case VTERM_ATTR_FONT: pen.font = unsigned(val->number); return 1;
    case VTERM_ATTR_FOREGROUND: pen.fg = val->color; return 1;
    case VTERM_ATTR_BACKGROUND: pen.bg = val->color; return 1;
    case VTERM_ATTR_SMALL: pen.small = val->boolean; return 1;
    case VTERM_ATTR_BASELINE: pen.baseline = unsigned(val->number); return 1;
    case VTERM_N_ATTRS: return 0;
    }
    return 0;
}

// settermprop
int TermScreen::cbSettermprop(VTermProp prop, VTermValue *val, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    switch (prop) {
    case VTERM_PROP_ALTSCREEN:
        s->m_buf = val->boolean ? &s->m_alt : &s->m_primary;
        s->markAll(); // screen.c damages on disable only (enable erases anyway); a full mark is equivalent
        break;
    case VTERM_PROP_REVERSE:
        s->m_globalReverse = val->boolean;
        s->markAll();
        break;
    default:
        break;
    }
    if (s->m_hooks && s->m_hooks->settermprop)
        return s->m_hooks->settermprop(prop, val, s->m_user);
    return 1;
}

int TermScreen::cbBell(void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    if (s->m_hooks && s->m_hooks->bell)
        return s->m_hooks->bell(s->m_user);
    return 0;
}

// setlineinfo
int TermScreen::cbSetlineinfo(int row, const VTermLineInfo *newInfo, const VTermLineInfo *oldInfo, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    if (row < 0 || row >= s->m_rows)
        return 1;
    if (newInfo->doublewidth != oldInfo->doublewidth || newInfo->doubleheight != oldInfo->doubleheight) {
        for (int col = 0; col < s->m_cols; col++) {
            TermCell *cell = s->cellPtr(row, col);
            cell->pen.dwl = newInfo->doublewidth;
            cell->pen.dhl = newInfo->doubleheight;
        }
        s->markRow(row);
        if (newInfo->doublewidth) {
            const VTermRect rect = { row, row + 1, s->m_cols / 2, s->m_cols };
            s->eraseInternal(rect, false);
        }
    }
    return 1;
}

// sb_clear: VTermScreen forwarded to a null callback → 0 (ED 3 left the scrollback alone). Kept.
int TermScreen::cbSbClear(void *)
{
    return 0;
}

// resize
int TermScreen::cbResize(int newRows, int newCols, VTermStateFields *fields, void *user)
{
    auto *s = static_cast<TermScreen *>(user);
    const bool altActive = s->m_buf == &s->m_alt;
    s->m_sbCells.resize(size_t(std::max(newCols, s->m_cols)));
    s->resizeBuffer(s->m_primary, true, newRows, newCols, !altActive, fields);
    s->resizeBuffer(s->m_alt, false, newRows, newCols, altActive, fields);
    s->m_buf = altActive ? &s->m_alt : &s->m_primary;
    s->m_rows = newRows;
    s->m_cols = newCols;
    s->m_dirty.assign(size_t(newRows), 1);
    s->markAny();
    return 1;
}

namespace {

// line_popcount
int linePopcount(const TermCell *row, int cols)
{
    int col = cols - 1;
    while (col >= 0 && row[col].chars[0] == 0)
        col--;
    return col + 1;
}

} // namespace

// resize_buffer, on a flat scratch copy of the new screen exactly like libvterm's (zeroed) new
// buffer, then split back into row handles. Resizing is rare; this path favours fidelity.
void TermScreen::resizeBuffer(Table &table, bool primary, int newRows, int newCols, bool active, VTermStateFields *fields)
{
    const int oldRows = m_rows;
    const int oldCols = m_cols;
    const int bufidx = primary ? 0 : 1;
    const bool REFLOW = m_reflow;

    VTermLineInfo *oldLineinfo = fields->lineinfos[bufidx];
    std::vector<TermCell> newBufferStore(size_t(newRows) * size_t(newCols), TermCell {});
    TermCell *newBuffer = newBufferStore.data();
    auto *newLineinfo = static_cast<VTermLineInfo *>(allocZeroed(sizeof(VTermLineInfo) * size_t(newRows), nullptr));
    auto oldCell = [&](int row, int col) -> const TermCell & { return table[size_t(row)].cells[col]; };

    int oldRow = oldRows - 1;
    int newRow = newRows - 1;

    const VTermPos oldCursor = fields->pos;
    VTermPos newCursor = { -1, -1 };

    int finalBlankRow = newRows;

    while (oldRow >= 0) {
        const int oldRowEnd = oldRow;
        while (REFLOW && oldLineinfo && oldRow >= 0 && oldLineinfo[oldRow].continuation)
            oldRow--;
        const int oldRowStart = oldRow;

        int width = 0;
        for (int row = oldRowStart; row <= oldRowEnd; row++) {
            if (REFLOW && row < (oldRows - 1) && oldLineinfo[row + 1].continuation)
                width += oldCols;
            else
                width += linePopcount(table[size_t(row)].cells, oldCols);
        }

        if (finalBlankRow == (newRow + 1) && width == 0)
            finalBlankRow = newRow;

        const int newHeight = REFLOW ? (width ? (width + newCols - 1) / newCols : 1) : 1;

        int newRowEnd = newRow;
        int newRowStart = newRow - newHeight + 1;

        oldRow = oldRowStart;
        int oldCol = 0;

        const int spareRows = newRows - finalBlankRow;

        if (newRowStart < 0 && spareRows >= 0
            && (!active || newCursor.row == -1 || (newCursor.row - newRowStart) < newRows)) {
            int downwards = -newRowStart;
            if (downwards > spareRows)
                downwards = spareRows;
            const int rowcount = newRows - downwards;
            std::memmove(&newBuffer[downwards * newCols], &newBuffer[0], size_t(rowcount) * size_t(newCols) * sizeof(TermCell));
            std::memmove(&newLineinfo[downwards], &newLineinfo[0], size_t(rowcount) * sizeof(newLineinfo[0]));
            newRow += downwards;
            newRowStart += downwards;
            newRowEnd += downwards;
            if (newCursor.row >= 0)
                newCursor.row += downwards;
            finalBlankRow += downwards;
        }

        if (newRowStart < 0) {
            if (oldRowStart <= oldCursor.row && oldCursor.row < oldRowEnd) {
                newCursor.row = 0;
                newCursor.col = oldCursor.col;
                if (newCursor.col >= newCols)
                    newCursor.col = newCols - 1;
            }
            break;
        }

        for (newRow = newRowStart, oldRow = oldRowStart; newRow <= newRowEnd; newRow++) {
            int count = width >= newCols ? newCols : width;
            width -= count;
            int newCol = 0;
            while (count) {
                newBuffer[newRow * newCols + newCol] = oldCell(oldRow, oldCol);
                if (oldCursor.row == oldRow && oldCursor.col == oldCol)
                    newCursor.row = newRow, newCursor.col = newCol;
                oldCol++;
                if (oldCol == oldCols) {
                    oldRow++;
                    if (!REFLOW) {
                        newCol++;
                        break;
                    }
                    oldCol = 0;
                }
                newCol++;
                count--;
            }
            if (oldCursor.row == oldRow && oldCursor.col >= oldCol) {
                newCursor.row = newRow, newCursor.col = (oldCursor.col - oldCol + newCol);
                if (newCursor.col >= newCols)
                    newCursor.col = newCols - 1;
            }
            while (newCol < newCols) {
                clearCell(newBuffer[newRow * newCols + newCol]);
                newCol++;
            }
            newLineinfo[newRow].continuation = (newRow > newRowStart);
        }

        oldRow = oldRowStart - 1;
        newRow = newRowStart - 1;
    }

    if (oldCursor.row <= oldRow) {
        newCursor.row = 0, newCursor.col = oldCursor.col;
        if (newCursor.col >= newCols)
            newCursor.col = newCols - 1;
    }

    // libvterm abort()s here ("screen_resize failed to update cursor position"); never seen, but a
    // terminal must not take the whole app down — clamp instead.
    if (active && (newCursor.row == -1 || newCursor.col == -1))
        newCursor = { std::max(newCursor.row, 0), std::max(newCursor.col, 0) };

    if (oldRow >= 0 && primary) {
        // Push spare lines to the scrollback. libvterm reads screen->buffer here — the ACTIVE
        // screen, so in the alternate screen it is the alternate screen's rows. Kept as is.
        for (int row = 0; row <= oldRow; row++)
            pushRowCopy((*m_buf)[size_t(row)].cells, oldCols);
        if (active)
            fields->pos.row -= (oldRow + 1);
    }
    if (newRow >= 0 && primary) {
        // Try to backfill rows by popping the scrollback (the sb_popline round trip).
        VTermScreenCell *sbBuffer = m_sbCells.data();
        while (newRow >= 0) {
            if (!popToScreenCells(oldCols, sbBuffer))
                break;
            VTermPos pos = { newRow, 0 };
            for (pos.col = 0; pos.col < oldCols && pos.col < newCols; pos.col += sbBuffer[pos.col].width) {
                const VTermScreenCell *src = &sbBuffer[pos.col];
                TermCell *dst = &newBuffer[pos.row * newCols + pos.col];
                for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
                    dst->chars[i] = src->chars[i];
                    if (!src->chars[i])
                        break;
                }
                dst->pen.bold = src->attrs.bold;
                dst->pen.underline = src->attrs.underline;
                dst->pen.italic = src->attrs.italic;
                dst->pen.blink = src->attrs.blink;
                dst->pen.reverse = src->attrs.reverse ^ m_globalReverse;
                dst->pen.conceal = src->attrs.conceal;
                dst->pen.strike = src->attrs.strike;
                dst->pen.font = src->attrs.font;
                dst->pen.small = src->attrs.small;
                dst->pen.baseline = src->attrs.baseline;
                dst->pen.fg = src->fg;
                dst->pen.bg = src->bg;
                if (src->width == 2 && pos.col < (newCols - 1))
                    (dst + 1)->chars[0] = kWideContinuation;
            }
            for (; pos.col < newCols; pos.col++)
                clearCell(newBuffer[pos.row * newCols + pos.col]);
            newRow--;
            if (active)
                fields->pos.row++;
        }
    }
    if (newRow >= 0) {
        // Scroll new rows back up to the top and fill in blanks at the bottom.
        const int moverows = newRows - newRow - 1;
        std::memmove(&newBuffer[0], &newBuffer[(newRow + 1) * newCols], size_t(moverows) * size_t(newCols) * sizeof(TermCell));
        std::memmove(&newLineinfo[0], &newLineinfo[newRow + 1], size_t(moverows) * sizeof(newLineinfo[0]));
        newCursor.row -= (newRow + 1);
        for (newRow = moverows; newRow < newRows; newRow++) {
            for (int col = 0; col < newCols; col++)
                clearCell(newBuffer[newRow * newCols + col]);
            newLineinfo[newRow] = VTermLineInfo {};
        }
    }

    // Split the flat buffer back into row handles (reusing the old buffers where they fit).
    for (size_t r = size_t(newRows); r < table.size(); ++r)
        std::free(table[r].cells);
    table.resize(size_t(newRows));
    for (int r = 0; r < newRows; ++r) {
        TermRow &row = table[size_t(r)];
        ensureCap(row, newCols);
        row.cols = uint32_t(newCols);
        std::memcpy(row.cells, &newBuffer[size_t(r) * size_t(newCols)], size_t(newCols) * sizeof(TermCell));
    }

    allocFree(oldLineinfo, nullptr);
    fields->lineinfos[bufidx] = newLineinfo;

    if (active)
        fields->pos = newCursor;
}
