# Native-behavior audit — "native is the default, extra is an override"

**Owner principle (the yardstick for this audit):** native behavior must be the *default*; any
custom behavior is an explicit *override*. The suspicion was that the framework replaces native Qt
behavior with hand-written emulation "by default" and fixes it case by case.

**Verdict (honest, evidence-based):** the architecture *mostly already follows the principle* — but
it breaks it in a few concentrated, high-traffic places, and it has several blanket *omissions* where
a native mechanism exists and was simply never wired. The problem is real, but it is **localized, not
pervasive**. This document separates what is already correct, the genuine violations (prioritized),
and the guardrail to keep new code on the native-default side.

---

## 1. What the architecture already does RIGHT (do not rewrite this)

The line between *behavior* (native) and *visuals* (CSS override) is drawn correctly almost
everywhere. This is exactly the principle, and it is the pattern to reinforce.

- **The engine base owns only paint + layout + cascade.** `CssRect`/`CssFill` contain no
  `MouseArea`/`TapHandler`/`Keys`/`clicked`/focus logic. Their *only* interaction primitive is a lazy
  `HoverHandler` created *only when a `:hover` CSS rule matches* (`cssrect.cpp:1366-1403`) — it feeds
  the cascade, not behavior. Paint (`Shape`/`MultiEffect`), the box model (`CssLayoutEngine`), and the
  cascade have no native Qt equivalent, so owning them is *thesis-mandated*, not a violation.
- **Most widgets = a real `QtQuick.Templates` control with its visuals nulled.** The dominant idiom
  (`snippetwidget.cpp` `composeInternal`) is:
  ```qml
  T.CheckBox { background: null; contentItem: null      // native VISUALS removed
    onToggled: { root.checked = checked; root.toggled() } // native BEHAVIOR kept
    indicator: Css.CssFill { cssPrimitive: "span"; ... }  // CSS-styled visual only
  }
  ```
  Confirmed native controls behind the CSS skin: `T.CheckBox`/`T.Switch` (toggles), `T.Slider`/
  `T.RangeSlider`, `T.ComboBox` (select), `T.SpinBox`, `T.Dial`, `T.TextField`/`T.TextArea`,
  `T.ProgressBar`, `T.DelayButton`, `T.Tumbler`, `T.TabBar`/`T.TabButton`, `T.ToolBar`, `T.SplitView`,
  `T.SwipeView`, `QQuickMenu`/`QQuickMenuItem`. These get keyboard, focus, IME/selection/clipboard,
  a11y, and activation **for free**; only paint is overridden. This IS the principle.
- **Widget events map to real native signals.** `<button onClick>` → `onClicked`, `<input onInput>` →
  `onTextEdited`/`Keys.onPressed`, `<select onChange>` → `activated(index)`, checkbox → `toggled`.
  Text editing (IME, selection, clipboard, undo/redo, cursor) is fully native via the Templates text
  controls — the exemplar of the thesis.

**Implication:** the fix is NOT to re-architect the widget layer. It is to make the few violators
follow the pattern the library already uses, and to wire the native mechanisms that were left unwired.

---

## 2. Genuine violations & omissions (prioritized fix queue)

### P1 — The `Button` family: the one systemic behavior re-implementation
`class Button : public QmlCss::CssFill` (`button.h:16`) does **not** compose a `T.Button`. It composes
only a label and hand-rolls the entire `AbstractButton` contract in C++ — the **only** widget in the
repo overriding raw input events (`grep mousePressEvent|keyPressEvent|hoverEnterEvent` → `button.cpp`
only):
- `mousePressEvent`/`mouseReleaseEvent` → `m_pressed`, `forceActiveFocus`, manual hit-test
  `if (boundingRect().contains(pos)) emit clicked()` (`button.cpp:127-144`)
- `keyPressEvent` → Space/Return/Enter → `emit clicked()` (`button.cpp:146-158`)
- `hoverEnterEvent`/`hoverLeaveEvent` → `m_hovered` (`button.cpp:113-125`)
- `syncState()` builds `["hover","active","focus","default","disabled"]` by hand (`button.cpp:97-111`)
- tab-stop via `setActiveFocusOnTab` (`button.cpp:69-72`)

**What it loses vs `T.AbstractButton`:** accessibility (role/name/state — screen readers see nothing),
`checkable`/`autoExclusive`, `autoRepeat`, `Action`/`Shortcut` binding, press-cancel-on-drag-out,
keyboard press *visual* state (Space/Enter emit `clicked` but never set `m_pressed`), robust
touch/pointer handling, `autoDefault`. The header comments show this was a *deliberate* port away from
`T.Button` (`button.h:8-11,73,82`) — i.e. a self-inflicted regression.

