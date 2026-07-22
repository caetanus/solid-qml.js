#include "terminalpanes.h"

#include "paneheader.h"
#include "terminalview.h"

#include <QCursor>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <functional>

struct TerminalPanes::Handle {
};

namespace {

// A thin draggable divider: a coloured rect that drags along one axis and reports pixel deltas.
class DividerItem : public QQuickItem {
public:
    DividerItem(bool horizontal, QQuickItem *parent)
        : QQuickItem(parent)
        , m_horizontal(horizontal)
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setFlag(ItemHasContents, true);
        setCursor(horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    }

    QColor color;
    std::function<void(qreal)> onDrag; // set per-rebuild; the item itself is persistent

    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<QSGSimpleRectNode *>(old);
        if (!node)
            node = new QSGSimpleRectNode;
        node->setColor(color);
        node->setRect(boundingRect());
        return node;
    }

protected:
    void mousePressEvent(QMouseEvent *e) override { m_last = e->scenePosition(); e->accept(); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPointF now = e->scenePosition();
        const qreal d = m_horizontal ? (now.x() - m_last.x()) : (now.y() - m_last.y());
        m_last = now;
        if (onDrag)
            onDrag(d);
        e->accept();
    }

private:
    bool m_horizontal;
    QPointF m_last;
};

constexpr qreal kHeaderH = 24.0;
constexpr qreal kDivider = 6.0;

} // namespace

TerminalPanes::TerminalPanes(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
}

TerminalView *TerminalPanes::makePane()
{
    auto *v = new TerminalView(this);
    applyStyle(v);
    // Per-pane title header (owner: "cada split precisa de seu proprio title").
    auto *header = new PaneHeader(this);
    header->setBackground(m_handleColor);
    header->setForeground(m_foreground);
    header->setAccent(QColor("#3584e4"));
    header->setTitle(v->title());
    m_headers.append(header);

    connect(v, &TerminalView::titleChanged, this, [this, v, header] {
        header->setTitle(v->title());
        if (m_panes.value(m_focused) == v)
            emit titleChanged(v->title());
    });
    connect(header, &PaneHeader::clicked, this, [this, v] {
        const int i = m_panes.indexOf(v);
        if (i >= 0) setFocusedIndex(i);
    });
    connect(header, &PaneHeader::closeRequested, this, [this, v] {
        const int i = m_panes.indexOf(v);
        if (i >= 0) removePane(i);
    });
    connect(v, &TerminalView::sessionFinished, this, [this, v] {
        const int idx = m_panes.indexOf(v);
        if (idx >= 0)
            removePane(idx);
    });
    connect(v, &QQuickItem::activeFocusChanged, this, [this] {
        for (int i = 0; i < m_panes.size(); ++i)
            m_headers[i]->setFocused(m_panes[i]->hasActiveFocus());
    });
    return v;
}

void TerminalPanes::componentComplete()
{
    QQuickItem::componentComplete();
    m_panes.append(makePane());
    m_fractions.append(1.0);
    rebuildDividers();
    relayout();
    m_panes[0]->ensureStarted();
    setFocusedIndex(0);
}

void TerminalPanes::split(int orient)
{
    if (m_panes.size() == 1)
        setOrientation(orient);
    addPaneAfterFocused();
}

void TerminalPanes::addPaneAfterFocused()
{
    TerminalView *v = makePane();
    const int at = qBound(0, m_focused + 1, m_panes.size());
    m_panes.insert(at, v);
    m_headers.move(m_headers.size() - 1, at); // makePane appended the header; align its index

    m_fractions.clear();
    const qreal share = 1.0 / m_panes.size();
    for (int i = 0; i < m_panes.size(); ++i)
        m_fractions.append(share);
    rebuildDividers();
    relayout();
    v->ensureStarted();
    setFocusedIndex(at);
    emit panesChanged();
}

void TerminalPanes::removePane(int index)
{
    if (index < 0 || index >= m_panes.size())
        return;
    TerminalView *v = m_panes.takeAt(index);
    PaneHeader *h = m_headers.takeAt(index);
    v->deleteLater();
    h->deleteLater();
    if (m_panes.isEmpty()) {
        emit allClosed();
        return;
    }
    m_fractions.clear();
    const qreal share = 1.0 / m_panes.size();
    for (int i = 0; i < m_panes.size(); ++i)
        m_fractions.append(share);
    rebuildDividers();
    relayout();
    setFocusedIndex(qBound(0, index, m_panes.size() - 1));
    emit panesChanged();
}

// Dividers are PERSISTENT items — recreated only when the pane COUNT changes, never mid-drag
// (that was the resize bug: relayout() deleted the divider being dragged and lost the grab).
void TerminalPanes::rebuildDividers()
{
    for (QQuickItem *h : std::as_const(m_handles))
        h->deleteLater();
    m_handles.clear();
    const bool horiz = m_orientation == Qt::Horizontal;
    for (int i = 0; i + 1 < m_panes.size(); ++i) {
        auto *div = new DividerItem(horiz, this);
        div->color = m_handleColor;
        const int leftIdx = i;
        div->onDrag = [this, leftIdx](qreal d) {
            const bool h = m_orientation == Qt::Horizontal;
            const qreal mainLen = qMax<qreal>(1, (h ? width() : height()) - kDivider * (m_panes.size() - 1));
            const qreal delta = d / mainLen;
            qreal &a = m_fractions[leftIdx];
            qreal &b = m_fractions[leftIdx + 1];
            const qreal minF = 0.05;
            const qreal na = qBound(minF, a + delta, a + b - minF);
            b = a + b - na;
            a = na;
            relayout(); // reposition only — dividers persist through the drag
        };
        m_handles.append(div);
    }
}

