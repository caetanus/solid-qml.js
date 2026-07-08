#include "select.h"

#include "snippetwidget.h"

namespace {

// Original Select.qml internals — `root` = the C++ wrapper.
const char *kSelectBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.ComboBox {
    id: ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    model: root.model
    onCurrentIndexChanged: root.currentIndex = currentIndex
    onActivated: (index) => root.activated(index)
    // Qt::Popup semantics (owner directive): the dropdown vanishes when the app window loses
    // focus — a native popup does not linger over other applications.
    Window.onActiveChanged: if (!Window.active) ctl.popup.close()
    // No visual chrome from Templates; the CssFill wrapper owns the box painting.
    background: null
    // leftPadding keeps the contentItem text clear of the border.
    leftPadding: 12

    Binding on currentIndex {
        value: root.currentIndex
        restoreMode: Binding.RestoreNone
    }

    // contentItem: CssText showing the selected item's display label.
    contentItem: Css.CssText {
        cssPrimitive: ""
        cssClass: ["value"]
        text: ctl.displayText
    }
    // Chevron: absolutely positioned at the right-centre of the ComboBox.
    Css.CssText {
        cssPrimitive: ""
        cssClass: ["chevron"]
        text: "▾"
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
    }
    // Delegate: one T.ItemDelegate per model row.
    delegate: T.ItemDelegate {
        id: optDel
        // The style must bind highlighted itself (Basic does the same) — without it keyboard
        // navigation moves highlightedIndex invisibly and the active row never changes.
        highlighted: ctl.highlightedIndex === index
        // Width must be explicit: ComboBox does not size delegates automatically.
        width: ctl.popup.width
        implicitHeight: 36
        background: Css.CssFill {
            cssPrimitive: "div"
            cssClass: ["option"]
            cssState: (optDel.highlighted ? ["hover"] : []).concat(ctl.currentIndex === index ? ["selected"] : [])
        }
        contentItem: Css.CssText {
            cssPrimitive: ""
            cssClass: ["option-label"]
            text: modelData
        }
    }
    // Popup: T.Popup below the control; padding ≥ border-width prevents clip. Templates popups
    // have NO implicit-size policy of their own (that's the style's job, and we ARE the style) —
    // without the implicitHeight line the popup opens 0px tall.
    popup: T.Popup {
        popupType: T.Popup.Item
        y: (ctl.mapToItem(null, 0, ctl.height + 2).y + height > (ctl.Window.height || Screen.height)) ? -(height + 2) : ctl.height + 2
        width: ctl.width
        implicitHeight: contentHeight + topPadding + bottomPadding
        padding: 1
        // cssAncestor: re-anchor the engine's ancestor walk at the control (overlay reparenting
        // severs the `.wg-select .popup` chain). background and contentItem are SIBLING slots;
        // every popup descendant's walk passes through one of them.
        background: Css.CssFill {
            property Item cssAncestor: ctl
            cssPrimitive: "div"
            cssClass: ["popup"]
        }
        contentItem: ListView {
            property Item cssAncestor: ctl
            clip: true
            model: ctl.delegateModel
            currentIndex: ctl.highlightedIndex
            implicitHeight: Math.min(contentHeight, 240)
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

Select::Select(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("select"));
    connect(this, &QQuickItem::enabledChanged, this, &Select::syncState);
}

void Select::setModel(const QVariant &v)
{
    if (m_model == v)
        return;
    m_model = v;
    emit modelChanged();
}

void Select::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void Select::setValues(const QVariant &v)
{
    if (m_values == v)
        return;
    m_values = v;
    emit valuesChanged();
}

void Select::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-select"), kSelectBody);
    if (m_control) {
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
    syncState();
}

void Select::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
