#include "icon.h"

#include "snippetwidget.h"

namespace {

// `root` = the C++ wrapper (Icon). Css.CssIcon already resolves `fallbackIconName` to
// `image://themeicon/<name>|<hexcolor>` (cssicon.cpp) and recolours the loaded pixmap via a REAL
// QtQuick.Effects MultiEffect (`color` drives `colorizationColor`) — so it re-tints with the
// cascade's inherited colour (light/dark, :hover/:active, …) instead of a baked-in one. Auto-
// sized to min(width, height) unless `iconSize` is set explicitly — a CSS `width`/`height` on the
// `<icon>` (e.g. 16px) is enough to size it correctly; no separate sizing prop to manage.
const char *kIconBody = R"(import qmlcss 1.0 as Css
Css.CssIcon {
    anchors.fill: parent
    fallbackIconName: root.name
    color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
}
)";

} // namespace

namespace SolidWidgets {

Icon::Icon(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
}

void Icon::setName(const QString &v)
{
    if (m_name == v)
        return;
    m_name = v;
    emit nameChanged();
}

void Icon::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_icon = composeInternal(this, content(), QStringLiteral("solidwidgets-icon"), kIconBody);
}

} // namespace SolidWidgets
