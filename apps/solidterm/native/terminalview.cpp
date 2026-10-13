#include "terminalview.h"

#include "autoscroll.h"
#include "linkscan.h"
#include "perftrace.h"
#include "mousereport.h"
#include "scrollanchor.h"

#include "qmlcss/componentcache.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QUrl>

#include <memory>
#include <vector>

// ─── GPU glyph material ───────────────────────────────────────────────────────────────────────────
// Samples the coverage atlas (.r) and tints by the per-vertex foreground colour (premultiplied in
// the fragment shader). One instance per TerminalView; its texture is the item's glyph atlas.
namespace {

struct GlyphVertex { float x, y, u, v, r, g, b, a, isColor; };

const QSGGeometry::Attribute kGlyphAttrs[] = {
    QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),  // pos
    QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType, false), // uv
    QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType, false), // color
    QSGGeometry::Attribute::create(3, 1, QSGGeometry::FloatType, false), // isColor (emoji flag)
};
const QSGGeometry::AttributeSet kGlyphAttrSet = { 4, sizeof(GlyphVertex), kGlyphAttrs };

class GlyphMaterial : public QSGMaterial {
public:
    GlyphMaterial() { setFlag(Blending, true); }
    QSGMaterialType *type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *o) const override {
        const auto *m = static_cast<const GlyphMaterial *>(o);
        if (texture == m->texture)
            return 0;
        const qint64 a = texture ? texture->comparisonKey() : 0;
        const qint64 b = m->texture ? m->texture->comparisonKey() : 0;
        return a < b ? -1 : 1;
    }
    // The material OWNS its atlas texture, so it's freed on the render thread when the node tree is
    // torn down — never touched from the GUI thread. `texture` is the raw view the shader binds.
    void setTexture(QSGTexture *t) { m_tex.reset(t); texture = t; }
    QSGTexture *texture = nullptr;

private:
    std::unique_ptr<QSGTexture> m_tex;
};

class GlyphShader : public QSGMaterialShader {
public:
    GlyphShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/solidterm/shaders/glyph.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/solidterm/shaders/glyph.frag.qsb"));
    }
    bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *) override
    {
        QByteArray *buf = state.uniformData();
        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 m = state.combinedMatrix();
            memcpy(buf->data(), m.constData(), 64);
            changed = true;
        }
        if (state.isOpacityDirty()) {
            const float o = state.opacity();
            memcpy(buf->data() + 64, &o, 4);
            changed = true;
        }
        return changed;
    }
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override
    {
        if (binding != 1)
            return;
        auto *mat = static_cast<GlyphMaterial *>(newMaterial);
        if (!mat->texture)
            return;
        mat->texture->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = mat->texture;
    }
};

QSGMaterialShader *GlyphMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new GlyphShader;
}

} // namespace

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickItem(parent)
{
    m_font = QFont(QStringLiteral("monospace"));
    m_font.setPixelSize(15);
    m_font.setStyleHint(QFont::Monospace);
    // Right is normally captured by the solid <ContextMenu>'s MouseArea, which sits ABOVE this item
    // and only declines (mouse.accepted = false) when the running program wants mouse reports and
    // Shift isn't held — see wantsMouseReport(). Declared here too so THAT right-press (forwarded by
    // Qt Quick's own hit-testing once the MouseArea declines) actually reaches mousePressEvent.
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setActiveFocusOnTab(true);
    setAcceptHoverEvents(true);               // hover to detect/underline links
    setFlag(ItemHasContents, true);
    // Without this Qt's compose/IME layer never engages for the terminal: a dead key is dropped
    // and the next letter arrives bare (pt-BR ´ + a typed "a", not "á").
    setFlag(ItemAcceptsInputMethod, true);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(530);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        if (!hasActiveFocus() || !m_cursorVisible || m_scrollOffset > 0)
            return;
        m_blinkOn = !m_blinkOn;
        update();
    });
    m_blinkTimer->start();
    connect(this, &QQuickItem::activeFocusChanged, this, [this] {
        resetBlink();
        updateFocusReport();
        // Losing focus mid-drag (a popup/dialog/menu grabbing it, window deactivation) is one of the
        // ways a local selection drag can end without ever reaching mouseReleaseEvent.
        if (!hasActiveFocus() && m_selecting) {
            m_selecting = false;
            stopAutoscroll();
            resumeOutputFromSelection();
        }
    });

    // Visual bell: BEL raises a brief bright wash, cleared ~130ms later. bellRang is emitted from the
    // vterm callback on the GUI thread, so this runs synchronously here (update() is safe).
    connect(this, &TerminalView::bellRang, this, [this] {
        m_bellActive = true;
        update();
        QTimer::singleShot(130, this, [this] { m_bellActive = false; update(); });
        // Urgency hint: when the window isn't focused, ask the WM for attention (taskbar flash / X11
        // urgency / wlr activation) so a bell in a background terminal is noticed. alert(0) stays until
        // the window is activated again.
        if (QWindow *w = window(); w && !w->isActive())
            w->alert(0);
    });

    connect(&m_pty, &PtySession::bytesRead, this, [this](const QByteArray &bytes) {
        if (!m_vt)
            return;
        if (Q_UNLIKELY(PerfTrace::on)) {
            PerfTrace::stage(PerfTrace::PtyRead);
            ++PerfTrace::ptyReads;
            PerfTrace::ptyBytes += bytes.size();
        }
        // One batch per read: the vterm callbacks only RECORD that the scrollback/offset moved or the
        // cursor moved (m_inInput), and the batch notifies once at the end — a `seq` scrolls ~500
        // lines per 4 KiB read, and per-line scrollChanged() re-ran the QML scrollbar's bindings and
        // its pulse() handler for every line (26% of the main thread), per-line resetBlink()
        // restarted the blink QTimer (3.5%). Nothing renders mid-batch, so the observable state is
        // the same.
        m_inInput = true;
        m_term.beginBatch();
        vterm_input_write(m_vt, bytes.constData(), size_t(bytes.size()));
        if (m_term.endBatch() && Q_UNLIKELY(PerfTrace::on))
            PerfTrace::stage(PerfTrace::Damage);
        m_inInput = false;
        if (m_scrollPending) {
            m_scrollPending = false;
            emit scrollChanged();
        }
        if (m_blinkPending) {
            m_blinkPending = false;
            resetBlink();
        }
        // Output never moves a scrolled-back view (VTE/Tilix default): a TUI that redraws a spinner
        // many times a second (Claude Code) made reading the history impossible. Typing and paste
        // still snap back to live; lines pushed meanwhile keep the view anchored (cbSbPushLine).
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

// Content changed outside a pty batch (resize, reset): repaint. Inside a batch TermScreen makes no
// call at all — the bytesRead handler update()s once at its end.
void TerminalView::cbDamage(void *user)
{
    if (Q_UNLIKELY(PerfTrace::on))
        PerfTrace::stage(PerfTrace::Damage);
    auto *self = static_cast<TerminalView *>(user);
    if (!self->m_inInput)
        self->update();
}

int TerminalView::cbMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    self->m_cursor = pos;
    self->m_cursorVisible = visible != 0;
    if (self->m_inInput)
        self->m_blinkPending = true; // once per pty batch, see the bytesRead handler
    else
        self->resetBlink();
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
    } else if (prop == VTERM_PROP_MOUSE) {
        // DECSET 1000/1002/1003 (CLICK/DRAG/MOVE) or a reset to NONE — decideMouseAction() in
        // mousereport.h reads this on every mouse event, no further bookkeeping needed here.
        self->m_mouseMode = val->number;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        self->m_altScreen = val->boolean;
        self->notifyScrollChanged(); // the scrollbar hides on the alt screen (vim/less), see altScreen()
    }
    return 1;
}

int TerminalView::cbBell(void *user)
{
    emit static_cast<TerminalView *>(user)->bellRang();
    return 1;
}

