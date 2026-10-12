#include "terminaltabs.h"

#include "terminalpanes.h"
#include "terminalview.h"

#include <QCoreApplication>
#include <QDropEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>

#include <utility>

// The tab BAR lives in the TSX now; this class is headless — it only keeps the live panes and shows
// one by index. The model (titles + active) is pushed to Solid via tabsChanged().

TerminalTabs::TerminalTabs(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
    s_allTabs.append(this);
}

TerminalTabs::~TerminalTabs()
{
    s_allTabs.removeAll(this);
    if (m_filteredWindow)
        m_filteredWindow->removeEventFilter(this);
}

QVector<TerminalTabs *> TerminalTabs::s_allTabs;

TerminalPanes *TerminalTabs::active() const
{
    return (m_active >= 0 && m_active < m_tabs.size()) ? m_tabs[m_active].panes : nullptr;
}

void TerminalTabs::emitModel()
{
    QStringList titles;
    titles.reserve(m_tabs.size());
    for (const Tab &t : m_tabs)
        titles.append(t.effective());
    emit tabsChanged(titles, m_active);
}

void TerminalTabs::setTabTitle(int index, const QString &title)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    m_tabs[index].customTitle = title;
    emitModel();
    if (index == m_active)
        emit titleChanged(m_tabs[index].effective());
}

QString TerminalTabs::tabTitle(int index) const
{
    return (index >= 0 && index < m_tabs.size()) ? m_tabs[index].effective() : QString();
}

QVariantList TerminalTabs::overview() const
{
    QVariantList out;
    for (int t = 0; t < m_tabs.size(); ++t) {
        const QStringList titles = m_tabs[t].panes->paneTitles();
        const int focused = m_tabs[t].panes->focusedPaneIndex();
        for (int p = 0; p < titles.size(); ++p) {
            QVariantMap row;
            row["tab"] = t;
            row["pane"] = p;
            row["panes"] = titles.size();
            row["tabTitle"] = m_tabs[t].effective();
            row["title"] = titles[p];
            row["active"] = (t == m_active && p == focused);
            out.append(row);
        }
    }
    return out;
}

void TerminalTabs::focusPane(int tab, int pane)
{
    if (tab < 0 || tab >= m_tabs.size())
        return;
    selectTab(tab);
    m_tabs[tab].panes->focusLeafByIndex(pane);
}

TerminalPanes *TerminalTabs::makePanes()
{
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return nullptr;
    if (!m_panesComponent) {
        m_panesComponent = new QQmlComponent(eng, this);
        m_panesComponent->setData(QByteArrayLiteral("import SolidTerm 1.0\nTerminalPanes {}"),
                                  QUrl(QStringLiteral("qrc:/solidterm/TabPanes.qml")));
    }
    auto *panes = qobject_cast<TerminalPanes *>(m_panesComponent->create(qmlContext(this)));
    if (!panes) {
        qWarning("solidterm: tab panes failed: %s", qPrintable(m_panesComponent->errorString()));
        return nullptr;
    }
    panes->setParentItem(this);
    // QObject OWNER too: a parent item does not delete its children, so without this a closed
    // window's panes — and their shells — outlived it (one process serves every window now).
    panes->setParent(this);
    applyStyle(panes);

    connect(panes, &TerminalPanes::accelerator, this, &TerminalTabs::accelerator);
    connect(panes, &TerminalPanes::searchChanged, this, [this, panes](int idx, int count) {
        if (active() == panes)
            emit searchChanged(idx, count);
    });
    connect(panes, &TerminalPanes::hoverLinkChanged, this, [this, panes](const QString &url) {
        if (active() == panes)
            emit hoverLinkChanged(url);
    });
    connect(panes, &TerminalPanes::unsafePasteRequested, this, &TerminalTabs::unsafePasteRequested);
    connect(panes, &TerminalPanes::zoomRequested, this, &TerminalTabs::zoomRequested);
    connect(panes, &TerminalPanes::paneMenuRequested, this, [this, panes](qreal x, qreal y, bool ro) {
        if (active() == panes)
            emit paneMenuRequested(x, y, ro);
    });
    connect(panes, &TerminalPanes::titleChanged, this, [this, panes](const QString &t) {
        const int i = [&] { for (int k = 0; k < m_tabs.size(); ++k) if (m_tabs[k].panes == panes) return k; return -1; }();
        if (i < 0)
            return;
        m_tabs[i].title = t;
        emitModel();
        if (i == m_active)
            emit titleChanged(m_tabs[i].effective());
    });
    connect(panes, &TerminalPanes::allClosed, this, [this, panes] {
        for (int k = 0; k < m_tabs.size(); ++k)
            if (m_tabs[k].panes == panes) { closeTab(k); return; }
    });
    return panes;
}

