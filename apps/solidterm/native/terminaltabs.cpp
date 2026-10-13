#include "terminaltabs.h"

#include "terminalpanes.h"
#include "terminalview.h"
#include "tmuxsession.h"
#include "glyphcache.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDropEvent>
#include <QGuiApplication>
#include <QUrl>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QProcess>

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
    // Closing a tmux window detaches: the session and its programs live on in tmux (5.1).
    if (m_tmux)
        m_tmux->detach();
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
        if (!t.hidden)
            titles.append(t.effective());
    const int active = visibleIndex(m_active);
    // Only real changes: the TSX tab bar's Repeater binds its model to these titles, and an identical
    // re-emission from inside its own (re)layout — the bar appearing shrinks this item, whose first
    // geometry selects the active tab again — reads as a binding loop.
    if (m_modelEmitted && titles == m_lastTitles && active == m_lastActive)
        return;
    m_modelEmitted = true;
    m_lastTitles = titles;
    m_lastActive = active;
    emit tabsChanged(titles, active);
}

void TerminalTabs::setTabTitle(int visible, const QString &title)
{
    const int index = realIndex(visible);
    if (index < 0)
        return;
    if (m_tmux && !title.isEmpty()) // a custom title persists in tmux (2.4)
        m_tmux->renameWindow(m_tabs[index].tmuxWindow, title);
    m_tabs[index].customTitle = title;
    emitModel();
    if (index == m_active)
        emit titleChanged(windowTitleFor(index));
}

QString TerminalTabs::tabTitle(int visible) const
{
    const int index = realIndex(visible);
    return index >= 0 ? m_tabs[index].effective() : QString();
}

QVariantList TerminalTabs::overview() const
{
    QVariantList out;
    for (int t = 0; t < m_tabs.size(); ++t) {
        if (m_tabs[t].hidden)
            continue;
        const QStringList titles = m_tabs[t].panes->paneTitles();
        const int focused = m_tabs[t].panes->focusedPaneIndex();
        for (int p = 0; p < titles.size(); ++p) {
            QVariantMap row;
            row["tab"] = visibleIndex(t);
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

void TerminalTabs::focusPane(int visibleTab, int pane)
{
    const int tab = realIndex(visibleTab);
    if (tab < 0)
        return;
    selectTab(visibleTab);
    m_tabs[tab].panes->focusLeafByIndex(pane);
}

TerminalPanes *TerminalTabs::makePanes()
{
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return nullptr;
    if (m_isTmux)
        TerminalPanes::setPendingTmux(m_tmux); // the container starts empty and follows a layout
    if (!m_panesComponent) {
        m_panesComponent = new QQmlComponent(eng, this);
        m_panesComponent->setData(QByteArrayLiteral("import SolidTerm 1.0\nTerminalPanes {}"),
                                  QUrl(QStringLiteral("qrc:/solidterm/TabPanes.qml")));
    }
    auto *panes = qobject_cast<TerminalPanes *>(m_panesComponent->create(qmlContext(this)));
    TerminalPanes::setPendingTmux(nullptr);
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
            emit titleChanged(windowTitleFor(i));
    });
    connect(panes, &TerminalPanes::allClosed, this, [this, panes] {
        for (int k = 0; k < m_tabs.size(); ++k)
            if (m_tabs[k].panes == panes) { closeAt(k); return; }
    });
    return panes;
}

void TerminalTabs::componentComplete()
{
    QQuickItem::componentComplete();
    if (!s_pendingTmux.isNull()) {
        // `--tmux <name>`: the tabs are the session's windows, built as tmux reports them.
        m_isTmux = true;
        const QString name = s_pendingTmux;
        s_pendingTmux = QString();
        startTmux(name, s_pendingTmuxAttachOnly);
        QMetaObject::invokeMethod(this, &TerminalTabs::emitTmuxState, Qt::QueuedConnection);
        return;
    }
    newTab(); // the first tab
}

void TerminalTabs::newTab()
{
    if (m_isTmux) { // server-driven: the new window arrives as %window-add (2.5)
        if (m_tmux)
            m_tmux->newWindow(m_tabs.value(m_active).tmuxWindow);
        return;
    }
    TerminalPanes *panes = makePanes();
    if (!panes)
        return;
    Tab t;
    t.panes = panes;
    m_tabs.append(t);
    activateTab(m_tabs.size() - 1);
}

void TerminalTabs::closeTab(int visible)
{
    const int index = realIndex(visible);
    if (index < 0)
        return;
    if (m_isTmux) { // owner decision 2: closing a tmux tab HIDES it (its programs keep running)
        if (m_tmux)
            m_tmux->hideWindow(m_tabs[index].tmuxWindow);
        return;
    }
    closeAt(index);
}

void TerminalTabs::closeAt(int index)
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
    activateTab(m_active);
}