void TerminalView::itemChange(ItemChange change, const ItemChangeData &data)
{
    // Cross-window move (pane detach/reattach): our cached scene-graph nodes and the glyph-atlas
    // texture belong to the OLD window's render context. Drop the cached pointers (Qt frees the old
    // nodes on the scene change) and force an atlas re-upload so updatePaintNode rebuilds fresh in the
    // new window — otherwise it would touch stale nodes and hang/crash on the new render thread.
    if (change == ItemSceneChange) {
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
        m_imageNode = nullptr;
        m_glyphs.reset();
        // Window close (data.window == nullptr, item leaving the scene for good) or pane detach/
        // reattach (data.window == the new window) both end any local selection drag that was live —
        // either way, pty reads must resume rather than stay frozen forever.
        if (m_selecting) {
            m_selecting = false;
            stopAutoscroll();
        }
        resumeOutputFromSelection();
        update();
    }
    // Focus reporting (DECSET 1004) cares about the WINDOW's activation, not just this item's — a
    // cross-window pane move (detach/reattach) means the old window's activeChanged must stop
    // driving it. Qt auto-disconnects when either end is destroyed, so this is also safe on teardown.
    if (change == ItemSceneChange) {
        QObject::disconnect(m_winActiveConn);
        m_winActiveConn = data.window
            ? connect(data.window, &QWindow::activeChanged, this, [this] { updateFocusReport(); })
            : QMetaObject::Connection();
    }
    QQuickItem::itemChange(change, data);
    // A new window (or the same window moved to a screen with another scale) changes the device
    // pixel ratio the atlas must be rasterised at: rebuild the font + grid for it.
    if ((change == ItemSceneChange && data.window) || change == ItemDevicePixelRatioHasChanged) {
        applyGrid();
        update();
    }
}

// A line entered the scrollback (TermScreen MOVED the row there; a full ring recycled its oldest).
void TerminalView::cbSbPushed(int sbCount, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    self->m_scrollOffset = offsetAfterPush(self->m_scrollOffset, sbCount);
    self->notifyScrollChanged(); // scrollbackCount (and maybe scrollOffset) moved — the scrollbar listens
}

// A line left the scrollback (popped back onto the screen when the terminal grew taller, or trimmed
// by a lower limit). TermScreen pads a line pushed while narrower with REAL blanks — a zeroed cell
// has width 0 and hangs libvterm's backfill loop forever (see scrollbackcells.h).
void TerminalView::cbSbPopped(int sbCount, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    self->m_scrollOffset = clampOffset(self->m_scrollOffset, sbCount);
    self->notifyScrollChanged();
}

static const TermScreenHooks kTermHooks = {
    /*movecursor*/ TerminalView::cbMoveCursor,
    /*settermprop*/ TerminalView::cbSetTermProp,
    /*bell*/ TerminalView::cbBell,
    /*damage*/ TerminalView::cbDamage,
    /*sbPushed*/ TerminalView::cbSbPushed,
    /*sbPopped*/ TerminalView::cbSbPopped,
};

void TerminalView::ensureSession()
{
    if (m_started)
        return;
    m_started = true;

    m_vt = TermScreen::newVTerm(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, [](const char *s, size_t len, void *user) {
        if (Q_UNLIKELY(PerfTrace::on))
            PerfTrace::stage(PerfTrace::PtyWrite);
        static_cast<TerminalView *>(user)->m_pty.writeBytes(QByteArray(s, int(len)));
    }, this);

    // Our own screen over libvterm's state layer (primary + alt screen + scrollback ring).
    m_term.setScrollbackLimit(m_scrollbackLimit);
    m_term.attach(m_vt, &kTermHooks, this);

    // Default colours feed SGR-reset cells. ANSI 0–15 come from the scheme palette at draw time
    // (toQColor); without one they stay vterm's xterm-256 standard.
    VTermState *state = vterm_obtain_state(m_vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, uint8_t(m_foreground.red()), uint8_t(m_foreground.green()), uint8_t(m_foreground.blue()));
    vterm_color_rgb(&bg, uint8_t(m_background.red()), uint8_t(m_background.green()), uint8_t(m_background.blue()));
    vterm_state_set_default_colors(state, &fg, &bg);
    m_term.reset(true);

    QString cwd;
    QStringList env;
    if (m_hasStart) {
        cwd = m_start.cwd;
        env = m_start.env;
    } else if (QQuickWindow *w = window()) {
        cwd = w->property("solidtermCwd").toString();
        env = w->property("solidtermEnv").toStringList();
    }
    if (cwd.isEmpty() && env.isEmpty() && s_pendingSpawn) {
        cwd = s_pendingSpawn->cwd;
        env = s_pendingSpawn->env;
    }
    m_pty.start(m_rows, m_cols, QString(), cwd, env);
    updateFocusReport(); // sync the program's focus state with whatever we're starting with
}

void TerminalView::componentComplete()
{
    QQuickItem::componentComplete();
    ensureStarted();
}

void TerminalView::ensureStarted()
{
    applyGrid();
    ensureSession();
    composeScrollBar(); // no-op once composed; needs this item's QQmlEngine, available by now
}

void TerminalView::applyGrid()
{
    // The grid uses the GlyphCache's (integer) cell metrics, so every cell lands exactly on an
    // atlas tile — glyphs stay crisp (no fractional placement). setFont is a no-op when unchanged.
    // Rasterised at the window's device pixel ratio (crisp on HiDPI); cell metrics stay logical.
    m_glyphs.setFont(m_font.family(), m_font.pixelSize(), window() ? window()->effectiveDevicePixelRatio() : 1.0);
    m_cellW = m_glyphs.cellWidth();
    m_cellH = m_glyphs.cellHeight();
    const int cols = qMax(2, int(width() / m_cellW));
    const int rows = qMax(2, int(height() / m_cellH));
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    if (m_vt) {
        vterm_set_size(m_vt, rows, cols); // TermScreen resizes (and damages) inside
        m_pty.resize(rows, cols);
    }
    emit scrollChanged(); // viewportRows moved — the scrollbar's thumb size depends on it
    update();
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
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
    if (m_hasPalette && VTERM_COLOR_IS_INDEXED(&c) && c.indexed.idx < 16)
        return m_palette[c.indexed.idx];
    vterm_state_convert_color_to_rgb(m_term.state(), &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    Q_UNUSED(isFg);
}

// ─── scene-graph build ────────────────────────────────────────────────────────────────────────────
// Runs on the render thread with the GUI thread blocked (the Quick sync point), so reading the
// vterm screen here is safe. Builds three geometry nodes drawn back-to-front: cell backgrounds +
// selection (vertex colour), glyphs (atlas material), cursor (vertex colour, on top).

namespace {

inline void pushBgQuad(std::vector<QSGGeometry::ColoredPoint2D> &v, qreal x, qreal y, qreal w,
                       qreal h, const QColor &c)
{
    // QSGVertexColorMaterial blends PREMULTIPLIED, so premultiply here — a straight (255,255,255,179)
    // adds to the backdrop instead of blending, saturating translucent white to opaque white.
    const int ca = c.alpha();
    const uchar r = uchar(c.red() * ca / 255), g = uchar(c.green() * ca / 255),
                b = uchar(c.blue() * ca / 255), a = uchar(ca);
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    QSGGeometry::ColoredPoint2D tl, tr, bl, br;
    tl.set(x0, y0, r, g, b, a);
    tr.set(x1, y0, r, g, b, a);
    bl.set(x0, y1, r, g, b, a);
    br.set(x1, y1, r, g, b, a);
    v.insert(v.end(), { tl, tr, bl, bl, tr, br }); // two triangles
}

inline void pushGlyphQuad(std::vector<GlyphVertex> &v, qreal x, qreal y, qreal w, qreal h,
                          const QRectF &uv, const QColor &c, bool colorGlyph)
{
    // The glyph fragment shader is premultiplied (vColor × coverage), so premultiply here — lets a
    // translucent foreground carry alpha < 1 correctly (opaque fg keeps alpha 1, unchanged).
    const float al = float(c.alphaF());
    const float r = float(c.redF()) * al, g = float(c.greenF()) * al, b = float(c.blueF()) * al, a = al;
    const float ic = colorGlyph ? 1.0f : 0.0f;
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    const float u0 = float(uv.left()), v0 = float(uv.top());
    const float u1 = float(uv.right()), v1 = float(uv.bottom());
    const GlyphVertex tl{ x0, y0, u0, v0, r, g, b, a, ic };
    const GlyphVertex tr{ x1, y0, u1, v0, r, g, b, a, ic };
    const GlyphVertex bl{ x0, y1, u0, v1, r, g, b, a, ic };
    const GlyphVertex br{ x1, y1, u1, v1, r, g, b, a, ic };
    v.insert(v.end(), { tl, tr, bl, bl, tr, br });
}

} // namespace

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    if (Q_UNLIKELY(PerfTrace::on)) {
        PerfTrace::stage(PerfTrace::PaintBegin);
        const int64_t t0 = PerfTrace::nowNs();
        QSGNode *n = buildPaintNode(oldNode, data);
        PerfTrace::paint(PerfTrace::nowNs() - t0);
        PerfTrace::stage(PerfTrace::PaintEnd);
        return n;
    }
    return buildPaintNode(oldNode, data);
}

