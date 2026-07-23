#include "terminalview.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QRegularExpression>
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

struct GlyphVertex { float x, y, u, v, r, g, b, a; };

const QSGGeometry::Attribute kGlyphAttrs[] = {
    QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),  // pos
    QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType, false), // uv
    QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType, false), // color
};
const QSGGeometry::AttributeSet kGlyphAttrSet = { 3, sizeof(GlyphVertex), kGlyphAttrs };

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
    setAcceptedMouseButtons(Qt::LeftButton); // right passes to the solid <ContextMenu> above
    setActiveFocusOnTab(true);
    setAcceptHoverEvents(true);               // hover to detect/underline links
    setFlag(ItemHasContents, true);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(530);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        if (!hasActiveFocus() || !m_cursorVisible || m_scrollOffset > 0)
            return;
        m_blinkOn = !m_blinkOn;
        update();
    });
    m_blinkTimer->start();
    connect(this, &QQuickItem::activeFocusChanged, this, [this] { resetBlink(); });

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
    QQuickItem::componentComplete();
    ensureStarted();
}

void TerminalView::ensureStarted()
{
    applyGrid();
    ensureSession();
}

void TerminalView::applyGrid()
{
    // The grid uses the GlyphCache's (integer) cell metrics, so every cell lands exactly on an
    // atlas tile — glyphs stay crisp (no fractional placement). setFont is a no-op when unchanged.
    m_glyphs.setFont(m_font.family(), m_font.pixelSize());
    m_cellW = m_glyphs.cellWidth();
    m_cellH = m_glyphs.cellHeight();
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
    vterm_screen_convert_color_to_rgb(m_screen, &c);
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
    const uchar r = uchar(c.red()), g = uchar(c.green()), b = uchar(c.blue()), a = uchar(c.alpha());
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    QSGGeometry::ColoredPoint2D tl, tr, bl, br;
    tl.set(x0, y0, r, g, b, a);
    tr.set(x1, y0, r, g, b, a);
    bl.set(x0, y1, r, g, b, a);
    br.set(x1, y1, r, g, b, a);
    v.insert(v.end(), { tl, tr, bl, bl, tr, br }); // two triangles
}