// Public API: indexes are VISIBLE tab indexes (the TSX tab bar). In tmux mode hidden tabs (owner
// decision 2) exist but are not shown; locally there are none, so visible == real.
int TerminalTabs::realIndex(int visible) const
{
    for (int k = 0, v = 0; k < m_tabs.size(); ++k) {
        if (m_tabs[k].hidden)
            continue;
        if (v++ == visible)
            return k;
    }
    return -1;
}

int TerminalTabs::visibleIndex(int real) const
{
    if (real < 0 || real >= m_tabs.size() || m_tabs[real].hidden)
        return -1;
    int v = 0;
    for (int k = 0; k < real; ++k)
        v += m_tabs[k].hidden ? 0 : 1;
    return v;
}

int TerminalTabs::visibleCount() const
{
    int n = 0;
    for (const Tab &t : m_tabs)
        n += t.hidden ? 0 : 1;
    return n;
}

void TerminalTabs::selectTab(int index)
{
    const int real = realIndex(index);
    if (real < 0)
        return;
    activateTab(real);
    // tmux: the window becomes the session's current one too (other clients follow; 2.4).
    if (m_tmux)
        m_tmux->selectWindow(m_tabs[real].tmuxWindow);
}

void TerminalTabs::activateTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    m_active = index;
    m_tabs[index].hidden = false; // showing a tab un-hides it
    for (int k = 0; k < m_tabs.size(); ++k)
        m_tabs[k].panes->setVisible(k == index);
    relayout();
    emitModel();
    if (auto *p = active()) {
        QMetaObject::invokeMethod(p, "refocus", Qt::QueuedConnection); // keyboard focus → its pane
        emit titleChanged(windowTitleFor(index));
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
    if (m_tmux && width() > 0 && height() > 0) {
        qreal cw, ch;
        GlyphCache::cellMetrics(m_fontFamily, m_fontSize, window() ? window()->effectiveDevicePixelRatio() : 1.0, &cw, &ch);
        m_tmux->setArea(width(), height(), cw, ch);
    }
    if (!m_didInitialFocus && width() > 0 && height() > 0 && active()) {
        m_didInitialFocus = true;
        activateTab(m_active);
    }
}

void TerminalTabs::focusInEvent(QFocusEvent *)
{
    if (auto *p = active())
        p->setFocus(true);
}

// ─── proxied pane ops ───────────────────────────────────────────────────────────────────────────

void TerminalTabs::split(int orient) { if (auto *p = active()) p->split(orient); } // no-op on a tmux tab (phase 3)
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
void TerminalTabs::copyLink(const QString &url)
{
    if (url.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(url);
    QGuiApplication::clipboard()->setText(url, QClipboard::Selection);
}
void TerminalTabs::openLink(const QString &url)
{
    if (!url.isEmpty())
        QDesktopServices::openUrl(QUrl::fromUserInput(url));
}
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
    // tmux ↔ local, and between tmux sessions, is refused: a tmux pane moves by tmux command within
    // its session (2.5); a local pane's pty cannot enter tmux.
    TerminalPanes *srcOwner = TerminalPanes::ownerOf(src);
    TerminalTabs *srcTabs = srcOwner ? qobject_cast<TerminalTabs *>(srcOwner->parentItem()) : nullptr;
    const bool srcTmux = srcOwner && srcOwner->isTmux();
    if (srcTmux != m_isTmux || (m_isTmux && (!srcTabs || srcTabs->tmuxSession() != m_tmux))) {
        if (drop)
            TerminalPanes::recordNoopDrop();
        event->setDropAction(Qt::IgnoreAction);
        event->accept();
        return true;
    }
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
    Tab t;
    t.panes = panes;
    t.title = v->title();
    m_tabs.append(t);
    activateTab(m_tabs.size() - 1);
}

// ─── tmux mode ──────────────────────────────────────────────────────────────────────────────────

TmuxSession *TerminalTabs::tmuxSession() const
{
    return m_tmux;
}