QSGNode *TerminalView::buildPaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_term.isAttached() || width() <= 0 || height() <= 0) {
        delete oldNode;
        m_imageNode = nullptr;
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
        return nullptr;
    }

    // Scratch vertex arrays, reused frame to frame (per render thread: panes in other windows build
    // concurrently) — a fresh vector per frame re-grew through ~11 reallocations of up to 1.2 MB.
    static thread_local std::vector<QSGGeometry::ColoredPoint2D> bg;
    static thread_local std::vector<GlyphVertex> glyphs;
    static thread_local std::vector<QSGGeometry::ColoredPoint2D> cursor;
    bg.clear();
    glyphs.clear();
    cursor.clear();

    // Base background fills the whole item. Its alpha = backgroundOpacity, so at <1 the layer behind
    // (a bg image, or — with transparent chrome — the desktop) shows through the terminal's colour.
    QColor baseBg = m_background;
    baseBg.setAlphaF(m_bgOpacity);
    pushBgQuad(bg, 0, 0, width(), height(), baseBg);

    const int sbShown = m_scrollOffset;
    const int sbCount = m_term.sbCount();
    m_term.clearDirty(); // every row is rebuilt below (per-row geometry reuse is proposal P4)
    // Past the end of a scrollback row narrower than the grid: a value-initialised cell, exactly what
    // the previous `VTermScreenCell cell {}` gave (zero colours = RGB black, not the default).
    static const TermCell kPastEnd {};
    for (int row = 0; row < m_rows; ++row) {
        const int liveRow = row - sbShown;
        TermScreen::Line line;
        if (liveRow < 0) {
            const int idx = sbCount + liveRow;
            if (idx < 0)
                continue;
            line = m_term.line(idx);
        } else {
            line = m_term.liveLine(liveRow);
        }
        // DECSCNM applies to live rows at read time (scrollback rows have it baked in).
        const bool flipReverse = line.live && m_term.globalReverse();
        const qreal y = row * m_cellH;

        int col = 0;
        while (col < m_cols) {
            const bool inLine = col < line.cols;
            const TermCell &cell = inLine ? line.cells[col] : kPastEnd;
            const int cw = inLine ? TermScreen::widthAt(line.cells, line.cols, col) : 1;
            const qreal x = col * m_cellW;
            const qreal w = cw * m_cellW;

            QColor fg = toQColor(cell.pen.fg, true);
            QColor bgc = toQColor(cell.pen.bg, false);
            if (cell.pen.reverse ^ flipReverse)
                std::swap(fg, bgc);
            if (bgc != m_background)
                pushBgQuad(bg, x, y, w, m_cellH, bgc);

            // One textured quad per non-blank cell — the atlas makes per-cell quads cheap, so the
            // old run-merge (a QPainter optimisation) is gone.
            // A single codepoint (nearly every cell) is looked up without building a QString; only
            // a real cluster (base + combining marks) pays for one.
            const char32_t c0 = cell.chars[0];
            if (c0 && !(c0 == U' ' && !cell.chars[1])) {
                GlyphCache::Entry e;
                if (!cell.chars[1]) {
                    e = m_glyphs.glyph(c0, cell.pen.bold, cell.pen.italic, cw);
                } else {
                    QString cluster;
                    for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
                        cluster.append(QString::fromUcs4(&cell.chars[ci], 1));
                    e = m_glyphs.glyph(cluster, cell.pen.bold, cell.pen.italic, cw);
                }
                if (e.valid)
                    pushGlyphQuad(glyphs, x, y, w, m_cellH, e.uv, fg, e.color);
            }
            if (cell.pen.underline)
                pushBgQuad(bg, x, y + m_cellH - 1, w, 1, fg);

            col += cw;
        }
    }

    // Selection: a translucent highlight behind the text (drawn last in the bg node → over cell
    // backgrounds, under glyphs).
    if (m_selValid) {
        CellPos from, to;
        selectedRange(from, to);
        QColor sel = m_foreground;
        sel.setAlpha(64);
        for (int row = 0; row < m_rows; ++row) {
            const int absRow = sbCount - sbShown + row;
            if (absRow < from.absRow || absRow > to.absRow)
                continue;
            const int c0 = absRow == from.absRow ? from.col : 0;
            const int c1 = absRow == to.absRow ? to.col : m_cols - 1;
            pushBgQuad(bg, c0 * m_cellW, row * m_cellH, (c1 - c0 + 1) * m_cellW, m_cellH, sel);
        }
    }

    // Cursor: a semi-transparent block (focused) so the glyph shows through, or an outline
    // (unfocused). Hidden while scrolled back.
    if (m_cursorVisible && sbShown == 0) {
        const qreal cx = m_cursor.col * m_cellW, cy = m_cursor.row * m_cellH;
        if (hasActiveFocus()) {
            if (m_blinkOn) {
                QColor cur = m_foreground;
                cur.setAlpha(140);
                pushBgQuad(cursor, cx, cy, m_cellW, m_cellH, cur);
            }
        } else {
            pushBgQuad(cursor, cx, cy, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy + m_cellH - 1, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy, 1, m_cellH, m_foreground);
            pushBgQuad(cursor, cx + m_cellW - 1, cy, 1, m_cellH, m_foreground);
        }
    }

    // Unfocused split pane: a subtle dark overlay on top of everything (tilix "dim inactive").
    if (m_dimmed)
        pushBgQuad(cursor, 0, 0, width(), height(), QColor(0, 0, 0, 66));

    // Visual bell: a brief bright wash over the pane (audible bells are jarring; tilix defaults to a
    // visual flash). m_bellActive is raised on BEL and cleared by a short timer (see the ctor).
    if (m_bellActive) {
        QColor flash = m_foreground;
        flash.setAlphaF(0.30);
        pushBgQuad(cursor, 0, 0, width(), height(), flash);
    }

    // Search highlights: every match on a visible row (current match brighter).
    if (!m_matches.isEmpty()) {
        const int top = sbCount - sbShown;
        for (int mi = 0; mi < m_matches.size(); ++mi) {
            const Match &m = m_matches[mi];
            const int row = m.absRow - top;
            if (row < 0 || row >= m_rows)
                continue;
            const QColor hl = mi == m_searchIndex ? QColor(255, 160, 0, 210) : QColor(230, 200, 0, 90);
            pushBgQuad(bg, m.c0 * m_cellW, row * m_cellH, m.len * m_cellW, m_cellH, hl);
        }
    }

    // Hovered hyperlink: underline it across its span (drawn in the bg layer, at the cell baseline).
    if (m_hoverLink.valid) {
        const int row = m_hoverLink.absRow - (sbCount - sbShown);
        if (row >= 0 && row < m_rows) {
            pushBgQuad(bg, m_hoverLink.c0 * m_cellW, row * m_cellH + m_cellH - 1,
                       (m_hoverLink.c1 - m_hoverLink.c0 + 1) * m_cellW, 1, m_foreground);
        }
    }

    // If the atlas overflowed mid-build (astronomically unlikely), reset it and ask the GUI thread
    // (queued — we're on the render thread here) to re-render with a fresh atlas.
    if (m_glyphs.overflowed()) {
        m_glyphs.reset();
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }

    QSGNode *root = oldNode;
    if (!root) {
        root = new QSGNode;
        m_imageNode = nullptr;
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
    }

    // Layer 0 (optional): the background image, cover-fit, below everything.
    if (m_bgImage.isNull()) {
        if (m_imageNode) {
            root->removeChildNode(m_imageNode);
            delete m_imageNode;
            m_imageNode = nullptr;
        }
    } else {
        if (!m_imageNode) {
            m_imageNode = window()->createImageNode();
            m_imageNode->setOwnsTexture(true);
            root->prependChildNode(m_imageNode); // below bg/glyph/cursor
        }
        if (m_bgImageDirty || !m_imageNode->texture()) {
            m_imageNode->setTexture(window()->createTextureFromImage(m_bgImage));
            m_bgImageDirty = false;
        }
        m_imageNode->setRect(0, 0, width(), height());
        const qreal iw = qMax(1, m_bgImage.width()), ih = qMax(1, m_bgImage.height());
        const qreal scale = qMax(width() / iw, height() / ih); // cover
        const qreal sw = width() / scale, sh = height() / scale;
        m_imageNode->setSourceRect(QRectF((iw - sw) / 2, (ih - sh) / 2, sw, sh));
    }

    // Backgrounds + selection (vertex colour).
    if (!m_bgNode) {
        m_bgNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_bgNode->setGeometry(geo);
        m_bgNode->setFlag(QSGNode::OwnsGeometry);
        m_bgNode->setMaterial(new QSGVertexColorMaterial);
        m_bgNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_bgNode);
    }
    {
        QSGGeometry *geo = m_bgNode->geometry();
        geo->allocate(int(bg.size()));
        memcpy(geo->vertexData(), bg.data(), bg.size() * sizeof(QSGGeometry::ColoredPoint2D));
        m_bgNode->markDirty(QSGNode::DirtyGeometry);
    }

    // Glyphs (atlas material). Rebuild the texture only when the atlas changed.
    if (!m_glyphNode) {
        m_glyphNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(kGlyphAttrSet, 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_glyphNode->setGeometry(geo);
        m_glyphNode->setFlag(QSGNode::OwnsGeometry);
        m_glyphNode->setMaterial(new GlyphMaterial);
        m_glyphNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_glyphNode);
    }
    auto *glyphMat = static_cast<GlyphMaterial *>(m_glyphNode->material());
    if (m_glyphs.takeDirty())
        glyphMat->setTexture(window()->createTextureFromImage(m_glyphs.atlas()));
    {
        QSGGeometry *geo = m_glyphNode->geometry();
        geo->allocate(int(glyphs.size()));
        if (!glyphs.empty())
            memcpy(geo->vertexData(), glyphs.data(), glyphs.size() * sizeof(GlyphVertex));
        m_glyphNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    // Cursor (vertex colour, on top).
    if (!m_cursorNode) {
        m_cursorNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_cursorNode->setGeometry(geo);
        m_cursorNode->setFlag(QSGNode::OwnsGeometry);
        m_cursorNode->setMaterial(new QSGVertexColorMaterial);
        m_cursorNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_cursorNode);
    }
    {
        QSGGeometry *geo = m_cursorNode->geometry();
        geo->allocate(int(cursor.size()));
        if (!cursor.empty())
            memcpy(geo->vertexData(), cursor.data(), cursor.size() * sizeof(QSGGeometry::ColoredPoint2D));
        m_cursorNode->markDirty(QSGNode::DirtyGeometry);
    }

    return root;
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

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    const QString commit = event->commitString();
    // Pre-edit (the pending dead key / IME composition) is not drawn in the grid; only the
    // committed text reaches the shell, exactly as a typed character would.
    if (m_vt && !m_readOnly && !commit.isEmpty()) {
        if (m_scrollOffset > 0) {
            m_scrollOffset = 0;
            emit scrollChanged();
            update();
        }
        resetBlink();
        for (const char32_t c : commit.toUcs4())
            vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
    }
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: // where an IME candidate popup should appear: the cursor cell
        return QRectF(m_cursor.col * m_cellW, m_cursor.row * m_cellH, m_cellW, m_cellH);
    case Qt::ImHints: // a shell, not prose: no auto-capitalisation / prediction
        return int(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhPreferLatin);
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (Q_UNLIKELY(PerfTrace::on))
        PerfTrace::stage(PerfTrace::Key);
    if (!m_vt) {
        QQuickItem::keyPressEvent(event);
        return;
    }
    // Reserved accelerators (split/close/focus … from the solid config) are consumed HERE, before
    // the pty — so they never leak to the shell and never hit Qt's ambiguous Shortcut map.
    if (!m_reserved.isEmpty()) {
        // Font-zoom chords by KEY CODE, not character: pressing Ctrl+'+' is really Ctrl+Shift+'=' (and
        // on non-US layouts the '='/'−'/'0' characters shift around), so a PortableText match misses
        // them. Normalise Ctrl+(Plus|Equal)→"Ctrl+=", Ctrl+(Minus|Underscore)→"Ctrl+-", Ctrl+0→"Ctrl+0"
        // regardless of Shift/layout, then dispatch through the same reserved-accelerator path.
        if (event->modifiers() & Qt::ControlModifier) {
            QString norm;
            switch (event->key()) {
            case Qt::Key_Plus: case Qt::Key_Equal:       norm = QStringLiteral("Ctrl+="); break;
            case Qt::Key_Minus: case Qt::Key_Underscore: norm = QStringLiteral("Ctrl+-"); break;
            case Qt::Key_0:                              norm = QStringLiteral("Ctrl+0"); break;
            }
            if (!norm.isEmpty() && m_reserved.contains(norm)) {
                emit accelerator(norm);
                event->accept();
                return;
            }
        }
        const QString seq = QKeySequence(event->keyCombination()).toString(QKeySequence::PortableText);
        if (!seq.isEmpty() && m_reserved.contains(seq)) {
            emit accelerator(seq);
            event->accept();
            return;
        }
    }
    // Copy/paste (Ctrl+Shift+C/V — the terminal convention; plain Ctrl+C/V belong to the shell).
    if ((event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_V) {
            pasteClipboard();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_C && m_selValid) {
            copySelection();
            event->accept();
            return;
        }
    }
    // Shift+Insert pastes the clipboard (the other terminal convention).
    if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_Insert) {
        pasteClipboard();
        event->accept();
        return;
    }
    // Typing snaps back to live output.
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        emit scrollChanged();
        update();
    }
    if (m_readOnly) { // read-only pane: consume the key but don't feed the pty
        event->accept();
        return;
    }
    resetBlink();
    keyToVTerm(event);
    event->accept();
}

