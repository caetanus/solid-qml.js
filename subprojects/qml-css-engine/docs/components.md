# QML component reference

All components are available after:

```qml
import "qrc:/qmlcss" as Css
```

They expect the `cssTheme` (`CssTheme *`) and `cssLayout` (`CssLayoutEngine *`) context
properties to be registered before the QML engine loads any QML.

---

## CssQmlItem signature

Every component that participates in the CSS system exposes the following set of
identity properties. `CssTheme::loadCss()` reads them to resolve and push styles.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `cssId` | `string` | `""` | CSS `#id` selector for this element |
| `cssAlternateId` | `var` | `[]` | Waybar-compat alias: string or string list; merged below `cssId` in the cascade |
| `cssClass` | `var` | `[]` | Authored state classes (`.focused`, `.active`, …); string or list |
| `cssPrimitive` | `string` | component-specific | Tells the engine which CSS→QML mapping to use (`"rect"`, `"text"`, `"item"`) |
| `cssPart` | `string` | `""` | Named sub-part (`"graph"`, `"item"`, …); resolves only rules that require this class, excluding the bare `#id` base |
| `style` | `var` | `{}` | **Written by the engine.** Do not bind — the engine owns this property |

An element is registered with the engine when `cssId`, `cssPart`, or `cssClass` is
non-empty. Elements used as plain renderers (no CSS identity) simply skip registration.

---

## `CssRect`

**Inherits:** `Item`  
**`cssPrimitive`:** `"rect"`

The primary CSS box: a painted rectangle that is also a flex/grid/block layout
container. Use `CssRect` for any element that needs a fill, border, shadow, or layout.

### Identity and style properties

All CssQmlItem signature properties plus:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `cssState` | `var` | `["hover"]` when hovered | Runtime state classes merged with `cssClass`; driven automatically by the built-in `HoverHandler` |
| `radius` | `real` | `0` | Corner radius fallback when `border-radius` is not set in CSS |
| `defaultColor` | `color` | `transparent` | Fill colour fallback |
| `defaultBorderColor` | `color` | `transparent` | Border colour fallback |
| `defaultBorderWidth` | `real` | `0` | Border width fallback |

### CSS-driven behaviour

| CSS property | Effect |
|-------------|--------|
| `background-color` | Solid fill |
| `background` | Solid colour, `linear-gradient()`, `radial-gradient()`, or multi-layer (comma-separated) |
| `border`, `border-color`, `border-width` | Stroke around the shape (independent `Shape` so border alpha is independent of fill alpha) |
| `border-radius` | 1–4 values, percentage; corners are true `PathArc` quarter-ellipses |
| `box-shadow` | Outset → drop shadow via `QtQuick.Effects.MultiEffect`; two inset shadows → 4-sided bevel; one inset shadow → directional edge band |
| `display: none` | `visible: false` |
| `transition` | Animated fill colour and opacity fade with `ColorAnimation`/`NumberAnimation` |
| `transform` | Static `rotate()`, `scale()`, `scaleX()`, `scaleY()`, `translate()`, `translateX()`, `translateY()` |
| `animation` | Looping `@keyframes` transform animation via `CssLayoutEngine.buildAnimStops/applyAnim` |
| `display: flex/inline-flex/grid/block` | Layout mode for children (see [CSS support](css-support.md)) |
| All flex/grid/box-model properties | Resolved and applied by `CssLayoutEngine` |

### CSS text inheritance

`CssRect` exposes read-only inherited properties that flow from parent to child without
explicit CSS classes on the child:

`inheritedColor`, `inheritedFontFamily`, `inheritedFontSize`, `inheritedFontWeight`,
`inheritedLetterSpacing`, `inheritedTextTransform`, `inheritedTextAlign`.

`CssText` reads these automatically, so an unstyled label inside a `CssRect` picks up
the container's colour and font.

### Content slot

```qml
Css.CssRect {
    cssId: "card"

    // Children go here — they are laid out by CssLayoutEngine
    Css.CssText { text: "Title" }
    Css.CssRect { cssId: "icon" }
}
```

The `default` property alias routes children into an inner `contentHolder` `Item`.
The layout engine positions children within the padding box.

