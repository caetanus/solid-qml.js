#include "terminaltabs.h"

#include "terminalpanes.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <functional>

static constexpr qreal kBarH = 34.0;

// The tab strip: painted tabs with an elided title + close ×, and a trailing + button. Callbacks
// (std::function, like DividerItem) instead of signals → no moc for this class. Global scope so the
// header can forward-declare it for TerminalTabs::m_bar.
class TabBar : public QQuickPaintedItem {
public:
    explicit TabBar(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent)
    {
        setAcceptedMouseButtons(Qt::LeftButton);
    }

    QStringList titles;
    int active = 0;
    QColor barBg{ "#151515" }, tabFg{ "#cfcfcf" }, activeBg{ "#1e1e1e" }, accent{ "#3584e4" };
    std::function<void(int)> onTabClicked, onCloseClicked;
    std::function<void()> onNewTab;

    void setModel(const QStringList &t, int a) { titles = t; active = a; update(); }

    void paint(QPainter *p) override
    {
        p->setRenderHint(QPainter::Antialiasing, true);
        p->fillRect(boundingRect(), barBg);
        const Layout L = layout();
        const QFontMetricsF fm(p->font());
        for (int i = 0; i < titles.size(); ++i) {
            const QRectF r = L.tab[i];
            const bool act = i == active;
            QPainterPath path;
            path.addRoundedRect(r.adjusted(0, 3, 0, 6), 7, 7); // round top, overshoot bottom
            p->fillPath(path, act ? activeBg : QColor(0, 0, 0, 0));
            if (act) {
                p->fillRect(QRectF(r.left() + 6, r.top() + 2, r.width() - 12, 2), accent);
            }
            p->setPen(act ? tabFg : QColor(tabFg.red(), tabFg.green(), tabFg.blue(), 150));
            const QRectF textR = r.adjusted(11, 0, -26, 0);
            const QString t = fm.elidedText(titles[i].isEmpty() ? QStringLiteral("terminal") : titles[i],
                                            Qt::ElideRight, textR.width());
            p->drawText(textR, Qt::AlignVCenter | Qt::AlignLeft, t);
            // close ×
            const QRectF c = L.close[i];
            p->setPen(QPen(QColor(tabFg.red(), tabFg.green(), tabFg.blue(), act ? 200 : 120), 1.4));
            const qreal m = 4.5;
            p->drawLine(c.left() + m, c.top() + m, c.right() - m, c.bottom() - m);
            p->drawLine(c.right() - m, c.top() + m, c.left() + m, c.bottom() - m);
        }
        // + button
        p->setPen(QPen(QColor(tabFg.red(), tabFg.green(), tabFg.blue(), 200), 1.6));
        const QRectF pl = L.plus;
        const QPointF ctr = pl.center();
        p->drawLine(ctr.x() - 6, ctr.y(), ctr.x() + 6, ctr.y());
        p->drawLine(ctr.x(), ctr.y() - 6, ctr.x(), ctr.y() + 6);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        const Layout L = layout();
        const QPointF pos = e->position();
        for (int i = 0; i < titles.size(); ++i) {
            if (L.close[i].contains(pos)) { if (onCloseClicked) onCloseClicked(i); e->accept(); return; }
        }
        for (int i = 0; i < titles.size(); ++i) {
            if (L.tab[i].contains(pos)) { if (onTabClicked) onTabClicked(i); e->accept(); return; }
        }
        if (L.plus.contains(pos)) { if (onNewTab) onNewTab(); e->accept(); return; }
        e->accept();
    }

private:
    struct Layout { QVector<QRectF> tab, close; QRectF plus; };
    Layout layout() const
    {
        Layout L;
        const qreal h = height();
        const int n = titles.size();
        const qreal gap = 2, plusW = 34;
        const qreal avail = qMax<qreal>(0, width() - plusW - 4);
        const qreal tabW = n > 0 ? qMin<qreal>(190, avail / n - gap) : 0;
        qreal x = 2;
        for (int i = 0; i < n; ++i) {
            const QRectF r(x, 0, tabW, h);
            L.tab.append(r);
            L.close.append(QRectF(r.right() - 22, r.center().y() - 9, 18, 18));
            x += tabW + gap;
        }
        L.plus = QRectF(x + 2, (h - 26) / 2, 26, 26);
        return L;
    }
};

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
    connect(panes, &TerminalPanes::titleChanged, this, [this, panes](const QString &t) {
        const int i = [&] { for (int k = 0; k < m_tabs.size(); ++k) if (m_tabs[k].panes == panes) return k; return -1; }();
        if (i < 0)
            return;
        m_tabs[i].title = t;
        syncBar();
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
    m_bar = new TabBar(this);
    auto *bar = static_cast<TabBar *>(m_bar);
    bar->onTabClicked = [this](int i) { selectTab(i); };
    bar->onCloseClicked = [this](int i) { closeTab(i); };
    bar->onNewTab = [this] { newTab(); };
    newTab(); // the first tab
}

void TerminalTabs::newTab()
{
    TerminalPanes *panes = makePanes();
    if (!panes)
        return;
    m_tabs.append({ panes, QString() });
    selectTab(m_tabs.size() - 1);
    emit tabsChanged();
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
    emit tabsChanged();
}

void TerminalTabs::selectTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    m_active = index;
    for (int k = 0; k < m_tabs.size(); ++k)
        m_tabs[k].panes->setVisible(k == index);
    relayout();
    syncBar();
    if (auto *p = active()) {
        QMetaObject::invokeMethod(p, "refocus", Qt::QueuedConnection); // keyboard focus → its pane
        emit titleChanged(m_tabs[index].title);
    }
}

void TerminalTabs::syncBar()
{
    if (!m_bar)
        return;
    auto *bar = static_cast<TabBar *>(m_bar);
    QStringList titles;
    for (const Tab &t : m_tabs)
        titles.append(t.title);
    bar->barBg = m_handleColor;
    bar->tabFg = m_foreground;
    bar->activeBg = m_background;
    bar->setModel(titles, m_active);
    bar->setVisible(m_tabs.size() > 1); // a lone tab needs no strip
}

void TerminalTabs::relayout()
{
    const bool showBar = m_tabs.size() > 1;
    const qreal barH = showBar ? kBarH : 0;
    if (m_bar) {
        m_bar->setX(0); m_bar->setY(0);
        m_bar->setWidth(width()); m_bar->setHeight(kBarH);
    }
    for (const Tab &t : m_tabs) {
        t.panes->setX(0); t.panes->setY(barH);
        t.panes->setWidth(width()); t.panes->setHeight(qMax<qreal>(0, height() - barH));
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
void TerminalTabs::copyFocused() { if (auto *p = active()) p->copyFocused(); }
void TerminalTabs::pasteFocused() { if (auto *p = active()) p->pasteFocused(); }
void TerminalTabs::clearFocused() { if (auto *p = active()) p->clearFocused(); }

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
        syncBar(); \
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