QString TerminalTabs::windowTitleFor(int index) const
{
    const QString t = (index >= 0 && index < m_tabs.size()) ? m_tabs[index].effective() : QString();
    return m_tmux ? m_tmux->windowTitle(t) : t;
}

int TerminalTabs::tmuxTabIndex(int window) const
{
    for (int k = 0; k < m_tabs.size(); ++k)
        if (m_tabs[k].tmuxWindow == window)
            return k;
    return -1;
}

TerminalPanes *TerminalTabs::tmuxTab(int window, bool create, bool hidden)
{
    const int i = tmuxTabIndex(window);
    if (i >= 0)
        return m_tabs[i].panes;
    if (!create || !m_isTmux)
        return nullptr;
    TerminalPanes *panes = makePanes();
    if (!panes)
        return nullptr;
    Tab t;
    t.panes = panes;
    t.tmuxWindow = window;
    t.hidden = hidden;
    m_tabs.append(t);
    if (!hidden && visibleCount() == 1)
        activateTab(m_tabs.size() - 1);
    else {
        panes->setVisible(false);
        relayout();
        emitModel();
    }
    return panes;
}

void TerminalTabs::tmuxRemoveTab(int window)
{
    const int index = tmuxTabIndex(window);
    if (index < 0)
        return;
    TerminalPanes *panes = m_tabs[index].panes;
    if (panes)
        panes->tmuxParkAll(); // a pane that moved on keeps its view (2.2)
    m_tabs.removeAt(index);
    if (panes)
        panes->deleteLater();
    if (m_tabs.isEmpty()) {
        m_active = 0;
        emitModel();
        return; // the session ends with its last window (detach-on-destroy): %exit closes us
    }
    if (m_active >= m_tabs.size())
        m_active = m_tabs.size() - 1;
    else if (index < m_active)
        --m_active;
    activateVisibleNear(m_active);
}

// After the active tab went away or was hidden: the nearest visible tab (none → the window stays
// as it is; TmuxSession detaches when the last visible tab is hidden).
void TerminalTabs::activateVisibleNear(int index)
{
    for (int d = 0; d < m_tabs.size(); ++d) {
        for (const int k : { index + d, index - d }) {
            if (k >= 0 && k < m_tabs.size() && !m_tabs[k].hidden) {
                const bool changed = k != m_active;
                activateTab(k);
                if (changed && m_tmux)
                    m_tmux->selectWindow(m_tabs[k].tmuxWindow);
                return;
            }
        }
    }
    emitModel();
}

void TerminalTabs::tmuxSetHidden(int window, bool hidden)
{
    const int i = tmuxTabIndex(window);
    if (i < 0 || m_tabs[i].hidden == hidden)
        return;
    m_tabs[i].hidden = hidden;
    if (hidden) {
        m_tabs[i].panes->setVisible(false);
        if (i == m_active)
            activateVisibleNear(i);
        else
            emitModel();
    } else {
        activateTab(i);
        if (m_tmux)
            m_tmux->selectWindow(window);
    }
}

void TerminalTabs::tmuxRenameTab(int window, const QString &name)
{
    const int i = tmuxTabIndex(window);
    if (i < 0)
        return;
    m_tabs[i].tmuxName = name;
    emitModel();
    if (i == m_active)
        emit titleChanged(windowTitleFor(i));
}

void TerminalTabs::tmuxSelectTab(int window)
{
    const int i = tmuxTabIndex(window);
    if (i >= 0 && i != m_active && !m_tabs[i].hidden) // a hidden tab stays hidden
        activateTab(i);
}

QList<int> TerminalTabs::tmuxWindows() const
{
    QList<int> out;
    for (const Tab &t : m_tabs)
        out.append(t.tmuxWindow);
    return out;
}

QString TerminalTabs::tmuxTabName(int window) const
{
    const int i = tmuxTabIndex(window);
    return i >= 0 ? m_tabs[i].effective() : QString();
}

void TerminalTabs::tmuxSessionEnded()
{
    m_tmux = nullptr; // nothing to detach any more
    if (QQuickWindow *w = window())
        w->close();
}

bool TerminalTabs::tmuxTabHidden(int window) const
{
    const int i = tmuxTabIndex(window);
    return i >= 0 && m_tabs[i].hidden;
}

int TerminalTabs::activeTmuxWindow() const
{
    return m_tabs.value(m_active).tmuxWindow;
}

