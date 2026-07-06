// MenuSeparator — a divider row in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A T.MenuSeparator whose contentItem is a 1px `.sep` CssRect. Only ever instantiated as
// a child of W.Menu. padding ≥ the .sep height keeps the rule off the popup's border.
//
// The transpiler emits:  W.MenuSeparator { }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.MenuSeparator {
    implicitWidth: 180
    implicitHeight: 9
    padding: 4
    contentItem: Css.CssRect {
        cssPrimitive: "div"
        cssClass: ["sep"]
        implicitHeight: 1
    }
}