void TerminalPanes::relayout()
{
    const bool horiz = m_orientation == Qt::Horizontal;
    const int n = m_panes.size();
    if (n == 0)
        return;
    const qreal main = horiz ? width() : height();
    const qreal cross = horiz ? height() : width();
    const qreal available = qMax<qreal>(0, main - kDivider * (n - 1));

    qreal pos = 0;
    for (int i = 0; i < n; ++i) {
        const qreal len = available * m_fractions.value(i, 1.0 / n);
        TerminalView *v = m_panes[i];
        PaneHeader *hdr = m_headers[i];
        if (horiz) {
            hdr->setX(pos); hdr->setY(0); hdr->setWidth(len); hdr->setHeight(kHeaderH);
            v->setX(pos); v->setY(kHeaderH); v->setWidth(len); v->setHeight(qMax<qreal>(0, cross - kHeaderH));
        } else {
            hdr->setX(0); hdr->setY(pos); hdr->setWidth(cross); hdr->setHeight(kHeaderH);
            v->setX(0); v->setY(pos + kHeaderH); v->setWidth(cross); v->setHeight(qMax<qreal>(0, len - kHeaderH));
        }
        pos += len;
        if (i < n - 1) {
            QQuickItem *div = m_handles.value(i);
            if (div) {
                if (horiz) { div->setX(pos); div->setY(0); div->setWidth(kDivider); div->setHeight(cross); }
                else { div->setX(0); div->setY(pos); div->setWidth(cross); div->setHeight(kDivider); }
            }
            pos += kDivider;
        }
    }
}

void TerminalPanes::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (isComponentComplete())
        relayout();
}

void TerminalPanes::setFocusedIndex(int i)
{
    if (i < 0 || i >= m_panes.size())
        return;
    m_focused = i;
    m_panes[i]->takeFocus();
    for (int k = 0; k < m_headers.size(); ++k)
        m_headers[k]->setFocused(k == i);
    emit titleChanged(m_panes[i]->title());
}

int TerminalPanes::focusedIndex() const
{
    for (int i = 0; i < m_panes.size(); ++i)
        if (m_panes[i]->hasActiveFocus())
            return i;
    return m_focused;
}

void TerminalPanes::closeFocused() { removePane(focusedIndex()); }
void TerminalPanes::focusNext() { setFocusedIndex((focusedIndex() + 1) % qMax(1, m_panes.size())); }
void TerminalPanes::focusPrev() { setFocusedIndex((focusedIndex() - 1 + m_panes.size()) % qMax(1, m_panes.size())); }

void TerminalPanes::copyFocused() { if (auto *v = m_panes.value(focusedIndex())) v->copySelection(); }
void TerminalPanes::pasteFocused() { if (auto *v = m_panes.value(focusedIndex())) v->pasteClipboard(); }
void TerminalPanes::clearFocused() { if (auto *v = m_panes.value(focusedIndex())) v->clearScrollback(); }

void TerminalPanes::applyStyle(TerminalView *v)
{
    v->setFontFamily(m_fontFamily);
    v->setFontSize(m_fontSize);
    v->setBackground(m_background);
    v->setForeground(m_foreground);
    v->setScrollbackLimit(m_scrollbackLimit);
}

void TerminalPanes::setOrientation(int v)
{
    if (m_orientation == v)
        return;
    m_orientation = v;
    emit orientationChanged();
    if (isComponentComplete()) {
        rebuildDividers();
        relayout();
    }
}

#define STYLE_SETTER(Setter, Member, Type) \
    void TerminalPanes::Setter(Type v) { \
        if (Member == v) return; \
        Member = v; emit styleChanged(); \
        for (TerminalView *p : std::as_const(m_panes)) applyStyle(p); \
    }
STYLE_SETTER(setFontFamily, m_fontFamily, const QString &)
STYLE_SETTER(setFontSize, m_fontSize, int)
STYLE_SETTER(setBackground, m_background, const QColor &)
STYLE_SETTER(setForeground, m_foreground, const QColor &)
STYLE_SETTER(setScrollbackLimit, m_scrollbackLimit, int)
#undef STYLE_SETTER

void TerminalPanes::setHandleColor(const QColor &v)
{
    if (m_handleColor == v)
        return;
    m_handleColor = v;
    emit styleChanged();
    for (QQuickItem *h : std::as_const(m_handles))
        if (auto *d = static_cast<DividerItem *>(h)) { d->color = v; d->update(); }
    for (PaneHeader *h : std::as_const(m_headers))
        h->setBackground(v);
}
