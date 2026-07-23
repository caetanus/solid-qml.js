#include "terminaltabs.h"

#include "terminalpanes.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>

#include <utility>

// The tab BAR lives in the TSX now; this class is headless — it only keeps the live panes and shows
// one by index. The model (titles + active) is pushed to Solid via tabsChanged().

TerminalTabs::TerminalTabs(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
}

TerminalTabs::~TerminalTabs() = default;

TerminalPanes *TerminalTabs::active() const
{
    return (m_active >= 0 && m_active < m_tabs.size()) ? m_tabs[m_active].panes : nullptr;
}

void TerminalTabs::emitModel()
{
    QStringList titles;
    titles.reserve(m_tabs.size());
    for (const Tab &t : m_tabs)
        titles.append(t.title);
    emit tabsChanged(titles, m_active);
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
    applyStyle(panes);

    connect(panes, &TerminalPanes::accelerator, this, &TerminalTabs::accelerator);
    connect(panes, &TerminalPanes::searchChanged, this, [this, panes](int idx, int count) {
        if (active() == panes)
            emit searchChanged(idx, count);
    });
    connect(panes, &TerminalPanes::unsafePasteRequested, this, &TerminalTabs::unsafePasteRequested);
    connect(panes, &TerminalPanes::titleChanged, this, [this, panes](const QString &t) {
        const int i = [&] { for (int k = 0; k < m_tabs.size(); ++k) if (m_tabs[k].panes == panes) return k; return -1; }();
        if (i < 0)
            return;
        m_tabs[i].title = t;
        emitModel();
        if (i == m_active)
            emit titleChanged(t);
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
        emit allClosed();
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
        emit titleChanged(m_tabs[index].title);
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
void TerminalTabs::copyFocused() { if (auto *p = active()) p->copyFocused(); }
void TerminalTabs::pasteFocused() { if (auto *p = active()) p->pasteFocused(); }
void TerminalTabs::pasteTextFocused(const QString &t) { if (auto *p = active()) p->pasteTextFocused(t); }
void TerminalTabs::clearFocused() { if (auto *p = active()) p->clearFocused(); }
void TerminalTabs::resetFocused() { if (auto *p = active()) p->resetFocused(); }
void TerminalTabs::searchFocused(const QString &q) { if (auto *p = active()) p->searchFocused(q); }
void TerminalTabs::searchNext() { if (auto *p = active()) p->searchNext(); }
void TerminalTabs::searchPrev() { if (auto *p = active()) p->searchPrev(); }
void TerminalTabs::clearSearch() { if (auto *p = active()) p->clearSearch(); }

// ─── style forwarding ───────────────────────────────────────────────────────────────────────────

void TerminalTabs::applyStyle(TerminalPanes *p)
{
    p->setFontFamily(m_fontFamily);
    p->setFontSize(m_fontSize);
    p->setBackground(m_background);
    p->setForeground(m_foreground);
    p->setScrollbackLimit(m_scrollbackLimit);
    p->setHandleColor(m_handleColor);
    p->setBackgroundImage(m_bgImage);
    p->setBackgroundOpacity(m_bgOpacity);
    p->setEmboss(m_emboss);
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
TABS_STYLE_SETTER(setScrollbackLimit, m_scrollbackLimit, int, setScrollbackLimit)
TABS_STYLE_SETTER(setHandleColor, m_handleColor, const QColor &, setHandleColor)
TABS_STYLE_SETTER(setBackgroundImage, m_bgImage, const QString &, setBackgroundImage)
TABS_STYLE_SETTER(setBackgroundOpacity, m_bgOpacity, qreal, setBackgroundOpacity)
TABS_STYLE_SETTER(setEmboss, m_emboss, bool, setEmboss)
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