TerminalView::CellPos TerminalView::cellAt(const QPointF &p) const
{
    CellPos c;
    const int row = qBound(0, int(p.y() / m_cellH), m_rows - 1);
    c.absRow = m_term.sbCount() - m_scrollOffset + row;
    c.col = qBound(0, int(p.x() / m_cellW), m_cols - 1);
    return c;
}

void TerminalView::selectedRange(CellPos &from, CellPos &to) const
{
    const bool fwd = m_selAnchor.absRow < m_selEnd.absRow
        || (m_selAnchor.absRow == m_selEnd.absRow && m_selAnchor.col <= m_selEnd.col);
    from = fwd ? m_selAnchor : m_selEnd;
    to = fwd ? m_selEnd : m_selAnchor;
}

QString TerminalView::selectedText() const
{
    if (!m_selValid)
        return QString();
    CellPos from, to;
    selectedRange(from, to);
    QString out;
    for (int absRow = from.absRow; absRow <= to.absRow; ++absRow) {
        const int c0 = absRow == from.absRow ? from.col : 0;
        const int c1 = absRow == to.absRow ? to.col : m_cols - 1;
        QString line;
        const TermScreen::Line src = m_term.line(absRow);
        for (int col = c0; col <= c1; ++col) {
            if (!src.cells) // past the live screen
                break;
            VTermScreenCell cell;
            m_term.cell(src, col, &cell);
            if (cell.chars[0] == 0) {
                line.append(QLatin1Char(' '));
            } else {
                for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
                    line.append(QString::fromUcs4(&cell.chars[ci], 1));
            }
            if (cell.width > 1)
                col += cell.width - 1;
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        out += line;
        if (absRow != to.absRow)
            out += QLatin1Char('\n');
    }
    return out;
}

void TerminalView::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

// "Copy link" / "Open link" (right-click context menu, Solid-side — see term.tsx): act on whatever
// link is CURRENTLY hovered. The menu only offers these when hoverLinkUrl() is non-empty (see
// hoverLinkChanged), so by the time either runs m_hoverLink should still be valid — but the mouse
// could in principle have moved between the right-click and the menu action, so both no-op quietly
// on an empty/stale hover rather than acting on the wrong link.
void TerminalView::copyHoverLink()
{
    if (!m_hoverLink.valid)
        return;
    QGuiApplication::clipboard()->setText(m_hoverLink.url);
    QGuiApplication::clipboard()->setText(m_hoverLink.url, QClipboard::Selection);
}

void TerminalView::openHoverLink()
{
    if (m_hoverLink.valid)
        QDesktopServices::openUrl(QUrl::fromUserInput(m_hoverLink.url));
}

void TerminalView::pasteClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    // Bracketed-paste-worthy safety: multiline text (a stray newline runs the command) → confirm
    // in the Solid dialog first; single-line pastes go straight through.
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
        emit unsafePasteRequested(text);
    else
        sendText(text);
}

void TerminalView::pasteText(const QString &text)
{
    sendText(text); // confirmed (or explicitly-chosen) paste — no re-prompt
}