void TerminalTabs::componentComplete()
{
    QQuickItem::componentComplete();
    newTab(); // the first tab
}

void TerminalTabs::newTab()
{
    TerminalPanes *panes = makePanes();
    if (!panes)
        return;
    m_tabs.append({ panes, QString() });
    selectTab(m_tabs.size() - 1);
}

void TerminalTabs::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    TerminalPanes *panes = m_tabs[index].panes;
    m_tabs.removeAt(index);
    if (panes)
        panes->deleteLater();
    if (m_tabs.isEmpty()) {
        // This window has no terminals left → close JUST this window (a detached window emptying must
        // not kill the whole app). Once no pane survives in any window, quit. Keep allClosed for any
        // Solid-side listeners, but the lifetime decision is native + multi-window aware now.
        emit allClosed();
        if (QQuickWindow *w = window())
            w->close();
        QTimer::singleShot(0, qApp, [] {
            if (!TerminalPanes::anyLive())
                QCoreApplication::quit();
        });
        return;
    }
    if (m_active >= m_tabs.size())
        m_active = m_tabs.size() - 1;
    else if (index < m_active)
        --m_active;
    selectTab(m_active);
}

void TerminalTabs::selectTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    m_active = index;
    for (int k = 0; k < m_tabs.size(); ++k)
        m_tabs[k].panes->setVisible(k == index);
    relayout();
    emitModel();
    if (auto *p = active()) {
        QMetaObject::invokeMethod(p, "refocus", Qt::QueuedConnection); // keyboard focus → its pane
        emit titleChanged(m_tabs[index].effective());
    }
}

void TerminalTabs::relayout()
{
    for (const Tab &t : m_tabs) {
        t.panes->setX(0); t.panes->setY(0);
        t.panes->setWidth(width()); t.panes->setHeight(height());
    }
}

void TerminalTabs::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (!isComponentComplete())
        return;
    relayout();
    if (!m_didInitialFocus && width() > 0 && height() > 0 && active()) {
        m_didInitialFocus = true;
        selectTab(m_active);
    }
}

void TerminalTabs::focusInEvent(QFocusEvent *)
{
    if (auto *p = active())
        p->setFocus(true);
}

// ─── proxied pane ops ───────────────────────────────────────────────────────────────────────────

void TerminalTabs::split(int orient) { if (auto *p = active()) p->split(orient); }
void TerminalTabs::closeFocused() { if (auto *p = active()) p->closeFocused(); }
void TerminalTabs::focusNext() { if (auto *p = active()) p->focusNext(); }
void TerminalTabs::focusPrev() { if (auto *p = active()) p->focusPrev(); }
void TerminalTabs::refocus() { if (auto *p = active()) p->refocus(); }
void TerminalTabs::toggleZoom() { if (auto *p = active()) p->toggleZoom(); }
int TerminalTabs::zoomFocused(int delta) { auto *p = active(); return p ? p->zoomFocused(delta) : 0; }
void TerminalTabs::copyFocused() { if (auto *p = active()) p->copyFocused(); }
void TerminalTabs::pasteFocused() { if (auto *p = active()) p->pasteFocused(); }
void TerminalTabs::pasteTextFocused(const QString &t) { if (auto *p = active()) p->pasteTextFocused(t); }
void TerminalTabs::clearFocused() { if (auto *p = active()) p->clearFocused(); }
void TerminalTabs::resetFocused() { if (auto *p = active()) p->resetFocused(); }
void TerminalTabs::setReadOnlyFocused(bool v) { if (auto *p = active()) p->setReadOnlyFocused(v); }
void TerminalTabs::searchFocused(const QString &q) { if (auto *p = active()) p->searchFocused(q); }
void TerminalTabs::searchNext() { if (auto *p = active()) p->searchNext(); }
void TerminalTabs::searchPrev() { if (auto *p = active()) p->searchPrev(); }
void TerminalTabs::clearSearch() { if (auto *p = active()) p->clearSearch(); }
void TerminalTabs::copyHoverLinkFocused() { if (auto *p = active()) p->copyHoverLinkFocused(); }
void TerminalTabs::openHoverLinkFocused() { if (auto *p = active()) p->openHoverLinkFocused(); }
bool TerminalTabs::wantsMouseReportFocused(int qtModifiers) const
{
    auto *p = active();
    return p && p->wantsMouseReportFocused(qtModifiers);
}

