#include "details.h"

#include "snippetwidget.h"

#include <QQmlListReference>

namespace {

// Original Details.qml summary header row — `root` = the C++ wrapper. The marker glyph is a
// plain Text inside an anchors.fill Item host (flex-pass insulation — once the author gives the
// summary box rules the engine's flex pass hits plain children too); rotates 90° while open.
// `.marker` CSS lands via the nested CssItem. The MouseArea toggles the disclosure and doubles
// as the hover tracker, like the Button component.
const char *kSummaryBody = R"(import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    cssClass: root.summaryClass
    cssPrimitive: "summary"
    cssState: (root.__open ? ["open"] : []).concat(__ma.containsMouse ? ["hover"] : [])
    Item {
        anchors.fill: parent
        Text {
            text: "▸"
            rotation: root.__open ? 90 : 0
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            Css.CssItem { cssPrimitive: "text"; cssClass: ["marker"] }
        }
    }
    MouseArea {
        id: __ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.__toggle()
    }
}
)";

// Original Details.qml disclosure body: only while open (invisible items leave the layout).
const char *kBodyBody = R"(import QtQuick
import qmlcss 1.0 as Css

Css.CssRect {
    cssPrimitive: "div"
    cssClass: ["content"]
    visible: root.__open
}
)";

} // namespace

namespace SolidWidgets {

Details::Details(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("details"));
}

void Details::setOpen(bool v)
{
    if (m_open == v)
        return;
    m_open = v;
    emit openChanged();
    // Init-binding semantics: `__open` follows `open` until the first user toggle.
    if (!m_userToggled)
        setEffectiveOpen(v);
}

void Details::setSummaryClass(const QVariant &v)
{
    if (m_summaryClass == v)
        return;
    m_summaryClass = v;
    emit summaryClassChanged();
}

void Details::__toggle()
{
    m_userToggled = true;
    setEffectiveOpen(!m_effectiveOpen);
}

void Details::setEffectiveOpen(bool v)
{
    if (m_effectiveOpen == v)
        return;
    m_effectiveOpen = v;
    emit effectiveOpenChanged();
    syncState();
}

void Details::syncState()
{
    QVariantList state;
    if (m_effectiveOpen)
        state << QStringLiteral("open");
    setCssState(state);
}

QQmlListProperty<QObject> Details::summaryContent()
{
    return QQmlListProperty<QObject>(this, nullptr, summary_append, nullptr, nullptr, nullptr);
}

QQmlListProperty<QObject> Details::contentSlot()
{
    return QQmlListProperty<QObject>(this, nullptr, content_append, nullptr, nullptr, nullptr);
}

void Details::summary_append(QQmlListProperty<QObject> *prop, QObject *obj)
{
    auto *self = static_cast<Details *>(prop->object);
    self->appendTo(self->m_summary, obj);
}

void Details::content_append(QQmlListProperty<QObject> *prop, QObject *obj)
{
    auto *self = static_cast<Details *>(prop->object);
    self->appendTo(self->m_body, obj);
}

void Details::appendTo(Slot &slot, QObject *obj)
{
    if (slot.box) {
        QQmlListReference content(slot.box, "content");
        content.append(obj);
        return;
    }
    slot.pending.append(obj);
}

void Details::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    // Seed the disclosure state before the snippets bind to __open.
    m_effectiveOpen = m_open;
    syncState();
    m_summary.box = composeInternal(this, content(), QStringLiteral("solidwidgets-details-summary"), kSummaryBody);
    m_body.box = composeInternal(this, content(), QStringLiteral("solidwidgets-details-body"), kBodyBody);
    for (Slot *slot : {&m_summary, &m_body}) {
        if (!slot->box)
            continue;
        QQmlListReference content(slot->box, "content");
        for (QObject *obj : std::as_const(slot->pending))
            content.append(obj);
        slot->pending.clear();
    }
}

} // namespace SolidWidgets
