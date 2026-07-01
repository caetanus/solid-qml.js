# qml-css-engine

Shared Qt/QML CSS engine. Parses CSS (cascade, specificity, `!important`, gradients,
box-shadow, transitions, `@keyframes`, transforms, `@media`) and applies it to Qt Quick
items via a reverse-slot (`CssTheme::loadCss`), AND does layout (the C++ `CssLayoutEngine`:
flex/grid/block, calc/min/max/clamp, vw/vh, padding/margin/aspect-ratio, absolute, plus
transform-keyframe animation).

QML primitives under `qrc:/qmlcss`: `CssRect` (Shape fill: solid/gradient/border/shadow +
layout container), `CssText`, `CssFill`, `CssFillLayer`, `CssIcon`, `CssItem`,
`CssDropShadow`, `CssKeyframes`.

Consumers provide a `cssTheme` context property (a `CssTheme`) and a `cssLayout`
(`CssLayoutEngine`). Used by `solid-qml-native` and `qbar` as a **meson subproject**:

```meson
qmlcss = subproject('qml-css-engine')
qml_css_engine_dep = qmlcss.get_variable('qml_css_engine_dep')
```

Styled objects carry: `cssId`, `cssClass`, `cssState`, `cssPart`, `cssPrimitive`, `style`.
