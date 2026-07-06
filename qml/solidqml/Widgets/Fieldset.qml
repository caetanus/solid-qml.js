// Fieldset — the <fieldset> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the wrapper).
// A plain CssFill box (cssPrimitive "fieldset") — no Templates control (T.GroupBox is chrome we'd
// null anyway); the border/box look is entirely author CSS. The <legend> stays a Css.CssText
// primitive: the transpiler hoists it to the FRONT of the children so it paints first, then passes
// the ordered children (legend + the rest) into this component's default `data`.
//
// The transpiler emits:  W.Fieldset { cssClass: […]; <legend CssText first>; <other children> }
import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    cssPrimitive: "fieldset"
    // Children (the hoisted legend + everything else) land in the default `data` and lay out
    // in document order, exactly like the old inline emit.
}