inline void pushGlyphQuad(std::vector<GlyphVertex> &v, qreal x, qreal y, qreal w, qreal h,
                          const QRectF &uv, const QColor &c)
{
    // The glyph fragment shader is premultiplied (vColor × coverage), so premultiply here — lets the
    // emboss shadow/highlight copies carry alpha < 1 correctly (opaque fg keeps alpha 1, unchanged).
    const float al = float(c.alphaF());
    const float r = float(c.redF()) * al, g = float(c.greenF()) * al, b = float(c.blueF()) * al, a = al;
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    const float u0 = float(uv.left()), v0 = float(uv.top());
    const float u1 = float(uv.right()), v1 = float(uv.bottom());
    const GlyphVertex tl{ x0, y0, u0, v0, r, g, b, a };
    const GlyphVertex tr{ x1, y0, u1, v0, r, g, b, a };
    const GlyphVertex bl{ x0, y1, u0, v1, r, g, b, a };
    const GlyphVertex br{ x1, y1, u1, v1, r, g, b, a };
    v.insert(v.end(), { tl, tr, bl, bl, tr, br });
}

} // namespace

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_screen || width() <= 0 || height() <= 0) {
        delete oldNode;
        m_imageNode = nullptr;
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
        return nullptr;
    }

    std::vector<QSGGeometry::ColoredPoint2D> bg;
    std::vector<GlyphVertex> glyphs;
    std::vector<QSGGeometry::ColoredPoint2D> cursor;

    // Base background fills the whole item. Its alpha = backgroundOpacity, so at <1 the layer behind
    // (a bg image, or — with transparent chrome — the desktop) shows through the terminal's colour.
    QColor baseBg = m_background;
    baseBg.setAlphaF(m_bgOpacity);
    pushBgQuad(bg, 0, 0, width(), height(), baseBg);

    const int sbShown = m_scrollOffset;
    for (int row = 0; row < m_rows; ++row) {
        const int liveRow = row - sbShown;
        const VTermScreenCell *sbCells = nullptr;
        int sbCols = 0;
        if (liveRow < 0) {
            const int idx = m_scrollback.size() + liveRow;
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
            const qreal x = col * m_cellW;
            const qreal w = cw * m_cellW;

            QColor fg = toQColor(cell.fg, true);
            QColor bgc = toQColor(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bgc);
            if (bgc != m_background)
                pushBgQuad(bg, x, y, w, m_cellH, bgc);

            // One textured quad per non-blank cell — the atlas makes per-cell quads cheap, so the
            // old run-merge (a QPainter optimisation) is gone.
            QString cluster;
            for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
                cluster.append(QString::fromUcs4(&cell.chars[ci], 1));
            if (!cluster.isEmpty() && cluster != QLatin1String(" ")) {
                const GlyphCache::Entry e = m_glyphs.glyph(cluster, cell.attrs.bold, cell.attrs.italic, cw);
                if (e.valid) {
                    // Emboss: a dark copy down-right + a light copy up-left behind the glyph → engraved.
                    if (m_emboss) {
                        pushGlyphQuad(glyphs, x + 1, y + 1, w, m_cellH, e.uv, QColor(0, 0, 0, 150));
                        pushGlyphQuad(glyphs, x - 1, y - 1, w, m_cellH, e.uv, QColor(255, 255, 255, 80));
                    }
                    pushGlyphQuad(glyphs, x, y, w, m_cellH, e.uv, fg);
                }
            }
            if (cell.attrs.underline)
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
            const int absRow = int(m_scrollback.size()) - sbShown + row;
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

    // Search highlights: every match on a visible row (current match brighter).
    if (!m_matches.isEmpty()) {
        const int top = int(m_scrollback.size()) - sbShown;
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
        const int row = m_hoverLink.absRow - (int(m_scrollback.size()) - sbShown);
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

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (!m_vt) {
        QQuickItem::keyPressEvent(event);
        return;
    }
    // Reserved accelerators (split/close/focus … from the solid config) are consumed HERE, before
    // the pty — so they never leak to the shell and never hit Qt's ambiguous Shortcut map.
    if (!m_reserved.isEmpty()) {
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
    // Typing snaps back to live output.
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        update();
    }
    resetBlink();
    keyToVTerm(event);
    event->accept();
}

TerminalView::CellPos TerminalView::cellAt(const QPointF &p) const
{
    CellPos c;
    const int row = qBound(0, int(p.y() / m_cellH), m_rows - 1);
    c.absRow = m_scrollback.size() - m_scrollOffset + row;
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
        for (int col = c0; col <= c1; ++col) {
            VTermScreenCell cell {};
            if (absRow < m_scrollback.size()) {
                const auto &cells = m_scrollback.at(absRow).cells;
                if (col < cells.size())
                    cell = cells.at(col);
            } else {
                const int liveRow = absRow - int(m_scrollback.size());
                if (liveRow >= m_rows)
                    break;
                vterm_screen_get_cell(m_screen, VTermPos{ liveRow, col }, &cell);
            }
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

void TerminalView::pasteClipboard()
{
    sendText(QGuiApplication::clipboard()->text());
}

void TerminalView::clearScrollback()
{
    m_scrollback.clear();
    m_scrollOffset = 0;
    m_selValid = false;
    emit selectionChanged();
    update();
}

void TerminalView::resetBlink()
{
    m_blinkOn = true;
    if (m_blinkTimer)
        m_blinkTimer->start(); // restart the phase so the cursor stays solid right after activity
    update();
}

// ─── search ─────────────────────────────────────────────────────────────────────────────────────

QString TerminalView::rowText(int absRow) const
{
    QString line(m_cols, QLatin1Char(' '));
    if (!m_screen || absRow < 0)
        return line;
    const int sbSize = m_scrollback.size();
    if (absRow < sbSize) {
        const auto &cells = m_scrollback.at(absRow).cells;
        for (int c = 0; c < m_cols && c < cells.size(); ++c)
            if (cells[c].chars[0])
                line[c] = QChar(char32_t(cells[c].chars[0]));
    } else {
        const int liveRow = absRow - sbSize;
        if (liveRow >= m_rows)
            return line;
        for (int c = 0; c < m_cols; ++c) {
            VTermScreenCell cell {};
            vterm_screen_get_cell(m_screen, VTermPos{ liveRow, c }, &cell);
            if (cell.chars[0])
                line[c] = QChar(char32_t(cell.chars[0]));
        }
    }
    return line;
}

void TerminalView::search(const QString &query)
{
    m_searchQuery = query;
    m_matches.clear();
    m_searchIndex = -1;
    if (!query.isEmpty() && m_screen) {
        const int total = m_scrollback.size() + m_rows;
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
    const int sbSize = m_scrollback.size();
    const int desiredTop = m_matches[i].absRow - m_rows / 2; // centre the match
    m_scrollOffset = qBound(0, sbSize - desiredTop, sbSize);
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

TerminalView::LinkSpan TerminalView::linkAt(const QPointF &p) const
{
    if (!m_screen)
        return {};
    const int row = int(p.y() / m_cellH);
    const int col = int(p.x() / m_cellW);
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return {};

    // Reconstruct the row as a col-indexed string (URLs are ASCII, so the first char per cell is
    // enough), from the same source the renderer uses (scrollback when scrolled, else live).
    const int sbShown = m_scrollOffset;
    const int liveRow = row - sbShown;
    const VTermScreenCell *sbCells = nullptr;
    int sbCols = 0;
    if (liveRow < 0) {
        const int idx = m_scrollback.size() + liveRow;
        if (idx < 0)
            return {};
        sbCells = m_scrollback.at(idx).cells.constData();
        sbCols = int(m_scrollback.at(idx).cells.size());
    }
    QString line(m_cols, QLatin1Char(' '));
    for (int c = 0; c < m_cols; ++c) {
        VTermScreenCell cell {};
        if (sbCells) { if (c < sbCols) cell = sbCells[c]; }
        else vterm_screen_get_cell(m_screen, VTermPos{ liveRow, c }, &cell);
        if (cell.chars[0])
            line[c] = QChar(char32_t(cell.chars[0]));
    }

    static const QRegularExpression re(
        QStringLiteral(R"((?:https?|ftp|file)://[^\s]+|www\.[^\s]+)"));
    auto it = re.globalMatch(line);
    while (it.hasNext()) {
        const auto m = it.next();
        int start = m.capturedStart();
        int end = m.capturedEnd(); // exclusive
        while (end > start && QStringLiteral(").,;:!?\"'>").contains(line[end - 1]))
            --end; // trailing punctuation isn't part of the URL
        if (col >= start && col < end) {
            LinkSpan s;
            s.absRow = int(m_scrollback.size()) - sbShown + row;
            s.c0 = start;
            s.c1 = end - 1;
            s.url = line.mid(start, end - start);
            if (s.url.startsWith(QLatin1String("www.")))
                s.url.prepend(QLatin1String("http://"));
            s.valid = true;
            return s;
        }
    }
    return {};
}

void TerminalView::hoverMoveEvent(QHoverEvent *event)
{
    const LinkSpan link = linkAt(event->position());
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    setCursor(link.valid && ctrl ? Qt::PointingHandCursor : Qt::IBeamCursor);
    if (link.valid != m_hoverLink.valid || link.absRow != m_hoverLink.absRow
        || link.c0 != m_hoverLink.c0 || link.c1 != m_hoverLink.c1) {
        m_hoverLink = link;
        update();
    }
}

void TerminalView::hoverLeaveEvent(QHoverEvent *)
{
    unsetCursor();
    if (m_hoverLink.valid) {
        m_hoverLink = {};
        update();
    }
}

// ─── mouse ────────────────────────────────────────────────────────────────────────────────────

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
    selectWordAt(cellAt(event->position())); // double-click → select the word
    m_lastDblTime = event->timestamp();
    m_selecting = false;
    event->accept();
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
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
    if (!m_selecting)
        return;
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
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    m_selecting = false;
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
        m_bgImage.load(p);
    }
    m_bgImageDirty = true;
    emit decorChanged();
    update();
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

void TerminalView::setEmboss(bool v)
{
    if (m_emboss == v)
        return;
    m_emboss = v;
    emit decorChanged();
    update();
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
