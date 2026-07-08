#include "toggles.h"

#include "snippetwidget.h"

namespace {

// Original Checkbox.qml internals: T.CheckBox + the 20×20 ✓ indicator. `root` = the C++ wrapper;
// the RestoreNone Binding forwards the wrapper's controlled `checked` into the control.
const char *kCheckboxBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.CheckBox {
    id: box
    anchors.fill: parent
    background: null
    contentItem: null
    activeFocusOnTab: solidTabstop.enabled
    onToggled: { root.checked = checked; root.toggled(); }

    indicator: Css.CssFill {
        cssPrimitive: "span"
        cssClass: ["indicator"]
        cssState: root.cssState
        width: 20
        height: 20
        implicitWidth: 20
        implicitHeight: 20
        Item {
            anchors.fill: parent
            Text {
                text: "✓"
                visible: box.checked
                anchors.centerIn: parent
                Css.CssItem { cssPrimitive: "text"; cssClass: ["indicator-glyph"] }
            }
        }
    }

    Binding on checked {
        value: root.checked
        restoreMode: Binding.RestoreNone
    }
}
)";

// Original Toggle.qml internals: T.Switch + the 36×20 track with the sliding knob.
const char *kToggleBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Switch {
    id: sw
    anchors.fill: parent
    background: null
    contentItem: null
    activeFocusOnTab: solidTabstop.enabled
    onToggled: { root.checked = checked; root.toggled(); }

    indicator: Css.CssFill {
        cssPrimitive: "span"
        cssClass: ["track"]
        cssState: root.cssState
        width: 36
        height: 20
        implicitWidth: 36
        implicitHeight: 20
        Item {
            anchors.fill: parent
            Rectangle {
                width: 16
                height: 16
                radius: 8
                color: "#ffffff"
                y: (parent.height - height) / 2
                x: sw.visualPosition * (parent.width - width)
                Behavior on x { NumberAnimation { duration: 120 } }
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["knob"] }
            }
        }
    }

    Binding on checked {
        value: root.checked
        restoreMode: Binding.RestoreNone
    }
}
)";

} // namespace

namespace SolidWidgets {

ToggleBase::ToggleBase(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("input"));
    connect(this, &QQuickItem::enabledChanged, this, &ToggleBase::syncState);
}

void ToggleBase::setChecked(bool v)
{
    if (m_checked == v)
        return;
    m_checked = v;
    emit checkedChanged();
    syncState();
}

void ToggleBase::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), snippetKey(), snippet());
    if (m_control) {
        // Focus lives on the control (Space toggles there); mirror it into `:focus`.
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
    }
    syncState();
}

void ToggleBase::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_checked)
        state << QStringLiteral("checked");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

Checkbox::Checkbox(QQuickItem *parent)
    : ToggleBase(parent)
{
    setImplicitWidth(20);
    setImplicitHeight(20);
}

const char *Checkbox::snippet() const { return kCheckboxBody; }

Toggle::Toggle(QQuickItem *parent)
    : ToggleBase(parent)
{
    setImplicitWidth(36);
    setImplicitHeight(20);
}

const char *Toggle::snippet() const { return kToggleBody; }

} // namespace SolidWidgets
