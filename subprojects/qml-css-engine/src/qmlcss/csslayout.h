#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class CssTheme;
class QQuickItem;
class QTimer;

// The CSS box-model layout engine, in C++ (the QML/V4 implementation it replaces was a
// prototype). Given a styled element (a CssRect) and its content holder, it lays the content
// children out per the resolved CSS — display flex/grid/block, gap, justify/align, padding,
// margin, sizing (px/%/calc/min/max/clamp/vw/vh), aspect-ratio, and absolute positioning —
// assigning each child's geometry and reporting the element's content size back via
// implicitWidth/Height. It also drives transform-keyframe animation interpolation.
//
// QML side is a thin shim: CssRect's size/style/children triggers call requestLayout(); the
// engine coalesces to one pass per event-loop turn.
class CssLayoutEngine : public QObject {
    Q_OBJECT

public:
    explicit CssLayoutEngine(CssTheme *theme, QObject *parent = nullptr);

    // Queue a (re)layout of `root`'s content (held by `content`); coalesced.
    Q_INVOKABLE void requestLayout(QQuickItem *root, QQuickItem *content);
    Q_INVOKABLE void notifyParentLayout(QQuickItem *item);

    // Layout hibernation (owner's batch gate): while a batch is open, requestLayout only
    // RECORDS work; endBatch runs ONE flush for everything. Brackets bulk style passes
    // (reapplyAll / descendant sweeps) so N applies cost one layout, not N. Outside a
    // batch the flush stays synchronous (the resize stale-frame guarantee is untouched).
    void beginBatch() { ++m_batchDepth; }
    void endBatch();
    // Lay out now.
    Q_INVOKABLE void layout(QQuickItem *root, QQuickItem *content);

    // Evaluate a CSS length value (px/%/calc()/min()/max()/clamp()/vw/vh/…) against an axis
    // length; NaN when absent/non-numeric. Exposed for any QML that still needs it.
    Q_INVOKABLE qreal length(const QString &value, qreal avail) const;

    // Transform-keyframe animation. buildAnimStops normalizes `@keyframes` frames into
    // [{offset, rotate, scale, tx, ty}] (identity-filled, 0%/100% synthesised); applyAnim
    // writes the interpolated transform at `progress` onto root's _animRotate/Scale/Tx/Ty.
    Q_INVOKABLE QVariantList buildAnimStops(const QVariantList &frames) const;
    Q_INVOKABLE void applyAnim(QQuickItem *root, const QVariantList &stops, qreal progress) const;

private slots:
    void flush();

private:
    struct Track {
        enum Kind { Px, Fr, Auto } kind = Auto;
        double value = 0;
    };

    // length / calc
    double evalExpr(const QString &value, double avail) const;
    double calcUnit(const QString &tok, double avail) const;

    // box-model helpers
    double sizeOf(const QVariantMap &style, const QString &key, double avail) const;
    // Clamp a resolved width/height by the item's min-/max-<axis> (axis = "width" | "height").
    double clampSize(const QVariantMap &style, const QString &axis, double v, double avail) const;
    QVector<double> box(const QString &value) const;
    QVector<double> paddingOf(const QVariantMap &style) const;
    QVector<double> borderOf(const QVariantMap &style) const;
    QVector<double> marginOf(const QVariantMap &style) const;
    double aspectOf(const QVariantMap &style) const;

    // geometry assignment
    void place(QQuickItem *k, double x, double y, double w, double h) const;
    void placeAbsolute(QQuickItem *k, double ox, double oy, double cw, double ch) const;
    // `wImposed`/`hImposed`: the written size did NOT derive from the item's own content
    // (cross-axis stretch, main-axis grow fill, or an explicit style size) — the CSS
    // "definite size" signal that gates the spec flex-shrink default for its children.
    void place(QQuickItem *k, double x, double y, double w, double h,
               bool wImposed, bool hImposed) const;

    // flex + grid; return the content [width, height] occupied
    QPair<double, double> layoutFlex(const QList<QQuickItem *> &flow, const QVariantMap &rootStyle,
                                     double cw, double ch, double ox, double oy, bool isFlex,
                                     bool widthDefinite, bool heightDefinite) const;
    QPair<double, double> layoutGrid(const QList<QQuickItem *> &flow, const QVariantMap &rootStyle,
                                     double cw, double ch, double ox, double oy) const;

    // grid tracks
    QVector<Track> parseTracks(const QString &spec, double avail) const;
    Track parseTrack(const QString &token, double avail) const;
    QString expandRepeat(const QString &spec) const;
    QStringList gridSplit(const QString &spec) const;
    QVector<double> resolveTracks(const QVector<Track> &tracks, double avail, double gap,
                                  const QVector<double> &contentSizes) const;

    CssTheme *m_theme;
    QHash<QQuickItem *, QPointer<QQuickItem>> m_pending; // root -> content
    QTimer *m_flushTimer;
    int m_batchDepth = 0;
    bool m_flushing = false;
};