void TerminalTabs::emitTmuxState()
{
    emit tmuxStateChanged(m_isTmux, m_tmux ? m_tmux->hidden() : QVariantList());
}

QVariantList TerminalTabs::hiddenPanes() const
{
    return m_tmux ? m_tmux->hidden() : QVariantList();
}

void TerminalTabs::restoreHidden(int window)
{
    if (m_tmux)
        m_tmux->restoreHidden(window);
}

void TerminalTabs::killFocused()
{
    if (auto *p = active())
        p->killFocused();
}

void TerminalTabs::killTab()
{
    if (m_tmux && active())
        m_tmux->killWindow(m_tabs[m_active].tmuxWindow);
}

void TerminalTabs::leaveTmuxModeFocused()
{
    if (auto *p = active())
        p->leaveTmuxModeFocused();
}

// ─── lifecycle (phase 4) ─────────────────────────────────────────────────────────────────────────

void TerminalTabs::startTmux(const QString &name, bool attachOnly)
{
    m_tmux = new TmuxSession(name, this, attachOnly);
    m_tmuxName = m_tmux->name();
    connect(m_tmux, &TmuxSession::hiddenChanged, this, &TerminalTabs::emitTmuxState);
    if (width() > 0 && height() > 0) {
        qreal cw, ch;
        GlyphCache::cellMetrics(m_fontFamily, m_fontSize, window() ? window()->effectiveDevicePixelRatio() : 1.0, &cw, &ch);
        m_tmux->setArea(width(), height(), cw, ch);
    }
    if (QQuickWindow *w = window())
        w->setProperty("solidtermTmuxSession", m_tmuxName);
}

void TerminalTabs::tmuxSessionLost(const QString &text, bool canReattach)
{
    m_tmux = nullptr;
    for (const Tab &t : std::as_const(m_tabs))
        for (TerminalView *v : t.panes->views())
        {
            v->setReadOnly(true); // the pane's program is out of reach; its output stays readable
            v->setTmuxInputHook(nullptr, nullptr); // the session object goes away
        }
    emit tmuxBanner(text, canReattach);
}

void TerminalTabs::tmuxReattach()
{
    if (!m_isTmux || m_tmux)
        return;
    for (const Tab &t : std::as_const(m_tabs))
        if (t.panes)
            t.panes->deleteLater();
    m_tabs.clear();
    m_active = 0;
    emitModel();
    emit tmuxBanner(QString(), false);
    startTmux(m_tmuxName, false);
    emitTmuxState();
}

void TerminalTabs::tmuxDetach()
{
    closeWindow(); // ~TerminalTabs detaches
}

void TerminalTabs::tmuxKillSession()
{
    if (m_tmux)
        m_tmux->killSession();
}

void TerminalTabs::closeWindow()
{
    if (QQuickWindow *w = window())
        w->close();
}

void TerminalTabs::openTmuxSession(const QString &name)
{
    if (s_openTmuxWindow && !name.isEmpty())
        s_openTmuxWindow(name, true);
}

void TerminalTabs::newTmuxSession(const QString &name)
{
    if (s_openTmuxWindow)
        s_openTmuxWindow(name.trimmed().isEmpty() ? QStringLiteral("main") : name.trimmed(), false);
}

void TerminalTabs::refreshTmuxSessions()
{
    // One-shot `tmux list-sessions` (8): the picker's list. #{@solidterm} marks the sessions
    // SolidTerm created (decision 3).
    auto *p = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("TMUX"));
    p->setProcessEnvironment(env);
    connect(p, &QProcess::finished, this, [this, p] {
        QVariantList out;
        const QStringList lines = QString::fromLocal8Bit(p->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &l : lines) {
            const QStringList f = l.split(QLatin1Char('\t'));
            if (f.size() < 3)
                continue;
            QVariantMap m;
            m[QStringLiteral("name")] = f[0];
            m[QStringLiteral("attached")] = f[1].toInt();
            m[QStringLiteral("ours")] = f[2] == QLatin1String("1");
            m[QStringLiteral("current")] = f[0] == m_tmuxName && m_isTmux;
            out.append(m);
        }
        emit tmuxSessionsChanged(out);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, p, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit tmuxSessionsChanged({});
            p->deleteLater();
        }
    });
    p->start(TmuxSession::binary(), { QStringLiteral("list-sessions"), QStringLiteral("-F"),
                                      QStringLiteral("#{session_name}\t#{session_attached}\t#{@solidterm}") });
}
