// Text — the text-primitive component (solidqml.Widgets). One cached type serves every text run
// (<text>/<span>/<h1>…<h6>/<p>/<cite>); the transpiler sets cssPrimitive for non-"text" tags and
// binds `text`. See Div.qml for the rationale (compile once, reuse/AOT vs. inline per occurrence).
import QtQuick
import qmlcss 1.0 as Css

Css.CssText {
    cssPrimitive: "text"
}
