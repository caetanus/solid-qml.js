# CSS support reference

This page lists the CSS properties and features that `qml-css-engine` actually handles,
based on the parser and component source code. Properties not listed here are passed
through into the `style` map as raw strings but have no built-in renderer.

---

## Cascade and selectors

| Feature | Notes |
|---------|-------|
| `* {}` universal selector | Matches all elements |
| `#id {}` id selector | Specificity 100 |
| `.class {}` class selector | Specificity 10 |
| `:pseudo-class` | Treated as a class (specificity 10); supply via `cssClass`/`cssState` |
| `::pseudo-element` | `::before`, `::after`; resolved via `resolveWith(…, pseudoElement)` |
| `#ancestor descendant {}` | Descendant combinator; ancestor matched by `#id` |
| `element {}` | Element-type selector; only matched via `resolveWith`, not globally |
| Specificity | id=100, class=10, pseudo-element=1, element=1; stable sort on ties |
| `!important` | Applied in a second pass, wins over non-important regardless of specificity |
| Source order | Later rule wins on equal specificity |

---

## At-rules

| Rule | Support |
|------|---------|
| `@import "file.css";` | Recursive; relative to the importing file; `file://` and absolute paths; cycle-guarded |
| `@import url("file.css");` | Same |
| `@define-color name value;` | GTK-style; cross-references resolved (up to 5 passes); stripped before parsing |
| `@media (min-width: Npx)` | Live; re-evaluated on `CssTheme::setViewport()` |
| `@media (max-width: Npx)` | " |
| `@media (min-height: Npx)` | " |
| `@media (max-height: Npx)` | " |
| `@media A, B {}` | OR: matches when any comma-group matches |
| `@media (A) and (B) {}` | AND within a comma-group |
| Media types (`screen`, `all`) | Recognised and ignored (no constraint) |
| `@keyframes name { … }` | Stored by name; `from`/`to`/`%` offsets; comma-separated offsets |
| `@supports`, `@container` | Recognised and dropped |

### GTK-style colour variables (`@define-color`)

The engine adopts GTK's CSS colour-variable extension as its main theming primitive — there are no
standard `var(--x)` custom properties; use `@define-color` instead. Declare named colours, reference
them anywhere with `@name`, and chain definitions together: the parser collects the declarations,
resolves cross-references between them (up to 5 passes), substitutes every `@name` throughout the
sheet, then strips the declarations before the cascade runs.

```css
@define-color bg        #1e1e2e;
@define-color accent    #89b4fa;
@define-color accent_hi @accent;     /* references resolve to another variable */

#card        { background-color: @bg; border: 1px solid @accent; }
#card:hover  { border-color: @accent_hi; }
```

`Contrast.js` (bundled under `qrc:/qmlcss`) complements this with WCAG luminance/contrast helpers,
e.g. to pick a readable text colour over a variable background.

---

## Colour values

| Format | Example |
|--------|---------|
| Named keyword | `red`, `blue`, `transparent` |
| `#rgb` | `#f0a` |
| `#rrggbb` | `#ff0080` |
| `#rrggbbaa` | `#ff008080` (CSS 8-digit) |
| `#aarrggbb` | `#80ff0080` (Qt ARGB notation) |
| `rgb(r, g, b)` | `rgb(255, 0, 128)` |
| `rgba(r, g, b, a)` | `rgba(0, 0, 0, 0.5)` |
| `transparent` | Normalised to `#00000000` before storage |
| `none` / `inherit` on colour properties | Normalised to `#00000000` |

---

## Length values and units

Used in `width`, `height`, `padding`, `margin`, `gap`, grid tracks, `top/right/bottom/left`:

| Unit | Resolves to |
|------|-------------|
| `px` or bare number | Pixels |
| `%` | Percentage of the available axis |
| `vw` | Percentage of viewport width |
| `vh` | Percentage of viewport height |
| `vmin` | Percentage of `min(vw, vh)` |
| `vmax` | Percentage of `max(vw, vh)` |
| `calc(expr)` | Full recursive-descent evaluator: +, −, ×, ÷, unary sign |
| `min(a, b, …)` | Minimum of arguments |
| `max(a, b, …)` | Maximum of arguments |
| `clamp(min, val, max)` | Clamped value |

Font-size units (resolved to Qt points by `CssTheme::parseFontSize`):

| Unit | Resolves to |
|------|-------------|
| `px` | `px × 72 / 96` points |
| `pt` or bare number | Points directly |
| `em` / `rem` | Multiplier of the fallback point size |

---

## Paint properties

### Background

| Property | Component | Notes |
|----------|-----------|-------|
| `background-color: <color>` | `CssRect`, `CssFill` | Solid fill |
| `background: <color>` | `CssRect` | Solid fill (when not a gradient or url) |
| `background: linear-gradient(…)` | `CssRect` | Single linear gradient (fast path via built-in `Shape` fill) |
| `background: radial-gradient(…)` | `CssRect` | Radial gradient via `CssFillLayer` |
| `background: layer1, layer2, …` | `CssRect` | Multi-layer; first listed paints on top |
| `background-image: url(…)` | `CssFill` | Image fill; supports `qrc:/`, `file://`, absolute/relative paths, http/https |
| `background-size: cover\|contain\|stretch` | `CssFill` | Image fill mode |
| `background-image-opacity: <0..1>` | `CssFill` | Custom; opacity of the image layer |

**Gradient syntax supported:**

```css
/* Linear */
linear-gradient(to right, #a, #b)
linear-gradient(135deg, #a 0%, #b 50%, #c 100%)

/* Radial */
radial-gradient(circle at center, #a, #b)
radial-gradient(ellipse at 30% 70%, #a, #b)
```

