#include "terminalview.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QSGVertexColorMaterial>

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
    setFlag(ItemHasContents, true);

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
    const float r = float(c.redF()), g = float(c.greenF()), b = float(c.blueF()), a = 1.0f;
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    const float u0 = float(uv.left()), v0 = float(uv.top());
    const float u1 = float(uv.right()), v1 = float(uv.bottom());
    const GlyphVertex tl{ x0, y0, u0, v0, r, g, b, a };
    const GlyphVertex tr{ x1, y0, u1, v0, r, g, b, a };
    const GlyphVertex bl{ x0, y1, u0, v1, r, g, b, a };
    const GlyphVertex br{ x1, y1, u1, v1, r, g, b, a };
    v.insert(v.end(), { tl, tr, bl, bl, tr, br });
}

QSGGeometryNode *takeChild(QSGNode *root, int index)
{
    return index < root->childCount() ? static_cast<QSGGeometryNode *>(root->childAtIndex(index)) : nullptr;
}

} // namespace

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_screen || width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }

    std::vector<QSGGeometry::ColoredPoint2D> bg;
    std::vector<GlyphVertex> glyphs;
    std::vector<QSGGeometry::ColoredPoint2D> cursor;

    // Base background fills the whole item (so gaps between cells and the last partial row are clean).
    pushBgQuad(bg, 0, 0, width(), height(), m_background);

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
                if (e.valid)
                    pushGlyphQuad(glyphs, x, y, w, m_cellH, e.uv, fg);
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
            QColor cur = m_foreground;
            cur.setAlpha(140);
            pushBgQuad(cursor, cx, cy, m_cellW, m_cellH, cur);
        } else {
            pushBgQuad(cursor, cx, cy, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy + m_cellH - 1, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy, 1, m_cellH, m_foreground);
            pushBgQuad(cursor, cx + m_cellW - 1, cy, 1, m_cellH, m_foreground);
        }
    }

    // If the atlas overflowed mid-build (astronomically unlikely), reset it and ask the GUI thread
    // (queued — we're on the render thread here) to re-render with a fresh atlas.
    if (m_glyphs.overflowed()) {
        m_glyphs.reset();
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }

    QSGNode *root = oldNode;
    if (!root)
        root = new QSGNode;

    // Child 0: backgrounds + selection (vertex colour).
    QSGGeometryNode *bgNode = takeChild(root, 0);
    if (!bgNode) {
        bgNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        bgNode->setGeometry(geo);
        bgNode->setFlag(QSGNode::OwnsGeometry);
        auto *mat = new QSGVertexColorMaterial;
        bgNode->setMaterial(mat);
        bgNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(bgNode);
    }
    {
        QSGGeometry *geo = bgNode->geometry();
        geo->allocate(int(bg.size()));
        memcpy(geo->vertexData(), bg.data(), bg.size() * sizeof(QSGGeometry::ColoredPoint2D));
        bgNode->markDirty(QSGNode::DirtyGeometry);
    }

    // Child 1: glyphs (atlas material). Rebuild the texture only when the atlas changed.
    QSGGeometryNode *glyphNode = takeChild(root, 1);
    if (!glyphNode) {
        glyphNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(kGlyphAttrSet, 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        glyphNode->setGeometry(geo);
        glyphNode->setFlag(QSGNode::OwnsGeometry);
        auto *mat = new GlyphMaterial;
        glyphNode->setMaterial(mat);
        glyphNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(glyphNode);
    }
    auto *glyphMat = static_cast<GlyphMaterial *>(glyphNode->material());
    if (m_glyphs.takeDirty())
        glyphMat->setTexture(window()->createTextureFromImage(m_glyphs.atlas()));
    {
        QSGGeometry *geo = glyphNode->geometry();
        geo->allocate(int(glyphs.size()));
        if (!glyphs.empty())
            memcpy(geo->vertexData(), glyphs.data(), glyphs.size() * sizeof(GlyphVertex));
        glyphNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    // Child 2: cursor (vertex colour, on top).
    QSGGeometryNode *cursorNode = takeChild(root, 2);
    if (!cursorNode) {
        cursorNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        cursorNode->setGeometry(geo);
        cursorNode->setFlag(QSGNode::OwnsGeometry);
        auto *mat = new QSGVertexColorMaterial;
        cursorNode->setMaterial(mat);
        cursorNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(cursorNode);
    }
    {
        QSGGeometry *geo = cursorNode->geometry();
        geo->allocate(int(cursor.size()));
        if (!cursor.empty())
            memcpy(geo->vertexData(), cursor.data(), cursor.size() * sizeof(QSGGeometry::ColoredPoint2D));
        cursorNode->markDirty(QSGNode::DirtyGeometry);
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

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
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
