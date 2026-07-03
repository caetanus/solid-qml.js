# Architecture overview

`qml-css-engine` has three layers that work together. Understanding how they interact
makes integration and debugging straightforward.

## The CSS parser — `CssTheme`

`CssTheme` (C++, `src/qmlcss/csstheme.h`) is the entry point. It:

1. Reads one or more `.css` files with `loadLayered()`, recursively expanding `@import`
   statements and substituting `@define-color` variables.
2. Strips comments, extracts `@keyframes` blocks, and pulls `@media` blocks aside.
3. Parses the remaining rules (selector + declaration block) into an in-memory cascade,
   tracking specificity and source order.
4. On `setViewport()` (call this when the window resizes), re-evaluates `@media`
   conditions and rebuilds the active rule set.
5. On every load or reload, calls `reapplyAll()` — the **reverse slot**: it pushes the
   newly-resolved `style` map into every registered item's `style` property directly,
   without any QML binding. No stale binding, no re-registration needed.

A component registers itself by calling `cssTheme.loadCss(this)` at construction.
Thereafter the engine re-pushes styles on theme reload and on `cssClass`/`cssState`
changes (observed via the property's NOTIFY signal).

### Cascade model

Matching rules are sorted by (specificity, source order) and merged; `!important`
declarations are applied on top in a second pass. Specificity follows the CSS3 model:
id selectors score 100, class/pseudo-class selectors 10, element type selectors 1.

### Supported selector forms

| Form | Example |
|------|---------|
| Universal | `* { … }` |
| ID | `#battery { … }` |
| Class | `.panel { … }` |
| ID + class | `#battery.charging { … }` |
| Pseudo-class | `#button:hover { … }` |
| Pseudo-element | `#workspaces button::after { … }` |
| Descendant | `#workspaces button { … }` |
| Descendant + state | `#workspaces button.focused { … }` |
| Element type (in context) | `button { … }` (only via `resolveWith`) |

### At-rules

| Rule | Support |
|------|---------|
| `@import` | Recursive, relative to the importing file; cycle-guarded |
| `@define-color name value;` | GTK-style variable; cross-reference resolved |
| `@media (min/max-width/height: …px)` | Live; re-evaluated on `setViewport()` |
| `@keyframes name { … }` | Stored by name; consumed by `CssRect` and `CssKeyframes` |
| `@supports`, `@container` | Recognised and dropped (not evaluated) |

## The layout engine — `CssLayoutEngine`

`CssLayoutEngine` (C++, `src/qmlcss/csslayout.h`) resolves box-model layout.
It is separate from `CssTheme` so it only runs on `QQuickItem`s and never touches
the CSS parsing path.

When a `CssRect`'s geometry, style, or children change, it calls
`cssLayout.requestLayout(root, contentHolder)`. The engine coalesces all pending
requests into a single pass per event-loop turn (a zero-interval `QTimer`).

### What the engine computes

- **Padding box** — `padding`, `padding-{top,right,bottom,left}`
- **Margin** — `margin`, `margin-{top,right,bottom,left}` (per child)
- **Size** — `width`, `height` in `px`, `%`, `calc()`, `min()`, `max()`, `clamp()`,
  `vw`, `vh`, `vmin`, `vmax`; `auto` falls back to the item's `implicitWidth/Height`
- **Aspect ratio** — `aspect-ratio: w/h` or bare number
- **Flexbox** — `display: flex` / `inline-flex`, `flex-direction` (row/column),
  `flex-grow`, `flex`, `justify-content`, `align-items`, `align-self`, `gap`,
  `row-gap`, `column-gap`
- **Grid** — `display: grid`, `grid-template-columns`, `grid-template-rows` (with
  `fr`, `auto`, `px`, `%`, `repeat()`, `minmax()`, `min-content`, `max-content`),
  `gap`, `column-gap`, `row-gap`, `justify-items`, `align-items`
- **Block** — `display: block` stacks children vertically (default)
- **Absolute positioning** — `position: absolute`, `top`/`right`/`bottom`/`left`,
  `inset` shorthand
- **Transform animation** — `buildAnimStops()` normalises `@keyframes` transform
  frames; `applyAnim()` interpolates and writes `_animRotate`/`_animScale`/
  `_animTx`/`_animTy` onto the root item for `CssRect`'s `NumberAnimation`

Children are included in the layout only when they carry a `style` or `cssPrimitive`
property (i.e., they are `CssRect`, `CssText`, etc.). `display: none` removes a child
from both view and layout.

## QML components — `import qmlcss 1.0` (C++ types)

The components are compiled QML files embedded as Qt resources. They are accessed via:

```qml
import qmlcss 1.0 as Css
```

They expect two **context properties** to be present in the QML engine:

| Context property | C++ type | Purpose |
|------------------|----------|---------|
| `cssTheme` | `CssTheme *` | CSS parsing and style resolution |
| `cssLayout` | `CssLayoutEngine *` | Box-model layout |

Each component that participates in the CSS system carries the **CssQmlItem
signature** — a set of identity properties that `CssTheme::loadCss()` reads to resolve
and push the right rules:

| Property | Type | Purpose |
|----------|------|---------|
| `cssId` | `string` | CSS `#id` selector |
| `cssAlternateId` | `var` (string or list) | Waybar-compat alias; merged under `cssId` |
| `cssClass` | `var` (string or list) | State classes (`.focused`, `.active`, …) |
| `cssPrimitive` | `string` | `"rect"`, `"text"`, or `"item"` — selects CSS→QML mapping |
| `cssPart` | `string` | Named sub-part; resolves only `.part` rules, not bare `#id` |
| `style` | `var` | Written by the engine; components bind their rendering to this |

See [Components](components.md) for the full component reference.

## How the three layers fit together

```
  Your CSS file
       │
       ▼
  CssTheme (C++)
  ─ parses rules, @keyframes, @media
  ─ pushes style map → item.style  (reverse slot)
  ─ watches files for hot-reload
       │
       ├─── style map ──────────────────────────────────────────┐
       │                                                         ▼
       │                                                   CssRect / CssText
       │                                                   ─ renders fill/border/shadow
       │                                                   ─ reads style for layout triggers
       │
       └─── viewport (vw/vh) ──► CssLayoutEngine (C++)
                                  ─ evaluates calc/min/max/clamp
                                  ─ runs flex/grid/block pass
                                  ─ assigns x/y/width/height to children
                                  ─ drives @keyframes transform animation
```

## Performance architecture

The engine is built so that **cost follows the CSS**, decided at runtime on every
(re)apply — the live stylesheet stays the source of truth (no build-time flattening).

- **Shape × Rectangle policy** — a resolved style that only needs solid colour +
  radius (uniform or per-corner) + a uniform border composes a REAL `QQuickRectangle`
  (the scene graph's batchable rect node). Gradients, `url()` backgrounds,
  `box-shadow`, per-side borders and %-radii take the full Shape shell. The verdict
  re-evaluates per apply and swaps the composition live (a `:hover` or theme change can
  move a box between paths).
- **Lazy composition everywhere** — the paint shell itself, the fill, borders, bevels,
  the shadow effect stack, text shadows, text backgrounds, `Translate`, the
  `@keyframes` driver, hover tracking and the scroll `Flickable` are only composed
  when the style first demands them. A layout-only `<div>` costs a `CssRect` and its
  content holder — nothing else.
- **Compiled-component cache** — every composed snippet is compiled ONCE per engine
  (`QmlCss::cachedComponent`) instead of per instance.
- **Single-shot mount** — the style is resolved and applied BEFORE the render subtree
  exists, so the first evaluation of every composed binding reads final values; style
  pushes ride ONE `cssIn` map (one write per apply, each derived binding evaluates once).
- **Selector index + memoization** — rules are bucketed by subject class/element/id and
  resolution is memoized per input signature (cleared when the active rules change);
  `resolveFontFamily` is memoized against QFontDatabase.
- **Layout hibernation** — bulk style passes (`reapplyAll`, descendant sweeps) bracket
  themselves with `CssLayoutEngine::beginBatch()/endBatch()`: N applies, one layout
  flush. Outside a batch the flush stays synchronous (the resize stale-frame guarantee).
- **Resize fast-path** — `setViewport()` only rebuilds and re-applies when the SET of
  matching `@media` groups changes; a tiling-WM retile within the same breakpoints
  costs nothing.
- **Async mounts + keyed lists** — `CssIncubator` streams a subtree in without blocking
  the frame (revealed atomically with a short fade); `CssRepeater` reconciles its model
  by value, so reorders MOVE existing delegates instead of recreating them.

Instrumentation: run any consumer with `SQ_MOUNT_STATS=1` to dump per-phase mount
timing, apply origins and per-class apply counts at exit.
