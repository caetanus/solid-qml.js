// MenuItem — a single row in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A T.MenuItem with CSS support, only ever instantiated as a child of W.Menu (its root
// lands in the menu's contentData → contentModel). `text` is inherited from T.AbstractButton (a FINAL
// property — must NOT be redeclared); the transpiler sets it directly (W.MenuItem { text: "…" }).
//
// The transpiler emits:  W.MenuItem { text: "…"; onTriggered: { … } }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.MenuItem {
    id: ctl
    // The menu highlights the hovered/keyboard-current row via `highlighted`; hoverEnabled makes
    // mouse rows current (Controls styles rely on the same).
    hoverEnabled: true
    implicitWidth: Math.max(180, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: 32
    leftPadding: 12
    rightPadding: 12

    // Desktop affordance: a pointing-hand cursor over the row (HoverHandler doesn't steal the press,
    // so highlight/activation still work through it).
    HoverHandler { cursorShape: Qt.PointingHandCursor }

    background: Css.CssFill {
        cssPrimitive: "div"
        cssClass: ["option"]
        cssState: ctl.highlighted ? ["hover"] : []
    }
    // Mnemonic marker: `&N` underlines N like every desktop toolkit (`&&` is a literal ampersand).
    // The label opts into Text.StyledText so the <u> renders; author text is entity-escaped.
    // Functional Alt+letter activation is the keyboard-model pass.
    function __mnemonicMarkup(s) {
        var out = "";
        for (var i = 0; i < s.length; i++) {
            var c = s.charAt(i);
            if (c === "&" && i + 1 < s.length) {
                i++;
                out += (s.charAt(i) === "&") ? "&amp;" : "<u>" + s.charAt(i) + "</u>";
            } else if (c === "<") {
                out += "&lt;";
            } else if (c === ">") {
                out += "&gt;";
            } else {
                out += c;
            }
        }
        return out;
    }
    contentItem: Css.CssText {
        cssPrimitive: ""
        cssClass: ["option-label"]
        styledText: true
        text: ctl.__mnemonicMarkup(ctl.text)
    }
}