void TerminalView::clearScrollback()
{
    m_term.clearScrollback();
    m_scrollOffset = 0;
    m_selValid = false;
    emit selectionChanged();
    emit scrollChanged();
    update();
}

void TerminalView::setDimmed(bool v)
{
    if (m_dimmed == v)
        return;
    m_dimmed = v;
    update();
}

void TerminalView::setReadOnly(bool v)
{
    if (m_readOnly == v)
        return;
    m_readOnly = v;
    emit readOnlyChanged();
}

void TerminalView::resetTerminal()
{
    if (m_term.isAttached())
        m_term.reset(true); // hard reset: clears attrs, modes, cursor
    m_scrollOffset = 0;
    emit scrollChanged();
    update();
}

void TerminalView::resetBlink()
{
    m_blinkOn = true;
    if (m_blinkTimer)
        m_blinkTimer->start(); // restart the phase so the cursor stays solid right after activity
    update();
}

// DECSET 1004: a program (tmux, vim) that asked for focus events gets a report whenever THIS pane
// both has Qt's active focus AND its window is the active one — the closest native analogue of
// "the terminal application is focused". vterm_state_focus_in/out() decide internally whether the
// program actually asked for 1004; we just report the transition, unconditionally, like xterm does.
void TerminalView::updateFocusReport()
{
    if (!m_vt)
        return;
    const bool focused = hasActiveFocus() && window() && window()->isActive();
    if (focused == m_focusReported)
        return;
    m_focusReported = focused;
    VTermState *state = vterm_obtain_state(m_vt);
    if (focused)
        vterm_state_focus_in(state);
    else
        vterm_state_focus_out(state);
}

// ─── search ─────────────────────────────────────────────────────────────────────────────────────

QString TerminalView::rowText(int absRow) const
{
    QString line(m_cols, QLatin1Char(' '));
    if (!m_term.isAttached() || absRow < 0)
        return line;
    const TermScreen::Line src = m_term.line(absRow); // empty past the live screen
    for (int c = 0; c < m_cols && c < src.cols; ++c)
        if (src.cells[c].chars[0])
            line[c] = QChar(char32_t(src.cells[c].chars[0]));
    return line;
}

void TerminalView::search(const QString &query)
{
    m_searchQuery = query;
    m_matches.clear();
    m_searchIndex = -1;
    if (!query.isEmpty() && m_term.isAttached()) {
        const int total = m_term.sbCount() + m_rows;
        for (int absRow = 0; absRow < total; ++absRow) {
            const QString line = rowText(absRow);
            int from = 0;
            while (true) {
                const int idx = line.indexOf(query, from, Qt::CaseInsensitive);
                if (idx < 0)
                    break;
                m_matches.append({ absRow, idx, int(query.length()) });
                from = idx + query.length();
            }
        }
        if (!m_matches.isEmpty()) {
            m_searchIndex = m_matches.size() - 1; // most recent (nearest the bottom)
            scrollToMatch(m_searchIndex);
        }
    }
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::scrollToMatch(int i)
{
    if (i < 0 || i >= m_matches.size())
        return;
    const int sbSize = m_term.sbCount();
    const int desiredTop = m_matches[i].absRow - m_rows / 2; // centre the match
    m_scrollOffset = qBound(0, sbSize - desiredTop, sbSize);
    emit scrollChanged();
}

void TerminalView::searchNext()
{
    if (m_matches.isEmpty())
        return;
    m_searchIndex = (m_searchIndex + 1) % m_matches.size();
    scrollToMatch(m_searchIndex);
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::searchPrev()
{
    if (m_matches.isEmpty())
        return;
    m_searchIndex = (m_searchIndex - 1 + m_matches.size()) % m_matches.size();
    scrollToMatch(m_searchIndex);
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::clearSearch()
{
    if (m_searchQuery.isEmpty() && m_matches.isEmpty())
        return;
    m_searchQuery.clear();
    m_matches.clear();
    m_searchIndex = -1;
    emit searchChanged(0, 0);
    update();
}

// ─── hyperlinks ───────────────────────────────────────────────────────────────────────────────

// Soft-wrap join cap: a URL can legitimately run this many terminal rows before we give up growing
// it (good for very long URLs, bounds the cost of a hover/click — VTermLineInfo::continuation only
// exists for the LIVE screen, so a scrollback row never joins; see the file-level note in linkscan.h).
namespace { constexpr int kLinkJoinRowCap = 6; }

TerminalView::LinkSpan TerminalView::linkAt(const QPointF &p) const
{
    if (!m_term.isAttached())
        return {};
    const int row = int(p.y() / m_cellH);
    const int col = int(p.x() / m_cellW);
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return {};

    const int sbSize = m_term.sbCount();
    const int queriedAbsRow = sbSize - m_scrollOffset + row;
    const int liveRow = queriedAbsRow - sbSize; // >= 0 while this row is the live screen, not scrollback

    // Find the actual start of the (possibly wrapped) line: walk backward over LIVE rows flagged as
    // a continuation of the previous one. Scrollback rows never join (no VTermLineInfo there).
    int startAbsRow = queriedAbsRow;
    if (liveRow >= 0 && m_vt) {
        VTermState *state = vterm_obtain_state(m_vt);
        int steps = 0;
        int lr = liveRow;
        while (lr > 0 && steps < kLinkJoinRowCap) {
            const VTermLineInfo *info = vterm_state_get_lineinfo(state, lr);
            if (!info || !info->continuation)
                break;
            --lr;
            ++steps;
        }
        startAbsRow = queriedAbsRow - (liveRow - lr);
    }

    // Join forward from startAbsRow while each NEXT row is itself a continuation, capped. Every
    // row contributes exactly m_cols characters, so position <-> (row, col) is a simple divmod.
    QString joined;
    int rows = 1;
    joined += rowText(startAbsRow);
    if (liveRow >= 0 && m_vt) {
        VTermState *state = vterm_obtain_state(m_vt);
        int lr = startAbsRow - sbSize + 1;
        while (rows < kLinkJoinRowCap && lr < m_rows) {
            const VTermLineInfo *info = vterm_state_get_lineinfo(state, lr);
            if (!info || !info->continuation)
                break;
            joined += rowText(startAbsRow + rows);
            ++rows;
            ++lr;
        }
    }

    // ASCII-lossy on purpose (matches the renderer's own assumption — see rowText callers elsewhere):
    // a non-Latin1 cell becomes '?', which can never start or extend a scheme match.
    const std::string scan = joined.toLatin1().toStdString();
    const int queriedIdx = (queriedAbsRow - startAbsRow) * m_cols + col;
    for (const LinkMatch &m : scanLinks(scan)) {
        const int mStart = int(m.start), mEnd = int(m.end);
        if (queriedIdx < mStart || queriedIdx >= mEnd)
            continue;
        const int rowStartIdx = (queriedAbsRow - startAbsRow) * m_cols;
        const int rowEndIdx = rowStartIdx + m_cols;
        LinkSpan s;
        s.absRow = queriedAbsRow;
        s.c0 = qMax(mStart, rowStartIdx) - rowStartIdx;
        s.c1 = qMin(mEnd, rowEndIdx) - rowStartIdx - 1;
        s.url = joined.mid(mStart, mEnd - mStart);
        if (s.url.startsWith(QLatin1String("www.")))
            s.url.prepend(QLatin1String("http://"));
        s.valid = true;
        return s;
    }
    return {};
}

void TerminalView::hoverEnterEvent(QHoverEvent *event)
{
    emit hovered();
    hoverMoveEvent(event); // same cursor/link state as any hover position
}

void TerminalView::hoverMoveEvent(QHoverEvent *event)
{
    // MOVE mode (DECSET 1003) reports motion with NO button down too — that only ever reaches this
    // item as a hover (mouseMoveEvent is only delivered between a press and its release).
    const bool isReport = decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report;
    if (isReport)
        reportMouseMotion(event->position(), event->modifiers(), Qt::NoButton);
    // Links are a LOCAL thing (like selection): with mouse reporting active, they only apply when
    // Shift is held too — decideMouseAction already encodes that rule (Report wins unless Shift).
    const LinkSpan link = isReport ? LinkSpan{} : linkAt(event->position());
    setCursor(link.valid ? Qt::PointingHandCursor : Qt::IBeamCursor);
    if (link.valid != m_hoverLink.valid || link.absRow != m_hoverLink.absRow
        || link.c0 != m_hoverLink.c0 || link.c1 != m_hoverLink.c1) {
        m_hoverLink = link;
        emit hoverLinkChanged(link.valid ? link.url : QString());
        update();
    }
}

void TerminalView::hoverLeaveEvent(QHoverEvent *)
{
    unsetCursor();
    if (m_hoverLink.valid) {
        m_hoverLink = {};
        emit hoverLinkChanged(QString());
        update();
    }
}

// ─── mouse ────────────────────────────────────────────────────────────────────────────────────

namespace {
// Qt's modifier bits → libvterm's, for mouse reports (keyToVTerm does the same for the keyboard).
VTermModifier qtModsToVTerm(Qt::KeyboardModifiers m)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier) mod = VTermModifier(mod | VTERM_MOD_SHIFT);
    if (m & Qt::ControlModifier) mod = VTermModifier(mod | VTERM_MOD_CTRL);
    if (m & Qt::AltModifier) mod = VTermModifier(mod | VTERM_MOD_ALT);
    return mod;
}

// xterm convention: left/middle/right = 1/2/3 (libvterm encodes the actual X10/SGR byte); 0 = "not
// a button libvterm's mouse API knows about" (e.g. a fourth/back/forward mouse button).
int vtermButtonFor(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}
} // namespace

bool TerminalView::wantsMouseReport(int qtModifiers) const
{
    const bool shift = Qt::KeyboardModifiers(qtModifiers) & Qt::ShiftModifier;
    return decideMouseAction(m_mouseMode, m_altScreen, shift) == MouseAction::Report;
}

// xterm never reports a scrolled-back position: any reported event first snaps the view back to
// live, exactly like typing already does (see keyPressEvent).
void TerminalView::snapToLive()
{
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        emit scrollChanged();
        update();
    }
}

