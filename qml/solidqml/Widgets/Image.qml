// Image — the <img> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component; the transpiler INSTANTIATES these, it no longer hand-emits the image's internals).
// Extends the engine's CssImage, which does object-fit + a rounded-rect clip via MultiEffect — so
// `border-radius` yields a real circular avatar. The .qml is just the CssImage + a `src` slot.
//
// The transpiler emits:  W.Image { cssClass: […]; src: <expr> || "" }
import QtQuick
import qmlcss 1.0 as Css

Css.CssImage {
    id: root
    // The image URL (the <img src> attribute). CssImage.source is a QUrl; a string coerces.
    property url src: ""
    source: root.src
}
