#include "terminalview.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    m_font = QFont(QStringLiteral("monospace"));
    m_font.setPixelSize(15);
    m_font.setStyleHint(QFont::Monospace);
    setAcceptedMouseButtons(Qt::AllButtons);
    setActiveFocusOnTab(true);
    setOpaquePainting(true);

    connect(&m_pty, &PtySession::bytesRead, this, [this](const QByteArray &bytes) {
        if (!m_vt)
            return;
        vterm_input_write(m_vt, bytes.constData(), size_t(bytes.size()));
        vterm_screen_flush_damage(m_screen);
        if (m_scrollOffset > 0) { // new output snaps back to live, like every terminal
            m_scrollOffset = 0;
        }
        update();
    });
    connect(&m_pty, &PtySession::finished, this, &TerminalView::sessionFinished);
}

TerminalView::~TerminalView()
{
    if (m_vt)
        vterm_free(m_vt);
}

// ─── libvterm plumbing ──────────────────────────────────────────────────────────────────────────

int TerminalView::cbDamage(VTermRect, void *user)
{
    static_cast<TerminalView *>(user)->update();
    return 1;
}

int TerminalView::cbMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    self->m_cursor = pos;
    self->m_cursorVisible = visible != 0;
    self->update();
    return 1;
}

int TerminalView::cbSetTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    if (prop == VTERM_PROP_TITLE) {
        self->m_title = QString::fromUtf8(val->string.str, qsizetype(val->string.len));
        emit self->titleChanged();
    } else if (prop == VTERM_PROP_CURSORVISIBLE) {
        self->m_cursorVisible = val->boolean;
        self->update();
    }
    return 1;
}

int TerminalView::cbBell(void *user)
{
    emit static_cast<TerminalView *>(user)->bellRang();
    return 1;
}

int TerminalView::cbSbPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    SbLine line;
    line.cells.resize(cols);
    std::copy(cells, cells + cols, line.cells.begin());
    self->m_scrollback.append(std::move(line));
    if (self->m_scrollback.size() > self->m_scrollbackLimit)
        self->m_scrollback.removeFirst();
    return 1;
}

int TerminalView::cbSbPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    if (self->m_scrollback.isEmpty())
        return 0;
    const SbLine line = self->m_scrollback.takeLast();
    const int n = qMin(cols, int(line.cells.size()));
    std::copy(line.cells.begin(), line.cells.begin() + n, cells);
    for (int i = n; i < cols; ++i)
        cells[i] = VTermScreenCell{};
    return 1;
}

static const VTermScreenCallbacks kScreenCallbacks = {
    /*damage*/ TerminalView::cbDamage,
    /*moverect*/ nullptr,
    /*movecursor*/ TerminalView::cbMoveCursor,
    /*settermprop*/ TerminalView::cbSetTermProp,
    /*bell*/ TerminalView::cbBell,
    /*resize*/ nullptr,
    /*sb_pushline*/ TerminalView::cbSbPushLine,
    /*sb_popline*/ TerminalView::cbSbPopLine,
    /*sb_clear*/ nullptr,
};

void TerminalView::ensureSession()
{
    if (m_started)
        return;
    m_started = true;

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, [](const char *s, size_t len, void *user) {
        static_cast<TerminalView *>(user)->m_pty.writeBytes(QByteArray(s, int(len)));
    }, this);

    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &kScreenCallbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);

    // Default colours feed SGR-reset cells; the palette stays vterm's xterm-256 standard.
    VTermState *state = vterm_obtain_state(m_vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, uint8_t(m_foreground.red()), uint8_t(m_foreground.green()), uint8_t(m_foreground.blue()));
    vterm_color_rgb(&bg, uint8_t(m_background.red()), uint8_t(m_background.green()), uint8_t(m_background.blue()));
    vterm_state_set_default_colors(state, &fg, &bg);
    vterm_screen_reset(m_screen, 1);

    m_pty.start(m_rows, m_cols);
}

void TerminalView::componentComplete()
{
    QQuickPaintedItem::componentComplete();
    applyGrid();
    ensureSession();
}