void TerminalView::reportMousePress(Qt::MouseButton button, const QPointF &pos, Qt::KeyboardModifiers mods)
{
    const int vb = vtermButtonFor(button);
    if (!m_vt || vb == 0)
        return;
    snapToLive();
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    vterm_mouse_move(m_vt, row, col, mod);
    vterm_mouse_button(m_vt, vb, true, mod);
    m_lastReportedCell = { row, col };
}

void TerminalView::reportMouseRelease(Qt::MouseButton button, const QPointF &pos, Qt::KeyboardModifiers mods)
{
    const int vb = vtermButtonFor(button);
    if (!m_vt || vb == 0)
        return;
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    vterm_mouse_move(m_vt, row, col, mod);
    vterm_mouse_button(m_vt, vb, false, mod);
    m_lastReportedCell = { row, col };
}

// Motion only reports in DRAG mode while a button is down, or unconditionally in MOVE mode (CLICK
// mode never reports motion at all — libvterm itself enforces that from m_mouseMode); either way,
// only on an actual cell change, so a touchpad doesn't flood the pty with same-cell reports.
void TerminalView::reportMouseMotion(const QPointF &pos, Qt::KeyboardModifiers mods, Qt::MouseButtons buttons)
{
    if (!m_vt)
        return;
    const bool wants = m_mouseMode == VTERM_PROP_MOUSE_MOVE
        || (m_mouseMode == VTERM_PROP_MOUSE_DRAG && buttons != Qt::NoButton);
    if (!wants)
        return;
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    if (row == m_lastReportedCell.row && col == m_lastReportedCell.col)
        return;
    m_lastReportedCell = { row, col };
    vterm_mouse_move(m_vt, row, col, qtModsToVTerm(mods));
}

// Wheel → xterm buttons 4 (up) / 5 (down), one PRESS per physical notch (no matching release — the
// wheel protocol only ever sends the press byte).
void TerminalView::reportWheel(const QPointF &pos, int angleDeltaY, Qt::KeyboardModifiers mods)
{
    const int notches = accumulateWheelNotches(m_wheelPending, angleDeltaY);
    if (!m_vt || notches == 0)
        return;
    snapToLive();
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    vterm_mouse_move(m_vt, row, col, mod);
    const int button = notches > 0 ? 4 : 5;
    for (int i = 0, n = qAbs(notches); i < n; ++i)
        vterm_mouse_button(m_vt, button, true, mod);
}

// Alternate-scroll (no mouse mode, altscreen app: less/man/git log): the wheel becomes cursor
// Up/Down keys, 3 per notch — the same "3 lines" feel as the ordinary scrollback wheel, and through
// vterm_keyboard_key so DECCKM (application cursor-key mode) picks the right escape (\eOA vs \e[A).
void TerminalView::sendAltScrollKeys(int angleDeltaY)
{
    const int notches = accumulateWheelNotches(m_wheelPending, angleDeltaY);
    if (!m_vt || notches == 0)
        return;
    const VTermKey key = notches > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN;
    for (int i = 0, n = qAbs(notches) * 3; i < n; ++i)
        vterm_keyboard_key(m_vt, key, VTERM_MOD_NONE);
}

void TerminalView::selectWordAt(const CellPos &c)
{
    const QString line = rowText(c.absRow);
    const auto isWord = [](QChar ch) {
        return ch.isLetterOrNumber() || QStringLiteral("._-~/:@%+=").contains(ch);
    };
    if (c.col >= line.size() || !isWord(line[c.col]))
        return;
    int a = c.col, b = c.col;
    while (a > 0 && isWord(line[a - 1])) --a;
    while (b + 1 < line.size() && isWord(line[b + 1])) ++b;
    m_selAnchor = { c.absRow, a };
    m_selEnd = { c.absRow, b };
    m_selValid = true;
    emit selectionChanged();
    update();
}

void TerminalView::selectLineAt(const CellPos &c)
{
    QString line = rowText(c.absRow);
    int end = line.size() - 1;
    while (end > 0 && line[end] == QLatin1Char(' ')) --end;
    m_selAnchor = { c.absRow, 0 };
    m_selEnd = { c.absRow, qMax(0, end) };
    m_selValid = true;
    emit selectionChanged();
    update();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Local only (same rule as selection/links generally — see hoverMoveEvent): with mouse reporting
    // active, this double-click would already have gone through mousePressEvent's Report branch for
    // BOTH of the two presses that make up a double-click, so reaching here at all already implies
    // Local — still, be explicit rather than relying on that.
    const bool isReport = decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report;
    const LinkSpan link = isReport ? LinkSpan{} : linkAt(event->position());
    if (link.valid) {
        // The whole URL, not a "word" — VTE/GNOME Terminal convention: double-click on a link
        // selects it entirely, ignoring selectWordAt's narrower word-separator rules (/:.?&=# etc.
        // are all valid URL characters that a plain word-select would stop at).
        m_selAnchor = { link.absRow, link.c0 };
        m_selEnd = { link.absRow, link.c1 };
        m_selValid = true;
        emit selectionChanged();
        update();
    } else {
        selectWordAt(cellAt(event->position())); // double-click → select the word
    }
    m_lastDblTime = event->timestamp();
    m_selecting = false;
    resumeOutputFromSelection(); // the first press of the pair already paused output; the pair's own
                                 // mouseReleaseEvent never runs (this handler replaces it) — resume here
    event->accept();
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    emit focusRequested(); // user intent: this pane is now the focused one (TerminalPanes updates m_focused)

    // A program that turned mouse tracking on gets the click reported (xterm/VTE behaviour) instead
    // of any of TerminalView's own button handling below — no paste, no selection, no local menu.
    // Right only ever reaches here via the QML <ContextMenu> declining the press (wantsMouseReport),
    // so this is always true for Right; Left/Middle can go either way.
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        reportMousePress(event->button(), event->position(), event->modifiers());
        event->accept();
        return;
    }
    stopAutoscroll(); // a fresh press always starts clean, even if a previous drag leaked one somehow
    resumeOutputFromSelection(); // ditto for a leaked pause (no-op when not paused)

    // Middle-click pastes the PRIMARY selection (X11/Wayland convention).
    if (event->button() == Qt::MiddleButton) {
        const QString sel = QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (!sel.isEmpty()) {
            if (sel.contains(QLatin1Char('\n')) || sel.contains(QLatin1Char('\r')))
                emit unsafePasteRequested(sel);
            else
                sendText(sel);
        }
        event->accept();
        return;
    }
    // Triple-click (a press shortly after a double-click) → select the whole line.
    if (event->button() == Qt::LeftButton && event->timestamp() - m_lastDblTime < 500) {
        m_lastDblTime = 0;
        selectLineAt(cellAt(event->position()));
        event->accept();
        return;
    }
    // Ctrl+click on an auto-detected link opens it (before starting a selection).
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        const LinkSpan link = linkAt(event->position());
        if (link.valid) {
            QDesktopServices::openUrl(QUrl::fromUserInput(link.url));
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        m_selAnchor = m_selEnd = cellAt(event->position());
        m_selecting = true;
        pauseOutputForSelection(); // STOP reading the pty for as long as this drag lasts
        if (m_selValid) {
            m_selValid = false;
            emit selectionChanged();
        }
        update();
    }
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        stopAutoscroll(); // defence in depth: a mode/shift flip mid-drag must not leave it running
        reportMouseMotion(event->position(), event->modifiers(), event->buttons());
        event->accept();
        return;
    }
    if (!m_selecting)
        return;
    // cellAt() clamps the row to [0, m_rows-1], so a pointer already past the edge extends the
    // selection to the currently-visible edge row immediately; updateAutoscroll (below) is what then
    // keeps revealing MORE of the scrollback/live screen without further movement.
    const CellPos p = cellAt(event->position());
    if (p.absRow != m_selEnd.absRow || p.col != m_selEnd.col) {
        m_selEnd = p;
        const bool valid = m_selEnd.absRow != m_selAnchor.absRow || m_selEnd.col != m_selAnchor.col;
        if (valid != m_selValid) {
            m_selValid = valid;
            emit selectionChanged();
        }
        update();
    }
    updateAutoscroll(event->position());
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        reportMouseRelease(event->button(), event->position(), event->modifiers());
        event->accept();
        return;
    }
    m_selecting = false;
    stopAutoscroll();
    resumeOutputFromSelection(); // drag is over — resume pty reads and drain whatever queued up
    // Copy-on-select to the PRIMARY selection (middle-click paste; doesn't clobber the clipboard).
    if (m_selValid) {
        const QString text = selectedText();
        if (!text.isEmpty())
            QGuiApplication::clipboard()->setText(text, QClipboard::Selection);
    }
    event->accept();
}