---

## `CssText`

**Inherits:** `Text`  
**`cssPrimitive`:** `"text"`

A CSS-styled text label. Reads colour, font, and text properties from its own `style`
or, when absent, from the nearest CSS-inheriting ancestor (`CssRect`).

### Identity and style properties

All CssQmlItem signature properties plus:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `defaultColor` | `color` | `black` | Text colour fallback |
| `defaultFontFamily` | `string` | `"Sans Serif"` | Font family fallback |
| `defaultFontSize` | `real` | `11` | Font size fallback (points) |

### CSS-driven behaviour

| CSS property | Effect on `Text` |
|-------------|-----------------|
| `color` | `color` property |
| `font-family` | `font.family` |
| `font-size` | `font.pointSize` (px→pt conversion, em/rem, bare pt) |
| `font-weight` | `font.weight` (normal/bold/100–900) |
| `letter-spacing` | `font.letterSpacing` |
| `text-transform` | `font.capitalization` (uppercase/lowercase/capitalize) |
| `text-align` | `horizontalAlignment` (left/center/right) |
| `text-shadow` | `layer.effect: CssDropShadow` |
| `display: none` | `visible: false` |

---

## `CssFill`

**Inherits:** `Item`

Compatibility shim over `CssRect` that adds `background-image: url(…)` support.
The image is rendered behind a `CssRect` that handles all other paint.

**New code should use `CssRect` directly.** `CssFill` exists for callers that need
`url()` image backgrounds.

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `style` | `var` | `{}` | Resolved style map (same shape as `CssRect.style`) |
| `radius` | `real` | `0` | Corner radius fallback |
| `defaultColor` | `color` | `transparent` | Fill fallback |
| `defaultBorderColor` | `color` | `transparent` | Border fallback |
| `defaultBorderWidth` | `real` | `0` | Border width fallback |

CSS properties consumed: `background`, `background-image` (url), `background-size`
(`cover`, `contain`, `stretch`), `background-image-opacity` (custom), plus everything
`CssRect` handles. `CssFill` does **not** carry the CssQmlItem signature itself; it is
typically driven by an explicit `style` from a parent that owns the identity.

---

## `CssFillLayer`

**Inherits:** `Shape`

Internal component used by `CssRect` to render one layer of a multi-layer or
radial-gradient `background`. Exposed publicly so custom containers can stack layers.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `spec` | `var` | Layer descriptor from `CssTheme.parseGradientLayers()`: `{ type: "color"\|"linear"\|"radial", color?, angle?, cx?, cy?, stops? }` |
| `radii` | `var` | `[topLeft, topRight, bottomRight, bottomLeft]` corner radii (already clamped) |

---

## `CssIcon`

**Inherits:** `Item`

An image/icon component that reads its source from the CSS `icon`/`icon-image` or
`icon-name` property and applies colour tinting via `MultiEffect`.

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `style` | `var` | `{}` | Style map; consumed for `icon`, `icon-image`, `icon-name` |
| `fallbackSource` | `string` | `""` | Image URL used when CSS provides no icon |
| `fallbackIconName` | `string` | `""` | Theme icon name used when CSS provides no `icon-name` |
| `color` | `color` | `white` | Tint colour applied via `MultiEffect` colorization |
| `colorize` | `bool` | `true` | Enable/disable `MultiEffect` colorization |
| `iconSize` | `int` | `min(width, height)` | Rendered icon size in pixels |

### CSS properties consumed

| Property | Description |
|----------|-------------|
| `icon` / `icon-image` | `url(...)` path or `qrc:/...` source |
| `icon-name` | XDG/theme icon name (requires an `image://themeicon/` image provider) |

---

## `CssItem`

**Inherits:** `Item` (invisible: `visible: false`, `width: 0`, `height: 0`)

A non-visual helper that carries the CssQmlItem signature and imperatively applies
resolved rules to its **parent** item. Drop it into any built-in Qt Quick item to make
it CSS-styled without replacing the item type.

```qml
Rectangle {
    width: 40; height: 40

    Css.CssItem {
        cssId: "circle"
    }
}
```

