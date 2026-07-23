#include "glyphcache.h"

#include <QFontMetricsF>
#include <QPainter>

size_t qHash(const GlyphCache::Key &k, size_t seed)
{
    return qHashMulti(seed, k.cluster, k.style, k.widthCells);
}

void GlyphCache::setFont(const QString &family, int pixelSize)
{
    if (!m_atlas.isNull() && m_font.family() == family && m_font.pixelSize() == pixelSize)
        return; // same font: keep the cached glyphs (resize must not throw away the atlas)
    m_font = QFont(family);
    m_font.setPixelSize(pixelSize);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setHintingPreference(QFont::PreferFullHinting);
    m_bold = m_font; m_bold.setBold(true);
    m_italic = m_font; m_italic.setItalic(true);
    m_boldItalic = m_bold; m_boldItalic.setItalic(true);

    const QFontMetricsF fm(m_font);
    m_cellW = qMax<qreal>(1, qRound(fm.horizontalAdvance(QLatin1Char('M'))));
    m_cellH = qMax<qreal>(1, qRound(fm.height()));
    m_ascent = fm.ascent();
    reset();
}

void GlyphCache::reset()
{
    // Fixed 2048² premultiplied-RGBA atlas: UVs must never shift mid-frame, so the atlas can't grow
    // while a frame is being built. 2048² at ~9×17 px/tile holds >25k glyph tiles — no real session's
    // glyph set overflows it; if one somehow does, glyph() flags overflow and the caller rebuilds.
    m_atlas = QImage(2048, 2048, QImage::Format_ARGB32_Premultiplied);
    m_atlas.fill(Qt::transparent);
    m_penX = 0;
    m_penY = 0;
    m_rowH = 0;
    m_cache.clear();
    m_dirty = true;
    m_overflow = false;
}

GlyphCache::Entry GlyphCache::glyph(const QString &cluster, bool bold, bool italic, int widthCells)
{
    Key k{ cluster, quint8((bold ? 1 : 0) | (italic ? 2 : 0)), qMax(1, widthCells) };
    const auto it = m_cache.constFind(k);
    if (it != m_cache.constEnd())
        return it.value();
    const Entry e = rasterize(k);
    m_cache.insert(k, e);
    return e;
}

GlyphCache::Entry GlyphCache::rasterize(const Key &k)
{
    const int tileW = int(m_cellW) * k.widthCells;
    const int tileH = int(m_cellH);

    // Shelf-pack: advance the pen; wrap to a new shelf. The atlas is fixed-size — if a new shelf
    // would overflow, flag it (the caller rebuilds next frame) and skip drawing this glyph.
    if (m_penX + tileW > m_atlas.width()) {
        m_penX = 0;
        m_penY += m_rowH + 1;
        m_rowH = 0;
    }
    if (m_penY + tileH > m_atlas.height()) {
        m_overflow = true;
        return Entry{}; // invalid: nothing drawn for this cell this frame
    }

    // Render the cluster in WHITE on transparent into its tile. Monochrome glyphs come out white
    // (coverage, tinted later by fg); colour glyphs (emoji, COLR/CBDT fonts) ignore the pen and
    // render in their own colours.
    QPainter p(&m_atlas);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setPen(Qt::white);
    p.setFont(k.style & 1 ? (k.style & 2 ? m_boldItalic : m_bold)
                          : (k.style & 2 ? m_italic : m_font));
    p.setClipRect(m_penX, m_penY, tileW, tileH);
    p.drawText(QPointF(m_penX, m_penY + m_ascent), k.cluster);
    p.end();

    Entry e;
    e.widthCells = k.widthCells;
    e.uv = QRectF(qreal(m_penX) / m_atlas.width(), qreal(m_penY) / m_atlas.height(),
                  qreal(tileW) / m_atlas.width(), qreal(tileH) / m_atlas.height());
    // Colour glyph if any drawn pixel has non-equal RGB channels (a coloured, not white, pixel).
    for (int y = m_penY; y < m_penY + tileH && !e.color; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(m_atlas.constScanLine(y));
        for (int x = m_penX; x < m_penX + tileW; ++x) {
            const QRgb px = row[x];
            if (qAlpha(px) && (qRed(px) != qGreen(px) || qGreen(px) != qBlue(px))) { e.color = true; break; }
        }
    }
    e.valid = true;

    m_penX += tileW + 1;
    m_rowH = qMax(m_rowH, tileH);
    m_dirty = true;
    return e;
}