// Involuntary grab loss (e.g. an ancestor Flickable/ScrollView deciding to steal the mouse mid-drag):
// the only way a local selection drag can end WITHOUT mouseReleaseEvent ever firing, so this is the
// backstop that still resumes pty output rather than leaving it frozen.
void TerminalView::mouseUngrabEvent()
{
    QQuickItem::mouseUngrabEvent();
    if (m_selecting) {
        m_selecting = false;
        stopAutoscroll();
    }
    resumeOutputFromSelection();
}

// Called from mouseMoveEvent with every fresh LOCAL-selection position: starts the timer the first
// time the pointer sits past an edge, keeps it aimed at the latest position otherwise, and stops it
// the moment the pointer is back inside the view (re-entry — see the file header in autoscroll.h).
void TerminalView::updateAutoscroll(const QPointF &rawPos)
{
    m_autoscrollPos = rawPos;
    if (edgeOvershoot(rawPos.y(), height()) == 0) {
        stopAutoscroll();
        return;
    }
    if (!m_autoscrollTimer) {
        m_autoscrollTimer = new QTimer(this);
        m_autoscrollTimer->setInterval(kAutoscrollTimerMs);
        connect(m_autoscrollTimer, &QTimer::timeout, this, &TerminalView::autoscrollTick);
    }
    if (!m_autoscrollTimer->isActive())
        m_autoscrollTimer->start();
}

void TerminalView::stopAutoscroll()
{
    if (m_autoscrollTimer)
        m_autoscrollTimer->stop();
}

// STOP reading the pty master while the local selection drag this call corresponds to is live (see
// ptysession.h). Idempotent — safe to call speculatively from a defensive reset site.
void TerminalView::pauseOutputForSelection()
{
    if (m_outputPaused)
        return;
    m_outputPaused = true;
    m_pty.pause();
    emit outputPausedChanged();
}

// Resume reading. Safe to call even when not currently paused (every drag-ending site calls this
// unconditionally rather than tracking "did I pause").
void TerminalView::resumeOutputFromSelection()
{
    if (!m_outputPaused)
        return;
    m_outputPaused = false;
    m_pty.resume();
    emit outputPausedChanged();
}

// One tick: scroll toward whichever edge the LAST known pointer position (m_autoscrollPos) sits past,
// then re-pin the selection's moving end to that edge row at the NEW scroll offset — exactly what a
// real mouseMoveEvent would have done had the content scrolled under a stationary pointer. No fresh
// QMouseEvent is needed for this to keep going, by design (see autoscroll.h).
void TerminalView::autoscrollTick()
{
    const qreal overshoot = edgeOvershoot(m_autoscrollPos.y(), height());
    if (!m_selecting || overshoot == 0) { // pointer re-entered or selection ended between ticks
        stopAutoscroll();
        return;
    }
    const int lines = autoscrollLinesPerTick(qAbs(overshoot), m_cellH);
    if (lines <= 0)
        return;
    const bool above = overshoot < 0;
    m_scrollOffset = autoscrollOffsetAfterTick(m_scrollOffset, m_term.sbCount(), lines, above);
    emit scrollChanged();

    const int edgeRow = above ? 0 : m_rows - 1;
    const CellPos edge = cellAt(QPointF(m_autoscrollPos.x(), edgeRow * m_cellH));
    if (edge.absRow != m_selEnd.absRow || edge.col != m_selEnd.col) {
        m_selEnd = edge;
        const bool valid = m_selEnd.absRow != m_selAnchor.absRow || m_selEnd.col != m_selAnchor.col;
        if (valid != m_selValid) {
            m_selValid = valid;
            emit selectionChanged();
        }
    }
    update();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    // Ctrl+wheel zooms the font (like Ctrl +/−), instead of scrolling or reporting — takes
    // precedence over everything else, exactly as today.
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() != 0)
            emit zoomRequested(event->angleDelta().y() > 0 ? 1 : -1);
        event->accept();
        return;
    }

    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (decideMouseAction(m_mouseMode, m_altScreen, shift)) {
    case MouseAction::Report:
        reportWheel(event->position(), event->angleDelta().y(), event->modifiers());
        event->accept();
        return;
    case MouseAction::AltScroll:
        sendAltScrollKeys(event->angleDelta().y());
        event->accept();
        return;
    case MouseAction::Local:
        break;
    }

    // Local (today's behaviour): scroll SolidTerm's own scrollback.
    const int lines = event->angleDelta().y() / 40; // 3 lines per notch
    m_scrollOffset = qBound(0, m_scrollOffset + lines, m_term.sbCount());
    emit scrollChanged();
    update();
    event->accept();
}

// ─── property surface ───────────────────────────────────────────────────────────────────────────

void TerminalView::setBackgroundImage(const QString &path)
{
    if (m_bgImagePath == path)
        return;
    m_bgImagePath = path;
    m_bgImage = QImage();
    if (!path.isEmpty()) {
        QString p = path;
        if (p.startsWith(QLatin1String("file://")))
            p = QUrl(p).toLocalFile();
        m_bgImageSource.load(p);
    } else {
        m_bgImageSource = QImage();
    }
    rebuildBackgroundImage();
    emit decorChanged();
    update();
}

namespace {

// One box-blur pass of radius r along rows (horizontal) or columns, with a sliding sum — O(pixels),
// independent of r. Premultiplied ARGB, so translucent pixels do not bleed dark fringes.
void boxBlurPass(QImage &img, int r, bool horizontal)
{
    const int w = img.width(), h = img.height();
    const int len = horizontal ? w : h, lines = horizontal ? h : w;
    const qsizetype stride = img.bytesPerLine() / 4;
    QRgb *base = reinterpret_cast<QRgb *>(img.bits());
    const qsizetype step = horizontal ? 1 : stride;
    std::vector<QRgb> line(static_cast<size_t>(len));
    const int win = 2 * r + 1;
    for (int l = 0; l < lines; ++l) {
        QRgb *p = horizontal ? base + l * stride : base + l;
        for (int i = 0; i < len; ++i)
            line[size_t(i)] = p[i * step];
        // Edge pixels are repeated (clamp), so the border does not darken.
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; ++i) {
            const QRgb c = line[size_t(qBound(0, i, len - 1))];
            sa += qAlpha(c); sr += qRed(c); sg += qGreen(c); sb += qBlue(c);
        }
        for (int i = 0; i < len; ++i) {
            p[i * step] = qRgba(sr / win, sg / win, sb / win, sa / win);
            const QRgb out = line[size_t(qBound(0, i - r, len - 1))];
            const QRgb in = line[size_t(qBound(0, i + r + 1, len - 1))];
            sa += qAlpha(in) - qAlpha(out); sr += qRed(in) - qRed(out);
            sg += qGreen(in) - qGreen(out); sb += qBlue(in) - qBlue(out);
        }
    }
}

} // namespace

