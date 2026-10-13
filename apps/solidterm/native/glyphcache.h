#pragma once

#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QRectF>
#include <QSGTexture>
#include <QSize>
#include <utility>

class QRhi;
class QRhiResourceUpdateBatch;
class QRhiTexture;

// GPU glyph atlas for the terminal (owner: "as fast as alacritty"). Each distinct
// (cluster, bold, italic, width-in-cells) is rasterised ONCE into a cell-sized tile and packed
// into a PREMULTIPLIED RGBA atlas; the terminal then draws the whole grid as textured quads in one
// scene-graph node (GPU-composited) instead of re-rasterising with QPainter every frame. Regular
// glyphs are white coverage (the shader tints them by the cell's fg colour); colour glyphs (emoji)
// keep their own colours (Entry::color → the shader uses the texture as-is). Grid-perfect: every
// tile is exactly cellW×cellH (×2 for wide chars), so cells align without per-glyph bearing math.
class GlyphCache {
public:
    struct Entry {
        QRectF uv;        // normalised atlas coordinates
        int widthCells = 1;
        bool color = false; // a colour glyph (emoji) → use the texture colour, don't tint by fg
        bool valid = false;
    };

    // `pixelSize` is in LOGICAL pixels; `dpr` is the window's device pixel ratio. Glyphs are
    // rasterised at physical resolution (pixelSize × dpr) so the atlas maps 1:1 onto the screen —
    // rasterising at 1× and letting the GPU scale it up blurred every glyph on a HiDPI output.
    void setFont(const QString &family, int pixelSize, qreal dpr = 1.0);
    // Cell metrics in LOGICAL pixels (a whole number of PHYSICAL pixels, so cells stay pixel-aligned).
    qreal cellWidth() const { return m_cellW / m_dpr; }
    qreal cellHeight() const { return m_cellH / m_dpr; }
    // The cell metrics setFont() would give (logical px), without building an atlas: the tmux mode
    // sizes the session in cells before any view exists (docs/solidterm-tmux-control-mode.md 2.3).
    static void cellMetrics(const QString &family, int pixelSize, qreal dpr, qreal *cellW, qreal *cellH);

    // Ensure a glyph is in the atlas; returns its atlas UV rect. `cluster` is the cell's text
    // (usually one char; combining marks / wide chars form a longer cluster).
    Entry glyph(const QString &cluster, bool bold, bool italic, int widthCells);
    // Same, for a cell holding ONE codepoint (nearly every cell): no QString is built unless the
    // glyph is not cached yet. Printable ASCII hits a flat array; the rest a 64-bit-keyed hash.
    Entry glyph(char32_t cp, bool bold, bool italic, int widthCells)
    {
        const quint8 style = quint8((bold ? 1 : 0) | (italic ? 2 : 0));
        if (cp < 128 && widthCells <= 1) {
            Entry &e = m_ascii[style][cp];
            if (!e.valid)
                e = glyph(QString(QChar(char16_t(cp))), bold, italic, 1);
            return e;
        }
        const quint64 key = quint64(cp) | (quint64(style) << 32) | (quint64(qMax(1, widthCells)) << 40);
        const auto it = m_cpCache.constFind(key);
        if (it != m_cpCache.constEnd())
            return it.value();
        const Entry e = glyph(QString::fromUcs4(&cp, 1), bold, italic, widthCells);
        if (e.valid)
            m_cpCache.insert(key, e);
        return e;
    }