### Border

| Property | Notes |
|----------|-------|
| `border: <width> <style> <color>` | Shorthand; any subset/order; parsed by `parseBorder()` |
| `border-color: <color>` | Overrides shorthand colour |
| `border-width: <length>` | Overrides shorthand width |
| `border-radius: <length>` | 1–4 values (CSS shorthand); percentage; true `PathArc` quarter-ellipse arcs |

### Shadow

| Property | Effect |
|----------|--------|
| `box-shadow: <x> <y> <blur> [<spread>] <color>` | Outset: drop shadow via `QtQuick.Effects.MultiEffect` (spread ignored) |
| `box-shadow: inset <x> <y> <blur> <color>` | Single inset: directional edge band on the offset side |
| `box-shadow: inset … , inset …` | Two inset shadows: 4-sided recessed bevel (dark + light pair) |
| Multiple outset+inset | Parsed correctly; first outset used for drop shadow, first two insets for bevel |
| `text-shadow: <x> <y> <blur> <color>` | `CssText` only; rendered via `CssDropShadow` (`MultiEffect`) |

---

## Visibility and display

| Value | Effect |
|-------|--------|
| `display: none` | `visible: false`; removed from layout |
| `display: block` | Default; stacks children vertically |
| `display: flex` / `inline-flex` | Flexbox (see below) |
| `display: grid` | Grid (see below) |

---

## Box model and layout

### Flexbox (`display: flex` / `inline-flex`)

| Property | Values supported |
|----------|-----------------|
| `flex-direction` | `row` (default), `column`, `row-reverse`, `column-reverse` |
| `flex-grow` | Number |
| `flex` | First token taken as `flex-grow` |
| `justify-content` | `flex-start` (default), `center`, `flex-end`, `end`, `space-between`, `space-around`, `space-evenly` |
| `align-items` | `stretch` (default), `center`, `flex-end`, `end` |
| `align-self` | Same values as `align-items`; per-child override |
| `gap` | Applied between items in the main axis |
| `row-gap`, `column-gap` | Axis-specific gap |

### Grid (`display: grid`)

| Property | Values supported |
|----------|-----------------|
| `grid-template-columns` | `px`, `fr`, `%`, `auto`, `min-content`, `max-content`, `minmax(min, max)`, `repeat(N, track)` |
| `grid-template-rows` | Same; auto-sized when omitted |
| `column-gap`, `gap` | Column spacing |
| `row-gap`, `gap` | Row spacing |
| `justify-items` | `stretch` (default), `center`, `end`, `flex-end` |
| `align-items` | `stretch` (default), `center`, `end`, `flex-end` |

Items are placed in row-major order (left-to-right, then next row). Individual
`grid-column` / `grid-row` placement is not implemented.

### Box model (all display modes)

| Property | Notes |
|----------|-------|
| `padding` | 1–4 value shorthand |
| `padding-top`, `padding-right`, `padding-bottom`, `padding-left` | Longhand; overrides shorthand |
| `margin` | 1–4 value shorthand (per child) |
| `margin-top`, `margin-right`, `margin-bottom`, `margin-left` | Longhand |
| `width`, `height` | All length units; `auto` uses `implicitWidth`/`implicitHeight` |
| `aspect-ratio` | `w/h` or bare number |

### Absolute positioning

| Property | Notes |
|----------|-------|
| `position: absolute` | Removes from flow; placed relative to the padding box |
| `top`, `right`, `bottom`, `left` | Offset from the corresponding edge; all length units |
| `inset` | 1–4 value shorthand for all four sides |

---

## Typography

Consumed by `CssText` directly and propagated via `CssRect`'s inherited properties:

| Property | Effect |
|----------|--------|
| `color` | Text colour |
| `font-family` | Font family name |
| `font-size` | Point size; px/pt/em/rem/bare (see length units) |
| `font-weight` | `normal`/`bold`/`bolder`/`lighter`/100–900 |
| `letter-spacing` | `font.letterSpacing` in pixels |
| `text-transform` | `uppercase` → `AllUppercase`, `lowercase` → `AllLowercase`, `capitalize` → `Capitalize` |
| `text-align` | `left` (default), `center`, `right` |
| `text-shadow` | See shadow section |

---

## Transform and animation

| Property | Supported functions / values |
|----------|------------------------------|
| `transform` | `rotate(<angle>)`, `scale(<x>[, <y>])`, `scaleX(<v>)`, `scaleY(<v>)`, `translate(<x>[, <y>])`, `translateX(<v>)`, `translateY(<v>)` |
| Angle units in `rotate` | `deg` (default), `rad`, `turn` |
| Pivot point | Item centre (Qt `transformOrigin: Item.Center`) |
| `transition` | `<property> <duration> [<timing-function>] [<delay>]` shorthand |
| Transition timing functions | `linear`, `ease`, `ease-in`, `ease-out`, `ease-in-out`, `out-cubic`, and other Qt easing names |
| Duration units | `ms`, `s`, bare number (ms) |
| `animation` | `<name> <duration> [<timing-function>] [<iteration-count>] [<direction>] [<delay>]` |
| `animation: … infinite` | Loops indefinitely |
| `@keyframes` | `from`/`to`/`<pct>%` offsets; comma-separated offsets; `transform` in frames drives `CssRect` rotation/scale/translate |

---

## Icon (custom properties)

Consumed by `CssIcon`:

| Property | Value |
|----------|-------|
| `icon` / `icon-image` | `url(path)` — file path or `qrc:/` resource |
| `icon-name` | XDG/theme icon name (requires a `image://themeicon/` provider) |
