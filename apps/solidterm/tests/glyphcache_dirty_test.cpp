// GlyphCache's dirty-rect tracking (glyphcache.h) is the mechanism GlyphAtlasTexture::
// commitTextureOperations relies on to upload only the atlas sub-rectangles that CHANGED, instead
// of re-uploading the whole fixed 2048x2048 RGBA atlas (~16 MB) on every new glyph — the ~64ms
// stall on the first burst of new glyphs measured in docs/evidence/solidterm/perf-2026-10-13.md
// (finding 5 / P3). This measures what a render pass would actually upload: bytes proportional to
// the number of NEW glyph cells, not the atlas size, plus the steady-state no-op (cache hits never
// mark anything dirty) and the always-full-reupload contract of reset() (font change / overflow).
//
// Needs a QGuiApplication (font engine) but no window or scene graph — QT_QPA_PLATFORM=offscreen.
#include "../native/glyphcache.h"

#include <QGuiApplication>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool cond, const char *what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

qint64 bytesOf(const QList<QRect> &rects)
{
    qint64 n = 0;
    for (const QRect &r : rects)
        n += qint64(r.width()) * r.height() * 4; // RGBA8/BGRA8: 4 bytes/pixel
    return n;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    GlyphCache cache;
    cache.setFont(QStringLiteral("monospace"), 15, 1.0);

    // setFont() calls reset(): the texture's first-ever upload has to see the whole atlas. Drain
    // that before measuring an incremental burst.
    const QList<QRect> initial = cache.takeDirtyRects();
    expect(!initial.isEmpty(), "setFont()/reset() reports the whole atlas dirty once");
    const qint64 fullAtlasBytes = bytesOf(initial);
    expect(fullAtlasBytes == qint64(2048) * 2048 * 4, "that one rect IS the whole 2048x2048 atlas");

    // Steady state: nothing rasterised since the last takeDirtyRects() call => nothing to upload.
    expect(cache.takeDirtyRects().isEmpty(), "no new glyphs means no dirty rects to re-upload");

    // A burst of N distinct, never-before-seen glyphs (Latin Extended-A — real font glyphs, not
    // ASCII, so each one is a genuine cache miss) — representative of the "first bold/italic font
    // load" burst the perf doc measured as a whole-atlas-reupload stall.
    const int N = 40;
    for (int i = 0; i < N; ++i) {
        const GlyphCache::Entry e = cache.glyph(char32_t(0x0100 + i), false, false, 1); // Ā, ā, Ă, ă, ...
        expect(e.valid, "glyph rasterised into the atlas");
    }

    const QList<QRect> dirty = cache.takeDirtyRects();
    const qint64 uploaded = bytesOf(dirty);
    const qint64 cellBytes = qint64(cache.cellWidth()) * qint64(cache.cellHeight()) * 4; // dpr=1: logical==physical
    const qint64 upperBound = qint64(N) * cellBytes * 3; // generous slack for the packer's 1px gaps
    std::fprintf(stderr, "burst of %d glyphs: %lld bytes across %d rect(s) (whole atlas = %lld bytes)\n",
                 N, static_cast<long long>(uploaded), int(dirty.size()), static_cast<long long>(fullAtlasBytes));

    expect(!dirty.isEmpty(), "the burst reports dirty rects");
    expect(uploaded > 0 && uploaded < upperBound,
           "bytes uploaded scale with the N new glyph cells, not with the atlas size");
    expect(uploaded < fullAtlasBytes / 10,
           "bytes uploaded are a small fraction of the whole atlas (not a 16MB re-upload)");
    // The shelf packer lays same-size glyphs left-to-right on one row; markDirty() coalesces
    // adjacent tiles, so a burst that fits on one (or a couple of) shelf rows is a handful of
    // rects, not one per glyph.
    expect(dirty.size() <= 3, "the burst coalesces into a handful of rects, not one per glyph");

    // Draining leaves nothing pending, and re-rasterising the SAME glyphs (cache hits) adds
    // nothing new either.
    expect(cache.takeDirtyRects().isEmpty(), "dirty rects are consumed once, not re-reported");
    for (int i = 0; i < N; ++i)
        cache.glyph(char32_t(0x0100 + i), false, false, 1);
    expect(cache.takeDirtyRects().isEmpty(), "cache hits never mark anything dirty");

    // reset() (font change / atlas overflow) always asks for a full re-upload again, by design —
    // the one case where "re-upload everything" is actually correct.
    cache.reset();
    const QList<QRect> afterReset = cache.takeDirtyRects();
    expect(bytesOf(afterReset) == fullAtlasBytes, "reset() always reports exactly one full-atlas rect");

    if (failures) {
        std::fprintf(stderr, "%d check(s) FAILED\n", failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
