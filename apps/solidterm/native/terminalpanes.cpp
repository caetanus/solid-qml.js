#include "terminalpanes.h"

#include "terminalview.h"

#include <QCursor>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <functional>

// A thin draggable divider between two panes. Reports drag deltas along the container's main axis.
struct TerminalPanes::Handle {
};

namespace {

// A minimal divider item: a coloured rect that drags along one axis. Implemented inline as a
// QQuickItem subclass so it can grab the mouse and emit deltas without a QML file.
class DividerItem : public QQuickItem {
public:
    DividerItem(bool horizontal, std::function<void(qreal)> onDrag, QQuickItem *parent)
        : QQuickItem(parent)
        , m_horizontal(horizontal)
        , m_onDrag(std::move(onDrag))
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setFlag(ItemHasContents, true);
        setCursor(horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    }

    QColor color;

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
        if (m_onDrag)
            m_onDrag(d);
        e->accept();
    }

private:
    bool m_horizontal;
    std::function<void(qreal)> m_onDrag;
    QPointF m_last;
};

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
    connect(v, &TerminalView::sessionFinished, this, [this, v] {
        const int idx = m_panes.indexOf(v);
        if (idx >= 0)
            removePane(idx);
    });
    connect(v, &TerminalView::titleChanged, this, [this, v] {
        if (m_panes.value(m_focused) == v)
            emit titleChanged(v->title());
    });
    return v;
}

void TerminalPanes::componentComplete()
{
    QQuickItem::componentComplete();
    // First pane.
    m_panes.append(makePane());
    m_fractions.append(1.0);
    relayout();
    m_panes[0]->ensureStarted();
    setFocusedIndex(0);
}

void TerminalPanes::split(int orient)
{
    if (m_panes.size() == 1)
        setOrientation(orient); // first split fixes the axis
    addPaneAfterFocused();
}

void TerminalPanes::addPaneAfterFocused()
{
    TerminalView *v = makePane();
    const int at = qBound(0, m_focused + 1, m_panes.size());
    m_panes.insert(at, v);

    // Split the focused pane's share in two; a fresh divider precedes the new pane.
    m_fractions.clear();
    const qreal share = 1.0 / m_panes.size();
    for (int i = 0; i < m_panes.size(); ++i)
        m_fractions.append(share);
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
    v->deleteLater();
    if (m_panes.isEmpty()) {
        emit allClosed();
        return;
    }
    m_fractions.clear();
    const qreal share = 1.0 / m_panes.size();
    for (int i = 0; i < m_panes.size(); ++i)
        m_fractions.append(share);
    relayout();
    setFocusedIndex(qBound(0, index, m_panes.size() - 1));
    emit panesChanged();
}

void TerminalPanes::relayout()
{
    // Drop stale dividers.
    for (QQuickItem *h : std::as_const(m_handles))
        h->deleteLater();
    m_handles.clear();

    const bool horiz = m_orientation == Qt::Horizontal;
    const qreal thick = 6.0;
    const int n = m_panes.size();
    const qreal main = horiz ? width() : height();
    const qreal cross = horiz ? height() : width();
    const qreal available = qMax<qreal>(0, main - thick * (n - 1));

    qreal pos = 0;
    for (int i = 0; i < n; ++i) {
        const qreal len = available * m_fractions.value(i, 1.0 / n);
        TerminalView *v = m_panes[i];
        if (horiz) {
            v->setX(pos); v->setY(0); v->setWidth(len); v->setHeight(cross);
        } else {
            v->setX(0); v->setY(pos); v->setWidth(cross); v->setHeight(len);
        }
        pos += len;
        if (i < n - 1) {
            const int leftIdx = i;
            auto *div = new DividerItem(horiz, [this, leftIdx, available](qreal d) {
                if (available <= 0)
                    return;
                const qreal delta = d / available;
                qreal &a = m_fractions[leftIdx];
                qreal &b = m_fractions[leftIdx + 1];
                const qreal minF = 0.05;
                const qreal na = qBound(minF, a + delta, a + b - minF);
                b = a + b - na;
                a = na;
                relayout();
            }, this);
            div->color = m_handleColor;
            if (horiz) { div->setX(pos); div->setY(0); div->setWidth(thick); div->setHeight(cross); }
            else { div->setX(0); div->setY(pos); div->setWidth(cross); div->setHeight(thick); }
            m_handles.append(div);
            pos += thick;
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
    if (isComponentComplete())
        relayout();
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
}