// ─── style forwarding ───────────────────────────────────────────────────────────────────────────

void TerminalTabs::applyStyle(TerminalPanes *p)
{
    p->setFontFamily(m_fontFamily);
    p->setFontSize(m_fontSize);
    p->setBackground(m_background);
    p->setForeground(m_foreground);
    p->setAnsiPalette(m_palette);
    p->setScrollbackLimit(m_scrollbackLimit);
    p->setHandleColor(m_handleColor);
    p->setBackgroundImage(m_bgImage);
    p->setBackgroundOpacity(m_bgOpacity);
    p->setBlur(m_blur);
    p->setScrollbarMode(m_scrollbarMode);
    p->setAnimateSplits(m_animateSplits);
    p->setSplitGap(m_splitGap);
    p->setFocusOnHover(m_focusOnHover);
    p->setReservedSequences(m_reserved);
}

#define TABS_STYLE_SETTER(Setter, Member, Type, PaneSetter) \
    void TerminalTabs::Setter(Type v) { \
        if (Member == v) return; \
        Member = v; \
        for (const Tab &t : std::as_const(m_tabs)) t.panes->PaneSetter(v); \
        emit styleChanged(); \
    }
TABS_STYLE_SETTER(setFontFamily, m_fontFamily, const QString &, setFontFamily)
TABS_STYLE_SETTER(setFontSize, m_fontSize, int, setFontSize)
TABS_STYLE_SETTER(setBackground, m_background, const QColor &, setBackground)
TABS_STYLE_SETTER(setForeground, m_foreground, const QColor &, setForeground)
TABS_STYLE_SETTER(setAnsiPalette, m_palette, const QStringList &, setAnsiPalette)
TABS_STYLE_SETTER(setScrollbackLimit, m_scrollbackLimit, int, setScrollbackLimit)
TABS_STYLE_SETTER(setHandleColor, m_handleColor, const QColor &, setHandleColor)
TABS_STYLE_SETTER(setBackgroundImage, m_bgImage, const QString &, setBackgroundImage)
TABS_STYLE_SETTER(setBackgroundOpacity, m_bgOpacity, qreal, setBackgroundOpacity)
TABS_STYLE_SETTER(setBlur, m_blur, int, setBlur)
TABS_STYLE_SETTER(setScrollbarMode, m_scrollbarMode, const QString &, setScrollbarMode)
TABS_STYLE_SETTER(setAnimateSplits, m_animateSplits, bool, setAnimateSplits)
TABS_STYLE_SETTER(setSplitGap, m_splitGap, int, setSplitGap)
TABS_STYLE_SETTER(setFocusOnHover, m_focusOnHover, bool, setFocusOnHover)
#undef TABS_STYLE_SETTER

void TerminalTabs::setReservedSequences(const QStringList &v)
{
    if (m_reserved == v)
        return;
    m_reserved = v;
    for (const Tab &t : std::as_const(m_tabs))
        t.panes->setReservedSequences(v);
    emit reservedChanged();
}

// ─── pane drag & drop routing ───────────────────────────────────────────────────────────────────

void TerminalTabs::itemChange(ItemChange change, const ItemChangeData &data)
{
    if (change == ItemSceneChange) {
        if (m_filteredWindow)
            m_filteredWindow->removeEventFilter(this);
        m_filteredWindow = data.window;
        if (m_filteredWindow)
            m_filteredWindow->installEventFilter(this);
    }
    QQuickItem::itemChange(change, data);
}