void TerminalView::applyGrid()
{
    const QFontMetricsF fm(m_font);
    m_cellW = fm.horizontalAdvance(QLatin1Char('M'));
    m_cellH = fm.height();
    m_cellAscent = fm.ascent();
    const int cols = qMax(2, int(width() / m_cellW));
    const int rows = qMax(2, int(height() / m_cellH));
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    if (m_vt) {
        vterm_set_size(m_vt, rows, cols);
        vterm_screen_flush_damage(m_screen);
        m_pty.resize(rows, cols);
    }
    update();
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (isComponentComplete())
        applyGrid();
}

// ─── rendering ──────────────────────────────────────────────────────────────────────────────────

QColor TerminalView::toQColor(VTermColor c, bool isFg) const
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&c))
        return m_foreground;
    if (VTERM_COLOR_IS_DEFAULT_BG(&c))
        return m_background;
    vterm_screen_convert_color_to_rgb(m_screen, &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    Q_UNUSED(isFg);
}

void TerminalView::paint(QPainter *p)
{
    p->fillRect(boundingRect(), m_background);
    if (!m_screen)
        return;
    p->setFont(m_font);
    paintCells(p);
}

void TerminalView::paintCells(QPainter *p)
{
    const int sbShown = m_scrollOffset;
    QString run;
    // Rows: the top `sbShown` rows come from scrollback (when scrolled), the rest are live.
    for (int row = 0; row < m_rows; ++row) {
        const int liveRow = row - sbShown;
        const VTermScreenCell *sbCells = nullptr;
        int sbCols = 0;
        if (liveRow < 0) {
            const int idx = m_scrollback.size() + liveRow; // liveRow is negative
            if (idx < 0)
                continue;
            sbCells = m_scrollback.at(idx).cells.constData();
            sbCols = int(m_scrollback.at(idx).cells.size());
        }
        const qreal y = row * m_cellH;

        int col = 0;
        while (col < m_cols) {
            VTermScreenCell cell {};
            if (sbCells) {
                if (col < sbCols)
                    cell = sbCells[col];
            } else {
                vterm_screen_get_cell(m_screen, VTermPos{ liveRow, col }, &cell);
            }
            const int cw = cell.width > 0 ? cell.width : 1;

            QColor fg = toQColor(cell.fg, true);
            QColor bg = toQColor(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);

            // Merge the run of consecutive cells with identical attrs into ONE fill + ONE text
            // draw (the difference between 60fps and molasses on full-screen output).
            run.clear();
            const auto sameStyle = [&](const VTermScreenCell &o) {
                QColor ofg = toQColor(o.fg, true), obg = toQColor(o.bg, false);
                if (o.attrs.reverse)
                    std::swap(ofg, obg);
                return ofg == fg && obg == bg && o.attrs.bold == cell.attrs.bold
                    && o.attrs.underline == cell.attrs.underline && o.attrs.italic == cell.attrs.italic;
            };
            int runStart = col;
            while (col < m_cols) {
                VTermScreenCell c2 {};
                if (sbCells) {
                    if (col < sbCols)
                        c2 = sbCells[col];
                } else {
                    vterm_screen_get_cell(m_screen, VTermPos{ liveRow, col }, &c2);
                }
                if (col != runStart && !sameStyle(c2))
                    break;
                if (c2.chars[0] == 0) {
                    run.append(QLatin1Char(' '));
                } else {
                    for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && c2.chars[ci]; ++ci)
                        run.append(QString::fromUcs4(&c2.chars[ci], 1));
                }
                col += c2.width > 0 ? c2.width : 1;
            }

            const qreal x = runStart * m_cellW;
            const qreal w = (col - runStart) * m_cellW;
            if (bg != m_background)
                p->fillRect(QRectF(x, y, w, m_cellH), bg);
            if (!run.trimmed().isEmpty() || cell.attrs.underline) {
                QFont f = m_font;
                if (cell.attrs.bold)
                    f.setBold(true);
                if (cell.attrs.italic)
                    f.setItalic(true);
                if (cell.attrs.underline)
                    f.setUnderline(true);
                p->setFont(f);
                p->setPen(fg);
                p->drawText(QPointF(x, y + m_cellAscent), run);
                p->setFont(m_font);
            }
            Q_UNUSED(cw);
        }
    }

    // Cursor (block; hollow when unfocused) — hidden while scrolled back.
    if (m_cursorVisible && sbShown == 0) {
        const QRectF r(m_cursor.col * m_cellW, m_cursor.row * m_cellH, m_cellW, m_cellH);
        if (hasActiveFocus()) {
            p->setCompositionMode(QPainter::CompositionMode_Difference);
            p->fillRect(r, Qt::white);
            p->setCompositionMode(QPainter::CompositionMode_SourceOver);
        } else {
            p->setPen(m_foreground);
            p->drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
}

// ─── input ──────────────────────────────────────────────────────────────────────────────────────

void TerminalView::keyToVTerm(QKeyEvent *event)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (event->modifiers() & Qt::ShiftModifier)
        mod = VTermModifier(mod | VTERM_MOD_SHIFT);
    if (event->modifiers() & Qt::ControlModifier)
        mod = VTermModifier(mod | VTERM_MOD_CTRL);
    if (event->modifiers() & Qt::AltModifier)
        mod = VTermModifier(mod | VTERM_MOD_ALT);

    VTermKey key = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Tab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
            key = VTermKey(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        break;
    }

    if (key != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_vt, key, mod);
        return;
    }

    // Ctrl+letter etc.: hand libvterm the PLAIN character + the modifier — it builds the control
    // byte / CSI-u itself. Otherwise send the composed text as-is.
    const QString text = event->text();
    if ((mod & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        vterm_keyboard_unichar(m_vt, uint32_t('a' + (event->key() - Qt::Key_A)), mod);
        return;
    }
    if (!text.isEmpty()) {
        const auto ucs4 = text.toUcs4();
        for (const char32_t c : ucs4)
            vterm_keyboard_unichar(m_vt, c, event->modifiers() & Qt::AltModifier ? VTERM_MOD_ALT : VTERM_MOD_NONE);
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (!m_vt) {
        QQuickPaintedItem::keyPressEvent(event);
        return;
    }
    // Paste (Ctrl+Shift+V — the terminal convention; plain Ctrl+V belongs to the shell).
    if ((event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::ShiftModifier)
        && event->key() == Qt::Key_V) {
        sendText(QGuiApplication::clipboard()->text());
        event->accept();
        return;
    }
    // Typing snaps back to live output.
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        update();
    }
    keyToVTerm(event);
    event->accept();
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    const int lines = event->angleDelta().y() / 40; // 3 lines per notch
    m_scrollOffset = qBound(0, m_scrollOffset + lines, int(m_scrollback.size()));
    update();
    event->accept();
}

// ─── property surface ───────────────────────────────────────────────────────────────────────────

void TerminalView::setFontFamily(const QString &f)
{
    if (m_font.family() == f)
        return;
    m_font.setFamily(f);
    emit fontChanged();
    if (isComponentComplete())
        applyGrid();
    update();
}

void TerminalView::setFontSize(int px)
{
    if (m_font.pixelSize() == px)
        return;
    m_font.setPixelSize(qMax(6, px));
    emit fontChanged();
    if (isComponentComplete())
        applyGrid();
    update();
}

void TerminalView::setBackground(const QColor &c)
{
    if (m_background == c)
        return;
    m_background = c;
    emit colorsChanged();
    update();
}

void TerminalView::setForeground(const QColor &c)
{
    if (m_foreground == c)
        return;
    m_foreground = c;
    emit colorsChanged();
    update();
}

void TerminalView::setScrollbackLimit(int v)
{
    if (m_scrollbackLimit == v)
        return;
    m_scrollbackLimit = qMax(0, v);
    emit scrollbackLimitChanged();
}

void TerminalView::sendText(const QString &text)
{
    if (!m_vt)
        return;
    const auto ucs4 = text.toUcs4();
    for (const char32_t c : ucs4)
        vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
}
