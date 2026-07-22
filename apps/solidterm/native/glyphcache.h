#pragma once

#include <QFont>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSize>

// GPU glyph atlas for the terminal (owner: "as fast as alacritty"). Each distinct
// (cluster, bold, italic, width-in-cells) is rasterised ONCE into a cell-sized coverage tile and
// packed into a grayscale atlas image; the terminal then draws the whole grid as textured quads
// in one scene-graph node (GPU-composited) instead of re-rasterising with QPainter every frame.
// Grid-perfect by construction: every tile is exactly cellW×cellH (×2 for wide chars), so cells
// align without per-glyph bearing math.
class GlyphCache {
public:
    struct Entry {
        QRectF uv;        // normalised atlas coordinates
        int widthCells = 1;
        bool valid = false;
    };

    void setFont(const QString &family, int pixelSize);
    qreal cellWidth() const { return m_cellW; }
    qreal cellHeight() const { return m_cellH; }

    // Ensure a glyph is in the atlas; returns its atlas UV rect. `cluster` is the cell's text
    // (usually one char; combining marks / wide chars form a longer cluster).
    Entry glyph(const QString &cluster, bool bold, bool italic, int widthCells);

    const QImage &atlas() const { return m_atlas; }
    bool takeDirty() { const bool d = m_dirty; m_dirty = false; return d; }
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

    QFont m_font, m_bold, m_italic, m_boldItalic;
    qreal m_cellW = 8, m_cellH = 16, m_ascent = 12;
    QImage m_atlas;
    int m_penX = 0, m_penY = 0, m_rowH = 0; // shelf packer
    QHash<Key, Entry> m_cache;
    bool m_dirty = false;
    bool m_overflow = false;
};

size_t qHash(const GlyphCache::Key &k, size_t seed = 0);