bool TerminalTabs::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_filteredWindow) {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove:
            if (routeDrag(static_cast<QDropEvent *>(event), false))
                return true;
            break;
        case QEvent::Drop:
            if (routeDrag(static_cast<QDropEvent *>(event), true))
                return true;
            break;
        case QEvent::DragLeave:
            if (TerminalPanes::dragInProgress()) {
                if (auto *p = active())
                    p->dragClear();
                showTabDropIndicator(false);
            }
            break;
        default:
            break;
        }
    }
    return QQuickItem::eventFilter(watched, event);
}

bool TerminalTabs::routeDrag(QDropEvent *event, bool drop)
{
    TerminalView *src = TerminalPanes::dragSource(event->mimeData());
    if (!src)
        return false; // not ours (or another process's pane: a pty cannot cross processes)
    const QPointF scene = event->position();
    TerminalPanes *panes = active();
    const QPointF local = panes ? panes->mapFromScene(scene) : QPointF();
    if (panes && QRectF(0, 0, panes->width(), panes->height()).contains(local)) {
        showTabDropIndicator(false);
        if (drop)
            panes->dragDrop(src, local);
        else
            panes->dragHover(src, local);
    } else {
        if (panes)
            panes->dragClear();
        // The chrome strip above the panes (header / tab bar) = "make it a tab of THIS window" —
        // pointless for a pane that already is the only one of a tab here.
        const qreal top = mapToScene(QPointF(0, 0)).y();
        TerminalPanes *owner = TerminalPanes::ownerOf(src);
        const bool alreadyATab = owner && owner->parentItem() == this && owner->count() == 1;
        const bool tabZone = scene.y() >= 0 && scene.y() < top && !alreadyATab;
        if (drop) {
            if (tabZone)
                TerminalPanes::recordNewTabDrop(this);
            else
                TerminalPanes::recordNoopDrop();
        } else {
            showTabDropIndicator(tabZone);
        }
    }
    if (drop)
        TerminalPanes::hideAllDropIndicators();
    event->setDropAction(Qt::MoveAction);
    event->accept();
    return true;
}

void TerminalTabs::showTabDropIndicator(bool on)
{
    QQuickWindow *w = window();
    if (!on || !w) {
        if (m_tabDropOverlay)
            m_tabDropOverlay->setVisible(false);
        return;
    }
    if (!m_tabDropOverlay) {
        QQmlEngine *eng = qmlEngine(this);
        if (!eng)
            return;
        QQmlComponent c(eng);
        // Same look as the split-zone indicator (TerminalPanes' DropZone).
        c.setData(QByteArrayLiteral("import QtQuick\nRectangle { visible: false; z: 99999; color: \"#403584e4\"; "
                                    "border.color: \"#3584e4\"; border.width: 2; radius: 4 }"),
                  QUrl(QStringLiteral("qrc:/solidterm/TabDropZone.qml")));
        m_tabDropOverlay = qobject_cast<QQuickItem *>(c.create(qmlContext(this)));
        if (!m_tabDropOverlay)
            return;
        m_tabDropOverlay->setParent(this);
    }
    const qreal top = mapToScene(QPointF(0, 0)).y();
    m_tabDropOverlay->setParentItem(w->contentItem());
    m_tabDropOverlay->setX(2);
    m_tabDropOverlay->setY(2);
    m_tabDropOverlay->setWidth(qMax<qreal>(0, w->width() - 4));
    m_tabDropOverlay->setHeight(qMax<qreal>(0, top - 4));
    m_tabDropOverlay->setVisible(true);
}

void TerminalTabs::hideAllTabDropIndicators()
{
    for (TerminalTabs *t : std::as_const(s_allTabs))
        t->showTabDropIndicator(false);
}

void TerminalTabs::adoptViewAsTab(TerminalView *v)
{
    if (!v)
        return;
    TerminalPanes::adoptOnNextCreate(v); // the new tab's container adopts the live view as pane 1
    TerminalPanes *panes = makePanes();
    TerminalPanes::adoptOnNextCreate(nullptr);
    if (!panes)
        return;
    m_tabs.append({ panes, v->title(), QString() });
    selectTab(m_tabs.size() - 1);
}
