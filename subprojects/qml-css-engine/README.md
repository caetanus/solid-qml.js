# qml-css-engine

**A real CSS engine for Qt Quick, 100% C++.** Style native scene-graph items with
plain CSS — cascade, specificity, `!important`, descendant selectors scoped by the
real ancestor chain, `:hover` on any element, `@media`, `@keyframes`, `@font-face`
(remote, cached), transitions that actually animate — and a C++ layout engine
(flex, grid, block, `calc()`/`min`/`max`/`clamp`, `vw`/`vh`, absolute positioning,
`overflow-y` scrolling via a composed `Flickable`).

The stylesheet stays live at runtime: hot reload on file change, runtime override
layers for theme switching (`loadLayeredString`), and every restyle re-resolves
through the same cascade. No build-time flattening.

## Components (`import qmlcss 1.0`)

C++ types, registered by one call:

```cpp
#include "qmlcss/QMLCss.h"
QmlCss::registerTypes(); // → import qmlcss 1.0
```

`CssRect` (box: paint + layout container), `CssText`, `CssFill`, `CssFillLayer`,
`CssImage`, `CssIcon`, `CssHr`, `CssItem`, `CssDropShadow`, `CssKeyframes`,
`CssIncubator` (async subtree mount, atomic reveal), `CssRepeater` (keyed flyweight
repeater — reorders move delegates instead of recreating), and the `Contrast`
singleton (WCAG utilities).

## Performance model

Cost follows the CSS, decided per (re)apply:

- **Shape × Rectangle policy** — rectangle-safe styles paint with a real, batchable
  `QQuickRectangle`; gradients/shadows/per-side borders compose the full Shape shell.
  The verdict swaps live when the style changes.
- **Lazy composition** — paint shells, effects, hover tracking, scrolling and
  animation drivers only exist once the style demands them; layout-only boxes carry
  no paint machinery at all.
- **One compile per snippet** (`QmlCss::cachedComponent`), **one write per style
  apply** (single `cssIn` map), **indexed + memoized selector matching**, **layout
  hibernation** for bulk passes (N applies → one flush), and a **resize fast-path**
  (`@media`-signature check — retiles are free).

Run any consumer with `SQ_MOUNT_STATS=1` for per-phase mount timing and apply-origin
counters.

## Consuming

Meson subproject (used by `solid-qml.js` and `qbar`):

```meson
qmlcss = subproject('qml-css-engine')
qml_css_engine_dep = qmlcss.get_variable('qml_css_engine_dep')
```

Provide two context properties — `cssTheme` (a `CssTheme`) and `cssLayout`
(a `CssLayoutEngine`) — and give styled items the CSS signature
(`cssId` / `cssClass` / `cssState` / `cssPrimitive` / `style`).

Full documentation under `docs/`: overview & performance architecture, CSS support
matrix, component reference, C++ API, integration guide.
