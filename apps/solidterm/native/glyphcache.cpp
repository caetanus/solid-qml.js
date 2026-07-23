#include "glyphcache.h"

#include <QFontMetricsF>
#include <QPainter>

size_t qHash(const GlyphCache::Key &k, size_t seed)
{
    return qHashMulti(seed, k.cluster, k.style, k.widthCells);
}

// ─── procedural cell graphics (box-drawing + block elements) ────────────────────────────────────
// Fonts rasterise U+2500-259F glyphs to their own ink box, which rarely matches the terminal cell —
// so blocks leave seams and box lines don't connect across cells. Like kitty/alacritty/wezterm, we
// DRAW these shapes ourselves at the exact cell rect [x0..x0+w]×[y0..y0+h], so neighbours tile with
// zero seam and lines join. White coverage (tinted by fg later); hard-edged (no AA) so edges meet.
namespace {

inline void fillR(QPainter *p, qreal x, qreal y, qreal w, qreal h)
{
    p->fillRect(QRectF(x, y, w, h), Qt::white);
}

// Box-drawing arms: weight per direction (0 none, 1 light, 2 heavy). Light/heavy table for
// U+2500..254B in {up,right,down,left} order. Dashed variants (2504-250B) render as solid — the
// seam-free join is what matters, not the dash pattern.
const quint8 kBoxArms[0x4C][4] = {
    /*2500*/{0,1,0,1},/*2501*/{0,2,0,2},/*2502*/{1,0,1,0},/*2503*/{2,0,2,0},
    /*2504*/{0,1,0,1},/*2505*/{0,2,0,2},/*2506*/{1,0,1,0},/*2507*/{2,0,2,0},
    /*2508*/{0,1,0,1},/*2509*/{0,2,0,2},/*250A*/{1,0,1,0},/*250B*/{2,0,2,0},
    /*250C*/{0,1,1,0},/*250D*/{0,2,1,0},/*250E*/{0,1,2,0},/*250F*/{0,2,2,0},
    /*2510*/{0,0,1,1},/*2511*/{0,0,1,2},/*2512*/{0,0,2,1},/*2513*/{0,0,2,2},
    /*2514*/{1,1,0,0},/*2515*/{1,2,0,0},/*2516*/{2,1,0,0},/*2517*/{2,2,0,0},
    /*2518*/{1,0,0,1},/*2519*/{1,0,0,2},/*251A*/{2,0,0,1},/*251B*/{2,0,0,2},
    /*251C*/{1,1,1,0},/*251D*/{1,2,1,0},/*251E*/{2,1,1,0},/*251F*/{1,1,2,0},
    /*2520*/{2,1,2,0},/*2521*/{2,2,1,0},/*2522*/{1,2,2,0},/*2523*/{2,2,2,0},
    /*2524*/{1,0,1,1},/*2525*/{1,0,1,2},/*2526*/{2,0,1,1},/*2527*/{1,0,2,1},
    /*2528*/{2,0,2,1},/*2529*/{2,0,1,2},/*252A*/{1,0,2,2},/*252B*/{2,0,2,2},
    /*252C*/{0,1,1,1},/*252D*/{0,1,1,2},/*252E*/{0,2,1,1},/*252F*/{0,2,1,2},
    /*2530*/{0,1,2,1},/*2531*/{0,1,2,2},/*2532*/{0,2,2,1},/*2533*/{0,2,2,2},
    /*2534*/{1,1,0,1},/*2535*/{1,1,0,2},/*2536*/{1,2,0,1},/*2537*/{1,2,0,2},
    /*2538*/{2,1,0,1},/*2539*/{2,1,0,2},/*253A*/{2,2,0,1},/*253B*/{2,2,0,2},
    /*253C*/{1,1,1,1},/*253D*/{1,1,1,2},/*253E*/{1,2,1,1},/*253F*/{1,2,1,2},
    /*2540*/{2,1,1,1},/*2541*/{1,1,2,1},/*2542*/{2,1,2,1},/*2543*/{2,1,1,2},
    /*2544*/{2,2,1,1},/*2545*/{1,1,2,2},/*2546*/{1,2,2,1},/*2547*/{2,2,1,2},
    /*2548*/{1,2,2,2},/*2549*/{2,1,2,2},/*254A*/{2,2,2,1},/*254B*/{2,2,2,2},
};

bool drawCellGraphic(QPainter *p, char32_t cp, qreal x0, qreal y0, qreal w, qreal h)
{
    const qreal xm = qRound(x0 + w / 2), ym = qRound(y0 + h / 2), x1 = x0 + w, y1 = y0 + h;

    // ---- Block Elements U+2580..259F ----
    switch (cp) {
    case 0x2588: fillR(p, x0, y0, w, h); return true;          // █ full
    case 0x2580: fillR(p, x0, y0, w, ym - y0); return true;     // ▀ upper half
    case 0x2584: fillR(p, x0, ym, w, y1 - ym); return true;     // ▄ lower half
    case 0x258C: fillR(p, x0, y0, xm - x0, h); return true;     // ▌ left half
    case 0x2590: fillR(p, xm, y0, x1 - xm, h); return true;     // ▐ right half
    case 0x2594: fillR(p, x0, y0, w, qRound(h / 8)); return true;             // ▔ upper 1/8
    case 0x2595: fillR(p, x1 - qRound(w / 8), y0, qRound(w / 8), h); return true; // ▕ right 1/8
    }
    if (cp >= 0x2581 && cp <= 0x2587) { // lower eighths (2588 full handled above)
        const qreal t = qRound(h * (cp - 0x2580) / 8.0);
        fillR(p, x0, y1 - t, w, t); return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) { // left eighths
        const qreal t = qRound(w * (8 - (cp - 0x2588)) / 8.0);
        fillR(p, x0, y0, t, h); return true;
    }
    if (cp == 0x2591 || cp == 0x2592 || cp == 0x2593) { // ░▒▓ shades → alpha coverage
        const int a = cp == 0x2591 ? 64 : cp == 0x2592 ? 128 : 192;
        p->fillRect(QRectF(x0, y0, w, h), QColor(255, 255, 255, a));
        return true;
    }
    if (cp >= 0x2596 && cp <= 0x259F) { // quadrants (bits UL=8 UR=4 LL=2 LR=1)
        static const quint8 q[10] = { 2, 1, 8, 11, 9, 14, 13, 4, 6, 7 };
        const quint8 m = q[cp - 0x2596];
        if (m & 8) fillR(p, x0, y0, xm - x0, ym - y0);
        if (m & 4) fillR(p, xm, y0, x1 - xm, ym - y0);
        if (m & 2) fillR(p, x0, ym, xm - x0, y1 - ym);
        if (m & 1) fillR(p, xm, ym, x1 - xm, y1 - ym);
        return true;
    }

    // ---- Powerline separators U+E0B0..E0B3 (Nerd-Font PUA) ----
    // The prompt's segment arrows: font glyphs often don't span the full cell height → a hairline
    // gap between segments. Draw the triangle to the exact cell so segment backgrounds meet flush.
    if (cp >= 0xE0B0 && cp <= 0xE0B3) {
        p->setRenderHint(QPainter::Antialiasing, true); // the diagonal wants AA; the flat side is on a cell edge so it still tiles
        if (cp == 0xE0B0 || cp == 0xE0B2) {             // solid ▶ / ◀
            p->setPen(Qt::NoPen);
            p->setBrush(Qt::white);
            QPolygonF tri;
            if (cp == 0xE0B0) tri << QPointF(x0, y0) << QPointF(x1, ym) << QPointF(x0, y1);
            else              tri << QPointF(x1, y0) << QPointF(x0, ym) << QPointF(x1, y1);
            p->drawPolygon(tri);
        } else {                                        // thin chevron › / ‹
            QPen pen(Qt::white, qMax<qreal>(1.5, h * 0.06));
            pen.setJoinStyle(Qt::MiterJoin);
            p->setBrush(Qt::NoBrush);
            p->setPen(pen);
            QPolygonF v;
            if (cp == 0xE0B1) v << QPointF(x0, y0) << QPointF(x1, ym) << QPointF(x0, y1);
            else              v << QPointF(x1, y0) << QPointF(x0, ym) << QPointF(x1, y1);
            p->drawPolyline(v);
        }
        return true;
    }

    // ---- Box Drawing U+2500..254B (light/heavy) ----
    if (cp >= 0x2500 && cp <= 0x254B) {
        const quint8 *a = kBoxArms[cp - 0x2500];
        const qreal tl = qMax<qreal>(1.0, qRound(h * 0.07));
        const qreal th = qMax<qreal>(2.0, qRound(h * 0.15));
        auto tk = [&](int wt) { return wt == 2 ? th : tl; };
        // Each present arm reaches from the cell edge to the centre (a touch past, to join cleanly).
        if (a[0]) { const qreal t = tk(a[0]); fillR(p, xm - t / 2, y0, t, ym - y0 + t / 2); }       // up
        if (a[2]) { const qreal t = tk(a[2]); fillR(p, xm - t / 2, ym - t / 2, t, y1 - ym + t / 2); } // down
        if (a[3]) { const qreal t = tk(a[3]); fillR(p, x0, ym - t / 2, xm - x0 + t / 2, t); }        // left
        if (a[1]) { const qreal t = tk(a[1]); fillR(p, xm - t / 2, ym - t / 2, x1 - xm + t / 2, t); } // right
        return true;
    }

    // ---- Double-line box U+2550..256C ----
    // Arms as {up,right,down,left}, value 0 none / 1 single / 2 double. Double arms draw two parallel
    // rails offset ±g; each rail overshoots the centre by g so perpendicular rails meet at corners.
    if (cp >= 0x2550 && cp <= 0x256C) {
        static const quint8 kDbl[0x1D][4] = {
            /*2550*/{0,2,0,2},/*2551*/{2,0,2,0},/*2552*/{0,2,1,0},/*2553*/{0,1,2,0},
            /*2554*/{0,2,2,0},/*2555*/{0,0,1,2},/*2556*/{0,0,2,1},/*2557*/{0,0,2,2},
            /*2558*/{1,2,0,0},/*2559*/{2,1,0,0},/*255A*/{2,2,0,0},/*255B*/{1,0,0,2},
            /*255C*/{2,0,0,1},/*255D*/{2,0,0,2},/*255E*/{1,2,1,0},/*255F*/{2,1,2,0},
            /*2560*/{2,2,2,0},/*2561*/{1,0,1,2},/*2562*/{2,0,2,1},/*2563*/{2,0,2,2},
            /*2564*/{0,2,1,2},/*2565*/{0,1,2,1},/*2566*/{0,2,2,2},/*2567*/{1,2,0,2},
            /*2568*/{2,1,0,1},/*2569*/{2,2,0,2},/*256A*/{1,2,1,2},/*256B*/{2,1,2,1},
            /*256C*/{2,2,2,2},
        };
        const quint8 *a = kDbl[cp - 0x2550];
        const qreal t = qMax<qreal>(1.0, qRound(h * 0.06));
        const qreal g = qMax<qreal>(t, qRound(h * 0.09)); // rail offset from centre
        // vertical rail x-positions (for up/down), horizontal rail y-positions (for left/right)
        const qreal vx[2] = { xm - g, xm + g }, hy[2] = { ym - g, ym + g };
        auto vrail = [&](qreal cx, qreal ya, qreal yb) { fillR(p, cx - t / 2, ya, t, yb - ya); };
        auto hrail = [&](qreal cy, qreal xa, qreal xb) { fillR(p, xa, cy - t / 2, xb - xa, t); };
        // up
        if (a[0] == 1) vrail(xm, y0, ym + g);
        else if (a[0] == 2) { vrail(vx[0], y0, ym + g); vrail(vx[1], y0, ym + g); }
        if (a[2] == 1) vrail(xm, ym - g, y1);
        else if (a[2] == 2) { vrail(vx[0], ym - g, y1); vrail(vx[1], ym - g, y1); }
        if (a[3] == 1) hrail(ym, x0, xm + g);
        else if (a[3] == 2) { hrail(hy[0], x0, xm + g); hrail(hy[1], x0, xm + g); }
        if (a[1] == 1) hrail(ym, xm - g, x1);
        else if (a[1] == 2) { hrail(hy[0], xm - g, x1); hrail(hy[1], xm - g, x1); }
        return true;
    }

    // ---- Braille U+2800..28FF ----
    // 2×4 dot matrix; the low 8 bits of (cp-0x2800) are the dots. Standard numbering:
    // 1 4 / 2 5 / 3 6 / 7 8  → bit0..bit7. Each dot is a filled blob in its sub-cell.
    if (cp >= 0x2800 && cp <= 0x28FF) {
        const int mask = int(cp - 0x2800);
        const qreal cw = w / 2, ch = h / 4;
        const qreal d = qMax<qreal>(1.0, qMin(cw, ch) * 0.62);
        static const int col[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
        static const int rowIdx[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
        for (int i = 0; i < 8; ++i)
            if (mask & (1 << i)) {
                const qreal cx = x0 + (col[i] + 0.5) * cw, cy = y0 + (rowIdx[i] + 0.5) * ch;
                p->setBrush(Qt::white);
                p->setPen(Qt::NoPen);
                p->setRenderHint(QPainter::Antialiasing, true);
                p->drawEllipse(QPointF(cx, cy), d / 2, d / 2);
            }
        return true;
    }

    // ---- Sextants U+1FB00..1FB3B (2×3 block mosaic) ----
    // 6 sub-cells, bit i = (col, row): b0=(0,0) b1=(1,0) b2=(0,1) b3=(1,1) b4=(0,2) b5=(1,2). The 60
    // codepoints enumerate 6-bit values 1..62 skipping 21 (=▌ left half) and 42 (=▐ right half).
    if (cp >= 0x1FB00 && cp <= 0x1FB3B) {
        const int index = int(cp - 0x1FB00);
        int v = 0, seen = -1;
        for (int cand = 1; cand <= 62; ++cand) {
            if (cand == 21 || cand == 42) continue;
            if (++seen == index) { v = cand; break; }
        }
        const qreal cw = w / 2, rh = h / 3;
        for (int i = 0; i < 6; ++i)
            if (v & (1 << i)) {
                const qreal sx = x0 + (i & 1 ? cw : 0), sy = y0 + (i / 2) * rh;
                fillR(p, sx, sy, cw, rh);
            }
        return true;
    }
    return false;
}

} // namespace

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
    // Box-drawing / block-element single codepoints: draw procedurally at the exact cell rect so they
    // tile seamlessly (the font's glyphs don't fill the cell → visible seams). Everything else: font.
    const QList<uint> cps = k.cluster.toUcs4();
    if (cps.size() == 1 && drawCellGraphic(&p, cps.first(), m_penX, m_penY, tileW, tileH)) {
        // drawn procedurally
    } else {
        // Nerd-Font / symbol glyphs are often wider (or taller) than one cell; the tile clip would cut
        // them (the git/github/folder icons get chopped on the right). If the ink overflows the cell,
        // uniform-scale it to fit and centre it (icons aren't baseline-aligned text). Normal glyphs
        // keep the shared baseline so text lines up.
        const QRectF br = QFontMetricsF(p.font()).boundingRect(k.cluster);
        if (br.width() > tileW + 0.5 || br.height() > tileH + 0.5) {
            const qreal s = qMin(tileW / qMax<qreal>(1.0, br.width()), tileH / qMax<qreal>(1.0, br.height()));
            p.save();
            p.translate(m_penX + tileW / 2.0, m_penY + tileH / 2.0); // tile centre
            p.scale(s, s);
            p.translate(-br.center().x(), -br.center().y());          // ink centre → tile centre
            p.drawText(QPointF(0, 0), k.cluster);
            p.restore();
        } else {
            p.drawText(QPointF(m_penX, m_penY + m_ascent), k.cluster);
        }
    }
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