`CssItem` infers the primitive from its parent (looks for `font`/`text` → `"text"`,
`radius`/`color` → `"rect"`, else `"item"`), or you can set `cssPrimitive` explicitly.

### Properties

All CssQmlItem signature properties. In addition:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `cssPrimitive` | `string` | `""` | If empty, inferred from parent type |

### What it can apply

| Primitive | Properties applied to parent |
|-----------|------------------------------|
| `"text"` | `color`, `font-family`, `font-size` |
| `"rect"` | `background-color` → `color`, `border-radius`, `border-color`, `border-width` |

> **Limitation:** `CssItem` can only set what is imperatively assignable on the parent.
> For gradients, inset-bevel box-shadow, text-shadow, or layout, use `CssRect` /
> `CssText` instead.

---

## `CssKeyframes`

**Inherits:** `Item` (invisible)

Drives a single numeric property on a target item through a CSS `@keyframes` sequence.
Useful for animating opacity or other numeric properties that `CssRect`'s built-in
transform animation does not cover.

```qml
Css.CssKeyframes {
    frames: cssTheme.keyframes("blink")
    animatedProperty: "opacity"
    target: myItem
    duration: 760
    easingType: Easing.InOutSine
    iterations: -1  // infinite
    running: true
}
```

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `frames` | `var` | `[]` | Frame list from `cssTheme.keyframes(name)` |
| `animatedProperty` | `string` | `""` | Name of the property to animate on `target` |
| `target` | `var` | `null` | The item whose property is written |
| `duration` | `int` | `1000` | Cycle duration in milliseconds |
| `easingType` | `int` | `Easing.InOutSine` | QML `Easing.Type` for the whole cycle |
| `iterations` | `int` | `-1` | Number of repetitions; -1 = infinite |
| `running` | `bool` | `false` | Start/stop the animation |

Only numeric frame properties are interpolated. One tick walks the `@keyframes` from
`0%` to `100%`.

---

## `CssDropShadow`

**Inherits:** `MultiEffect`

A reusable drop-shadow effect built from a parsed shadow map. Intended as a
`layer.effect` on a `Text` or `Item`.

```qml
layer.enabled: shadow.color !== undefined
layer.effect: Css.CssDropShadow { shadow: parsedShadow }
```

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `shadow` | `var` | Shadow map: `{ x, y, blur, color }` as returned by `CssTheme.parseBoxShadow()` |

Used internally by `CssText` for `text-shadow`.

---

## `Contrast.js`

A `.pragma library` JS module with colour-contrast utilities. Import in QML:

```qml
import "qrc:/qmlcss/Contrast.js" as Contrast
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `luminance(c)` | `color → real` | Simple luminance (`0.2126r + 0.7152g + 0.0722b`) |
| `relativeLuminance(c)` | `color → real` | WCAG relative luminance (linearised channels) |
| `contrastRatio(a, b)` | `(color, color) → real` | WCAG contrast ratio |
| `blendOver(fg, bg)` | `(color, color) → color` | Alpha-composite `fg` over `bg` |
| `toHex(c)` | `color → string` | `"#rrggbb"` hex string |
| `bestBlackOrWhite(bg)` | `color → color` | Best-contrast black or white for `bg` |
| `naturalContrastColor(fg, bg, minRatio?)` | `→ color` | Adjust `fg` to clear `minRatio` (default 4.5) while preserving hue |
| `averageGradientColor(gradient, fallback)` | `→ color` | Average colour of gradient stops |
| `styleBackgroundColor(style, cssTheme, fallback)` | `→ color` | Effective background from a `style` map |
| `barBackground(cssTheme, fallback)` | `→ color` | Resolve `window#waybar background-color` |
| `effectiveBackground(ownBg, cssTheme, fallback)` | `→ color` | Element's actual background, blended over the bar |
| `needsDarkIcon(bg)` | `color → bool` | True when the background needs dark icons |
| `contrastColor(bg)` | `color → string` | High-contrast icon/text colour (`"#ffffff"` or charcoal) |
| `contrastFill(bg, alpha)` | `(color, real) → string` | `rgba(…)` contrast fill string |