// Blur over the BACKGROUND image: three box passes each way ≈ a gaussian. Applied ONCE per change
// (not per frame) and never to the glyphs, so text stays crisp. The image is first reduced — a blur
// discards detail anyway, so a smaller texture loses nothing and keeps a large radius cheap; the
// cover-fit draw scales it back up smoothly.
static QImage blurImage(const QImage &src, int strength)
{
    const qreal k = qBound(0, strength, 100) / 100.0;
    constexpr int kWorkSize = 960;   // longest side the blur runs at
    constexpr qreal kMaxRadius = 32; // at full strength, in work-size pixels (~3% of the width)
    QImage img = src.scaled(QSize(kWorkSize, kWorkSize), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int r = qMax(1, int(kMaxRadius * k));
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(img, r, true);
        boxBlurPass(img, r, false);
    }
    return img;
}

void TerminalView::rebuildBackgroundImage()
{
    m_bgImage = (m_blur > 0 && !m_bgImageSource.isNull())
        ? blurImage(m_bgImageSource, m_blur)
        : m_bgImageSource;
    m_bgImageDirty = true;
}

void TerminalView::setBackgroundOpacity(qreal v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(m_bgOpacity, v))
        return;
    m_bgOpacity = v;
    emit decorChanged();
    update();
}

void TerminalView::setBlur(int v)
{
    v = qBound(0, v, 100);
    if (m_blur == v)
        return;
    m_blur = v;
    rebuildBackgroundImage(); // the effect lives on the background image, not on the glyphs
    emit decorChanged();
    update();
}

void TerminalView::setScrollbarMode(const QString &v)
{
    if (m_scrollbarMode == v)
        return;
    m_scrollbarMode = v;
    emit scrollbarModeChanged();
}

void TerminalView::scrollTo(int offset)
{
    const int clamped = clampOffset(offset, m_term.sbCount());
    if (clamped == m_scrollOffset)
        return;
    m_scrollOffset = clamped;
    emit scrollChanged();
    update();
}

// Composes the desktop scrollbar — same look as QmlCss::composeScrollBar (vendor/qml-css-engine's
// cssscroll.h: 12px track, 6px rounded thumb, the same three alpha-black thumb states) but bound to
// scrollOffset/scrollbackCount/viewportRows instead of a Flickable's contentY/contentHeight, since
// TerminalView paints its own grid rather than hosting content in one — "a thin custom bar styled
// from the same CSS hooks" (owner), not a reuse of composeScrollBar itself (its required `flick`
// interface — contentY, contentHeight, visibleArea.heightRatio — has no TerminalView equivalent).
// Lazy + idempotent: called from ensureStarted(), which only has a QQmlEngine once this item is
// actually in a QML scene (C++-created panes go through TerminalPanes' QQmlComponent, so this is
// never null in practice — see TerminalPanes::makeLeaf).
void TerminalView::composeScrollBar()
{
    if (m_scrollBar)
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("solidterm-scroll-bar"), R"(
import QtQuick
Rectangle {
    id: sbTrack
    required property var view
    objectName: "termScrollTrack"
    z: 1000
    x: view.width - width
    y: 0
    width: 12
    height: view.height
    color: "transparent"
    readonly property int sbSize: view.scrollbackCount
    readonly property int rowCount: view.viewportRows
    readonly property bool hasScrollback: sbSize > 0 && !view.altScreen
    property bool dragging: false
    property bool hoveredBar: false
    property bool pulseOn: false
    function pulse() { pulseOn = true; fadeTimer.restart() }
    Timer { id: fadeTimer; interval: 1000; onTriggered: sbTrack.pulseOn = false }
    Connections { target: view; function onScrollChanged() { sbTrack.pulse() } }
    readonly property bool shown: view.scrollbarMode === "always" ? true
        : view.scrollbarMode === "never" ? false
        : (pulseOn || dragging || hoveredBar || view.outputPaused)
    // A paused drag always shows the bar — even with no scrollback yet — so there's some minimal,
    // theme-able feedback that output is frozen (owner: optional "subtle paused tint").
    visible: (hasScrollback && opacity > 0.01) || view.outputPaused
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }
    HoverHandler {
        onHoveredChanged: { sbTrack.hoveredBar = hovered; if (hovered) sbTrack.pulse() }
    }
    WheelHandler {
        onWheel: (ev) => view.scrollTo(view.scrollOffset + Math.round(ev.angleDelta.y / 40))
    }
    MouseArea {
        anchors.fill: parent
        onPressed: (m) => {
            sbTrack.pulse()
            var page = Math.max(1, Math.round(sbTrack.rowCount * 0.9))
            if (m.y < sbThumb.y) view.scrollTo(view.scrollOffset + page)
            else if (m.y > sbThumb.y + sbThumb.height) view.scrollTo(view.scrollOffset - page)
        }
    }
    Rectangle {
        id: sbThumb
        objectName: "termScrollThumb"
        x: 3
        width: 6
        radius: 3
        // Paused tint (amber) wins over the normal hover/pressed grey states — minimal, theme-able
        // feedback that a local selection drag has frozen pty reads (owner: subtle, not alarming).
        color: view.outputPaused ? "#99f59e0b"
            : thumbArea.pressed ? "#99000000" : (thumbArea.containsMouse ? "#77000000" : "#55000000")
        height: Math.min(sbTrack.height, Math.max(24,
            (sbTrack.rowCount / Math.max(1, sbTrack.sbSize + sbTrack.rowCount)) * sbTrack.height))
        y: {
            if (sbTrack.sbSize <= 0) return sbTrack.height - height
            var fracTop = Math.max(0, Math.min(1, (sbTrack.sbSize - view.scrollOffset) / sbTrack.sbSize))
            return fracTop * (sbTrack.height - height)
        }
        MouseArea {
            id: thumbArea
            objectName: "termScrollThumbArea"
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            property real __grab: 0
            onPressed: (m) => { __grab = m.y; sbTrack.dragging = true; sbTrack.pulse() }
            onReleased: sbTrack.dragging = false
            onPositionChanged: (m) => {
                if (!pressed) return
                var maxY = sbTrack.height - sbThumb.height
                if (maxY <= 0) return
                var ny = sbThumb.y + (m.y - __grab)
                var fracTop = Math.max(0, Math.min(ny / maxY, 1))
                view.scrollTo(Math.round((1 - fracTop) * sbTrack.sbSize))
            }
        }
    }
}
)");
    auto *bar = qobject_cast<QQuickItem *>(comp->createWithInitialProperties(
        {{QStringLiteral("view"), QVariant::fromValue<QObject *>(this)}}, qmlContext(this)));
    if (!bar) {
        qWarning("solidterm: failed to compose the scrollbar: %s", qPrintable(comp->errorString()));
        return;
    }
    bar->setParent(this);
    bar->setParentItem(this);
    m_scrollBar = bar;
}

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

QString TerminalView::currentDirectory() const
{
    const qint64 pid = m_pty.pid();
    if (pid <= 0)
        return {};
    const QString cwd = QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
    return QFileInfo(cwd).isDir() ? cwd : QString();
}

void TerminalView::setAnsiPalette(const QStringList &names)
{
    if (m_paletteNames == names)
        return;
    m_paletteNames = names;
    // All-or-nothing: a partial or unparsable list falls back to the xterm defaults entirely.
    m_hasPalette = names.size() == 16;
    for (int i = 0; m_hasPalette && i < 16; ++i) {
        m_palette[i] = QColor::fromString(names.at(i));
        m_hasPalette = m_palette[i].isValid();
    }
    emit colorsChanged();
    update();
}

void TerminalView::setReservedSequences(const QStringList &v)
{
    if (m_reserved == v)
        return;
    m_reserved = v;
    emit reservedSequencesChanged();
}

void TerminalView::setScrollbackLimit(int v)
{
    if (m_scrollbackLimit == v)
        return;
    m_scrollbackLimit = qMax(0, v);
    m_term.setScrollbackLimit(m_scrollbackLimit); // a lower limit trims at once (cbSbPopped clamps the offset)
    emit scrollbackLimitChanged();
}

void TerminalView::sendText(const QString &text)
{
    if (!m_vt || m_readOnly)
        return;
    const auto ucs4 = text.toUcs4();
    for (const char32_t c : ucs4)
        vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
}
