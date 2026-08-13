#include "tabbutton.h"

#include "snippetwidget.h"

#include <QQmlContext>

namespace {

// Original TabButton.qml slot internals — `root` = the C++ control.
const char *kTabBackground = R"(import qmlcss 1.0 as Css
Css.CssFill {
    cssPrimitive: "div"
    cssClass: ["tab"]
    cssState: (root.checked ? ["selected"] : []).concat(root.hovered ? ["hover"] : []).concat(root.activeFocus ? ["focus"] : [])
}
)";

const char *kTabLabel = R"(import qmlcss 1.0 as Css
Css.CssText {
    cssPrimitive: ""
    cssClass: ["tab-label"]
    cssState: (root.checked ? ["selected"] : []).concat(root.hovered ? ["hover"] : []).concat(root.activeFocus ? ["focus"] : [])
    text: root.text
}
)";

} // namespace

namespace SolidWidgets {

TabButton::TabButton(QQuickItem *parent)
    : QQuickTabButton(parent)
{
    setPadding(8);
    // Templates default to NoFocus — a CLICKED tab must take keyboard focus (QTabBar model), or
    // arrows after a click go nowhere near the bar's Keys handlers. ClickFocus only: the
    // tab-chain stays on the syncTabstop line (StrongFocus would overwrite it).
    setFocusPolicy(Qt::ClickFocus);
    connect(this, &QQuickAbstractButton::checkedChanged, this, &TabButton::syncTabstop);
    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    connect(this, &QQuickControl::implicitBackgroundWidthChanged, this, &TabButton::syncImplicit);
    connect(this, &QQuickControl::implicitBackgroundHeightChanged, this, &TabButton::syncImplicit);
    connect(this, &QQuickControl::implicitContentWidthChanged, this, &TabButton::syncImplicit);
    connect(this, &QQuickControl::implicitContentHeightChanged, this, &TabButton::syncImplicit);
    connect(this, &QQuickControl::paddingChanged, this, &TabButton::syncImplicit);
}

void TabButton::componentComplete()
{
    QQuickTabButton::componentComplete();
    setBackground(createBoundItem(this, QStringLiteral("solidwidgets-tabbutton-bg"), kTabBackground));
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-tabbutton-label"), kTabLabel));
    if (QQmlContext *ctx = qmlContext(this))
        m_tabstop = ctx->contextProperty(QStringLiteral("solidTabstop")).value<QObject *>();
    if (m_tabstop)
        connect(m_tabstop, SIGNAL(enabledChanged()), this, SLOT(syncTabstop()));
    syncTabstop();
    syncImplicit();
}

void TabButton::syncTabstop()
{
    // Only the CURRENT (checked) tab is a tab stop; the others are reached with arrows.
    setActiveFocusOnTab((!m_tabstop || m_tabstop->property("enabled").toBool()) && isChecked());
}

void TabButton::syncImplicit()
{
    setImplicitWidth(qMax(implicitBackgroundWidth() + leftInset() + rightInset(),
                          implicitContentWidth() + leftPadding() + rightPadding()));
    setImplicitHeight(qMax(implicitBackgroundHeight() + topInset() + bottomInset(),
                           implicitContentHeight() + topPadding() + bottomPadding()));
}

} // namespace SolidWidgets