    const QImage &atlas() const { return m_atlas; }
    // Atlas rectangles rasterised since the last call, already coalesced (consecutive glyphs
    // packed onto the same shelf row merge into one rect). Empty when nothing changed. reset()
    // reports one rect covering the whole atlas, so a fresh GlyphCache (or one just rebuilt after
    // a font change / overflow) naturally asks its caller for a full re-upload — no separate
    // "full" flag needed. The caller (GlyphAtlasTexture) uploads exactly these sub-rectangles
    // instead of re-uploading the whole fixed 2048x2048 atlas (~16 MB) every time.
    QList<QRect> takeDirtyRects() { QList<QRect> r = std::move(m_dirtyRects); m_dirtyRects.clear(); return r; }
    // The atlas is a fixed size (UVs must not shift mid-frame). If it ever fills, glyph() returns
    // invalid entries and sets this; the caller reset()s and rebuilds on the NEXT frame.
    bool overflowed() const { return m_overflow; }
    void reset();

private:
    struct Key {
        QString cluster;
        quint8 style; // bit0 bold, bit1 italic
        int widthCells;
        bool operator==(const Key &o) const
        { return style == o.style && widthCells == o.widthCells && cluster == o.cluster; }
    };
    friend size_t qHash(const GlyphCache::Key &k, size_t seed);

    Entry rasterize(const Key &k);
    void markDirty(const QRect &tile);

    QFont m_font, m_bold, m_italic, m_boldItalic;
    qreal m_cellW = 8, m_cellH = 16, m_ascent = 12; // PHYSICAL pixels (atlas tile size, baseline)
    qreal m_dpr = 1.0;
    int m_logicalPx = 0;
    QImage m_atlas;
    int m_penX = 0, m_penY = 0, m_rowH = 0; // shelf packer
    QHash<Key, Entry> m_cache;
    Entry m_ascii[4][128];             // single-codepoint fast path (style × ASCII), see glyph(char32_t)
    QHash<quint64, Entry> m_cpCache;   // single non-ASCII codepoint: cp | style<<32 | width<<40
    QList<QRect> m_dirtyRects;         // pixel rects rasterised since the last takeDirtyRects()
    bool m_overflow = false;
};

size_t qHash(const GlyphCache::Key &k, size_t seed = 0);

// GPU-resident copy of the glyph atlas. The old path (QQuickWindow::createTextureFromImage(),
// called whenever GlyphCache::takeDirty() was true) re-uploaded the WHOLE fixed 2048x2048 RGBA
// atlas (~16 MB) through a fresh QSGPlainTexture on every new glyph — measured as a one-off ~64 ms
// stall on the first burst of new glyphs (docs/evidence/solidterm/perf-2026-10-13.md, finding 5 /
// P3). This QSGTexture instead keeps ONE QRhiTexture alive across frames and, in
// commitTextureOperations() (called every frame from the material shader's updateSampledImage,
// per QSGTexture's documented contract), uploads only the atlas sub-rectangles that changed since
// the last render pass, batched into a single QRhiResourceUpdateBatch::uploadTexture() call.
//
// Lifetime note: this holds its own QImage (assigned from GlyphCache::atlas() — implicitly
// shared/ref-counted, so the assignment is cheap) and a plain QList<QRect>, never a pointer back
// into GlyphCache or TerminalView. GlyphCache::rasterize() paints into its OWN QImage via
// QPainter, which detaches (copy-on-write) before writing if anything else (like this texture)
// still holds a reference — so the pixels this texture uploads from are never mutated out from
// under it. That decoupling matters because this texture is owned by the scene graph (via
// GlyphMaterial), which may outlive the TerminalView/GlyphCache briefly during teardown.
class GlyphAtlasTexture : public QSGTexture
{
public:
    ~GlyphAtlasTexture() override;

    // Called from TerminalView::buildPaintNode (render thread, during sync) with GlyphCache's
    // current atlas image and whatever it reported dirty this frame. Rects accumulate across
    // calls until commitTextureOperations() consumes them.
    void addDirtyRects(const QImage &atlas, const QList<QRect> &dirty);

    qint64 comparisonKey() const override;
    QSize textureSize() const override { return m_atlas.size(); }
    bool hasAlphaChannel() const override { return true; }
    bool hasMipmaps() const override { return false; }
    QRhiTexture *rhiTexture() const override { return m_texture; }
    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates) override;

private:
    QImage m_atlas;
    QList<QRect> m_pendingDirty;
    QRhiTexture *m_texture = nullptr;
};