**Fix (mirror the library's own pattern):** `Button` should be `T.Button { background: null;
contentItem: <CssText label>; onClicked: root.clicked() }` (or subclass `QQuickAbstractButton` like
`TabButton` does). Its ~70 lines of C++ event handling collapse to a snippet + a signal forward. Its
siblings `DelayButton` (composes `T.DelayButton`) and `TabButton` (subclasses `QQuickTabButton`) prove
this works — Button is the inconsistency, not the rule.

### P2 — Blanket omissions: native mechanisms that exist but were never wired
These are *omissions*, not overrides — cheap to add, no re-architecture:
- **Accessibility.** `grep -rniE "QAccessible|Accessible"` over `src/` + engine = **zero hits**. The
  Templates-backed controls get a11y free, but `Button`, all `div`/containers, headings, and text
  nodes expose no role/name/state. Wire `Accessible.role/name` (or `QAccessible`) on `Button` and the
  CSS primitives; derive the name from the button label / `&`-mnemonic.
- **Alt-mnemonics are cosmetic.** `MenuItem::__mnemonicMarkup` underlines `&X` but binds **no Alt+X
  accelerator** (`menuwidgets.cpp:148-169`). No F10/Alt to open a menu bar. Wire real accelerators.
- **Tooltips.** No `ToolTip` attached usage anywhere (only the system-tray icon tooltip). No hover
  tooltips on controls.
- **Clipboard / drag-and-drop.** No app-level `QClipboard` path (only inside native text controls) and
  no `Drag`/`DropArea` primitive (a `jsx.d.ts` type exists but nothing implements it).

### P3 — Focus ring: gratuitous 60 fps polling
`Tabstop::track()` is a **16 ms `QTimer` poll** (`focusring.cpp:16-17,58-68`) that reads
`window.activeFocusItem()`, `mapToItem`s its rect, and sizes a global overlay `CssRect` styled via the
`::tab-stop` pseudo-element. The tab *chain* is native (`activeFocusOnTab: solidTabstop.enabled`), but
the ring is a window-level polled rectangle that lags a frame behind scroll/resize, can't follow focus
clipped inside a scroll viewport, and draws a uniform dotted box regardless of the control's shape.
**Fix:** drive it from `activeFocusItemChanged` + the focused item's geometry-changed signals
(event-driven, no timer); longer term, once `Button` is a Templates control, prefer per-control
style focus rings (what every other Templates control already does).

### P4 — Scroll: native `Flickable` used as a substrate, then everything hand-rolled on top
`cssscroll.h` composes a bare `Flickable` and re-implements: wheel as discrete steps with **no kinetic
fling** (`cssscroll.h:34-45`), a **Controls-free scrollbar** (raw `Rectangle` + two `MouseArea`s for
thumb drag / track paging, `cssscroll.h:62-106`) instead of `ScrollBar`, and focus-follows-scroll by
hand (`cssscroll.h:48-60`). **Missing:** kinetic/trackpad momentum, **keyboard scrolling
(PageUp/PageDown/Home/End)**, and **horizontal scroll** (`VerticalFlick` only). **Fix:** wrap the
content holder in a native `ScrollView` (its `ScrollBar` stays CSS-themable via `contentItem`/
`background`), inheriting wheel, keyboard, kinetic flick, and both axes; keep the CSS box model.
(Having overflow scroll at all is legitimate — only the hand-built scrollbar/wheel/ensureVisible is the
soft spot.)

### P5 — Data grids: `TableView`/`TreeView`/`Details`/`MonthGrid` (borderline)
These compose plain `Item` trees with `MouseArea`/`TapHandler` for row/cell interaction rather than a
native item-view (`ListView` *is* native, for contrast). Weaker violation — Qt's `TableView`/`TreeView`
delegate model is genuinely hard to CSS-style — but row-click/selection is re-emulated. Revisit only
after P1–P4; document the trade-off if kept.

### P6 — Duplicate hover model (minor)
A plain `<div onClick>` derives `:hover` from a synthetic `MouseArea.containsMouse` (`qml.ts:125`),
a second hover path parallel to the engine's own `HoverHandler`→`cssEngineHover` (`cssrect.cpp:1399`).
Route the plain-element hover through the engine's existing mechanism.

### Noted, but ~forced by design: Dialog chrome
`Dialog` re-implements Esc / Enter-fires-default / focus-on-open on a raw `Window` (`dialog.cpp:33-96`).
This is *mostly forced* — a `<dialog>` is a real top-level `Window` (owner decision), so it can't ride
a `QQuickDialog` popup. Keep, but factor the default-button/Esc logic into a reusable helper.

---

## 3. The guardrail (keep new code native-default)

The library already embodies the rule; make it explicit so it isn't re-broken:

> **A widget MUST inherit its behavior from the nearest `QtQuick.Templates` control** (compose it with
> `background: null` / `contentItem: null`, or subclass the `QQuick*` C++ class), and override **only
> its visual slots** with CSS-styled items. Hand-writing input events (`mousePressEvent`,
> `keyPressEvent`, `hoverEnterEvent`, manual focus/state) is allowed **only** when no native control
> has the behavior (bare interactive `<div>`, the CSS box model, `:hover` cascade). If you find
> yourself re-deriving click / keyboard activation / press-hover-focus state / a11y, you are
> overriding behavior that should be inherited — stop and compose the `T.*` control instead.

**One-line summary:** the principle is right and *already applied* across the widget library; the work
is to (1) refit `Button` onto `T.Button` — deleting the only genuine behavior re-implementation — and
(2) wire the native mechanisms (a11y, Alt-mnemonics, tooltips, `ScrollView`, clipboard/DnD) that were
left unwired. Not a rewrite; a consolidation onto the pattern the codebase already proves it can use.
