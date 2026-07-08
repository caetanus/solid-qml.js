#include "datefield.h"

#include "snippetwidget.h"

namespace {

// Original DateField.qml internals — `root` = the C++ wrapper; the snippet root is the anchored
// plain-Item host (skipped by the wrapper's flex pass; anchors hold inside), with the T.Popup as
// a resource.
const char *kDateFieldBody = R"(import QtQuick
import QtQuick.Templates as T
import solidqml.Widgets 1.0 as W
import qmlcss 1.0 as Css

Item {
    id: host
    anchors.fill: parent

    // Qt::Popup semantics: vanish when the app window loses focus.
    Window.onActiveChanged: if (!Window.active) pop.close()

    function __step(days) {
        var b = root.cursor instanceof Date ? root.cursor : (root.value instanceof Date ? root.value : new Date())
        var d = new Date(b.getFullYear(), b.getMonth(), b.getDate() + days)
        root.cursor = d
        grid.viewMonth = d.getMonth()
        grid.viewYear = d.getFullYear()
    }
    function __commit() {
        if (!(root.cursor instanceof Date)) return
        root.dayPicked(root.cursor)
        pop.close()
    }

    // ReadOnly text field — display only; typing dates is out of scope.
    T.TextField {
        id: field
        anchors.fill: parent
        background: null
        readOnly: true
        activeFocusOnTab: solidTabstop.enabled
        // Enter/Space/Down open the popup; with it open, arrows move the cursor and Enter/Space
        // commit it. The popup keeps focus on this field (focus: false default), so keys land here.
        Keys.onReturnPressed: pop.visible ? host.__commit() : pop.open()
        Keys.onSpacePressed: pop.visible ? host.__commit() : pop.open()
        Keys.onDownPressed: pop.visible ? host.__step(7) : pop.open()
        Keys.onUpPressed: { if (pop.visible) host.__step(-7) }
        Keys.onLeftPressed: { if (pop.visible) host.__step(-1) }
        Keys.onRightPressed: { if (pop.visible) host.__step(1) }
        color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
        font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
        font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
        leftPadding: 12
        rightPadding: 36
        verticalAlignment: TextInput.AlignVCenter
    }
    // Binding: keep the displayed text in sync with the value, formatted as ISO date.
    // Guard: Qt.formatDate throws on null/undefined (no Date object yet).
    Binding {
        target: field
        property: "text"
        value: root.value instanceof Date ? Qt.formatDate(root.value, "yyyy-MM-dd") : ""
        restoreMode: Binding.RestoreNone
    }
    // Calendar glyph — IDENTICAL to the <select> chevron (same class, same CSS box) so the two
    // dropdowns align. Living inside the anchored host keeps it out of the wrapper's layout pass.
    Css.CssText {
        cssPrimitive: ""
        cssClass: ["chevron"]
        text: "▾"
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
    }
    // MouseArea over the whole field: click toggles the popup. CloseOnPressOutside fires on the
    // PRESS, so by the time the click lands here the popup already closed — a naive visible-check
    // reopens it. The popup stamps its close time; a click right after a close is a toggle-close.
    MouseArea {
        anchors.fill: parent
        onClicked: { field.forceActiveFocus(); if (pop.visible) pop.close(); else if (Date.now() - pop.__closedAt > 150) pop.open() }
    }

    // Popup: T.Popup renders on the window overlay; padding ≥ 1 prevents CssFill border clip.
    T.Popup {
        id: pop
        property double __closedAt: 0
        onOpened: root.cursor = root.value instanceof Date ? root.value : new Date()
        onClosed: { __closedAt = Date.now(); root.cursor = null }
        onVisibleChanged: root.popupVisible = visible
        // In-scene overlay popup, flip on WINDOW overflow (Popup.Window mis-positions on Wayland once
        // the app window floats).
        popupType: T.Popup.Item
        y: (root.mapToItem(null, 0, root.height + 2).y + height > (root.Window.height || Screen.height)) ? -(height + 2) : root.height + 2
        // Templates popups have no implicit-size policy (style's job — ours): without these two lines
        // the calendar dropdown opens 0x0.
        implicitWidth: contentWidth + leftPadding + rightPadding
        implicitHeight: contentHeight + topPadding + bottomPadding
        padding: 1
        // cssAncestor: overlay reparenting severs the visual chain — re-anchor at the date wrapper so
        // `.wg-date .popup` / `.wg-date .day` keep matching (background and contentItem are SIBLINGS).
        background: Css.CssFill {
            property Item cssAncestor: root
            cssPrimitive: "div"
            cssClass: ["popup"]
        }
        contentItem: W.MonthGrid {
            id: grid
            property Item cssAncestor: root
            selectedDate: root.value
            cursorDate: root.cursor
            onDayPicked: (date) => { root.dayPicked(date); pop.close() }
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

DateField::DateField(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("input"));
    setImplicitWidth(200);
    setImplicitHeight(36);
    connect(this, &QQuickItem::enabledChanged, this, &DateField::syncState);
}

void DateField::setValue(const QVariant &v)
{
    if (m_value == v)
        return;
    m_value = v;
    emit valueChanged();
}

void DateField::setCursorValue(const QVariant &v)
{
    if (m_cursor == v)
        return;
    m_cursor = v;
    emit cursorChanged();
}

void DateField::setPopupVisible(bool v)
{
    if (m_popupVisible == v)
        return;
    m_popupVisible = v;
    emit popupVisibleChanged();
    syncState();
}

void DateField::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-datefield"), kDateFieldBody);
    syncState();
}

void DateField::syncState()
{
    QVariantList state;
    if (m_popupVisible)
        state << QStringLiteral("focus");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
