# Keyboard & accessibility

Interactive native widgets (`<button>`, `<input>`, `Menu`, …) get keyboard focus, the Tab chain
and screen-reader semantics for free, because each one wraps a real `QtQuick.Templates` control.
A plain element (`<div>`, `<text>`, `<span>`, …) wraps no control, so without this mapping
`<div role="button" tabIndex={0} aria-label="Close" onClick={…}>` would be invisible to Tab and
to a screen reader — a real gap this release closes for the attributes listed below.

The mechanism is deliberately NOT a new engine feature: `tabIndex`, `role` and `aria-*` on a
plain element are transpiled straight into the same `Accessible` attached-property lines and the
same `activeFocusOnTab` property a native widget's own `.qml` already uses (see
`src/widgets/button.cpp`'s `::tab-stop` ring and `app-wide tab-navigation` guard). Both are
standard `QtQuick` facilities available on *any* `Item`, so a plain element that uses them gets
exactly the native widget's behavior, and an element that uses none of them emits not one extra
line of QML — the cost is opt-in, not structural.

## Mapping table

| Attribute | Maps to | Notes |
|---|---|---|
| `tabIndex={0}` | `activeFocusOnTab: <app-wide tab-navigation guard>` | Joins the Tab chain in DOM/document order, with the same `::tab-stop` focus ring every native widget gets. |
| `tabIndex={-1}` | nothing | Not reachable via Tab; `ref.focus()` (any `QQuickItem`) still works regardless. |
| `tabIndex={N}`, `N > 0` | same as `tabIndex={0}` | Positive priority reordering is not supported; a warning is printed once per value. |
| `role="button"` / `"link"` / `"checkbox"` / `"switch"` / `"tab"` / `"tablist"` / `"menuitem"` / `"listitem"` / `"list"` / `"heading"` / `"dialog"` / `"alert"` / `"img"` / `"textbox"` | `Accessible.role: Accessible.<Role>` | ARIA → `QAccessible::Role`. An unmapped role prints one warning and emits nothing (the element's other effects — `onClick`, focus — still apply). |
| `aria-label={expr}` | `Accessible.name: <binding>` | Reactive: a signal read keeps the accessible name live. |
| `aria-description` / `aria-describedby` | `Accessible.description: "…"` | Static text only — no live DOM id lookup, no re-announcement on change. |
| `aria-checked` / `aria-pressed` / `aria-selected` | `Accessible.checked` / `.pressed` / `.selected` | Reactive. A literal `"true"`/`"false"` string is normalised to a real boolean. |
| `aria-hidden="true"` | `Accessible.ignored: true` | Removed from the accessible tree, mirroring `QQuickItemPrivate::isAccessible`. |
| `aria-disabled={expr}` | `enabled: !(<expr>)` | Drives the widgets' own standard disabled a11y state too (`enabled` is read by `QAccessibleQuickItem::state()`). |

### Keyboard activation (WAI-ARIA authoring practice)

While the element has keyboard focus, and only when it also has `onClick`:

| Role | Enter | Space |
|---|---|---|
| `button` | activates (key press) | activates on key **release** (press is swallowed, matching the browser: Space must not also scroll the page) |
| `link`, `menuitem`, `tab` | activates (key press) | activates (key press) |
| `checkbox`, `switch` | — | activates (key press) |

`onKeyDown` and the role-based activation share a single `Keys.onPressed` handler (QML allows
only one binding per signal); both run in the same block.

### Known edge case

Assigning `activeFocusOnTab` directly on a `.qml` **component's own document root** (as opposed to
any of its children, which is how every swept real app uses `tabIndex`/`role` today) can hit an
unrelated QML property-revision quirk ("`"X.activeFocusOnTab" is not available in solidqml.Widgets
1.0`") depending on what else is bound on that same root object. Not observed on any nested
element, and not hit by solid-mail, solid-player, office or solidterm's generated output (the
sweep for this release). If a future component's own JSX root ends up being the `tabIndex`/`role`
element itself, wrap it in a transparent parent, or test it live before shipping.

### Accessible name from content

A browser gives `<div role="button"><img/><text>Pastas</text></div>` the accessible name "Pastas"
even without `aria-label` — the DOM accname algorithm's "name from content" rule. This release adds
the same rule for roles that allow it: `button`, `link`, `menuitem`, `tab`, `checkbox`, `switch`,
`listitem`, `heading` — **not** `list`/`tablist`/`dialog`/`textbox`/`img` (a list's name should not
become the jumbled text of every row; `textbox`/`img` have their own naming contract — a `<label>`,
`alt` — covered below).

With no `aria-label` and a role that allows it, the name is the concatenation of the element's
descendant text (every `<text>`/`CssText` item's rendered text, in document order, whitespace-
collapsed) plus every descendant `<img>`'s `alt`. **This is computed at RUNTIME, not emitted as a
transpiled expression**: `src/widgets/a11y.cpp`'s `ContentNameAccessible` walks the item's
`childItems()` the first time (and every time) an accessibility client asks for the name
(`QAccessible::queryAccessibleInterface(item)->text(QAccessible::Name)`) — never from a layout or
paint pass. This is a deliberate choice over synthesizing a transpile-time expression from the
descendant text nodes: it is correct for every content shape alike — a literal `<text>`, a reactive
signal read, a `<For>`/`<Show>`/component subtree whose text isn't known until it runs — with no
special-casing for "static vs. dynamic" content, it is reactive by construction (there is nothing
to invalidate: the next query just re-walks the current tree), and it costs nothing while no AT
client is active (lazy, on-demand, same philosophy as the Text/Image primitives' lazy
accessibility above). The one thing it does NOT do (yet) is push a `NameChanged` event when content
changes while an AT is live and already holds the interface — a live screen reader that caches the
name needs to re-query it; see the module comment in `a11y.cpp` for the trade-off.

Implementation note: the role/explicit-name check has to happen lazily, inside the interface's own
`text()` (called once the interface already exists), not while `QAccessible::
queryAccessibleInterface` is still choosing which factory answers for the object — querying the
attached `Accessible.role` from inside that factory call observed it as not-yet-applied even though
the exact same lookup, called a moment later from `text()`/`role()`, sees it correctly (some Qt Quick
attached-property state apparently isn't settled yet mid-factory-selection). `ContentNameAccessible`
is therefore built unconditionally for any accessible plain item and decides at text-query time,
matching the pattern `PrimitiveAccessible` (the Text/Image interface, above) already uses.

An explicit `Accessible.name`/`aria-label` always wins — the content walk only runs when there is
none.

### aria-label on native controls

`aria-label`/`aria-description`/`aria-describedby` on a native control that already wraps a
`QtQuick.Templates` control (`<button>`, every `<input>` type the transpiler maps, `<textarea>`,
`<select>`) are no longer silently warn-only: they are forwarded onto the widget's `accessibleName`/
`accessibleDescription` property, which each C++ wrapper binds as `Accessible.name`/
`Accessible.description` on the INNER control — the item that actually takes keyboard focus and
answers `QAccessible` (`button.cpp`'s `T.Button`, `textinputs.cpp`'s `T.TextField`/`T.TextArea`,
`toggles.cpp`'s `T.CheckBox`/`T.Switch`, `sliders.cpp`'s `T.Slider`, `spindial.cpp`'s `T.SpinBox`,
`select.cpp`'s `T.ComboBox`, `datefield.cpp`'s readOnly `T.TextField`) — not onto the outer `CssFill`
wrapper, which never receives focus. `<input type="radio">` has no C++ wrapper (it emits a raw
`T.RadioButton` directly in `qml.ts`) — `Accessible.name`/`.description` are set on it directly.

A `<button>`'s accessible name defaults to its `text` (`QQuickAbstractButton` exposes it to
`QAccessible` on its own); `aria-label` overrides that default. The other controls have no default
name at all — without `aria-label` a `<input type="range">`/`<select>`/… stays unnamed to AT, same
as the browser's own contract (a `<label>` is normally what names them there).

`role`/`tabIndex`/most other `aria-*` on these tags stay warn-only (the control already carries its
own role and focus chain — see "Not mapped" below); only `aria-label`/`aria-description`/
`aria-describedby` are consumed.

**Gap, not mapped**: `<label for="…">`/a wrapping `<label>` naming its input — the transpiler does
not model `<label>` at all today (no element maps to it), so there is no live association to read a
label's text from. Until `<label>` is modeled, `aria-label` is the only way to name these controls.

### Not mapped

- `aria-expanded`: `QQuickAccessibleAttached` exposes no `expanded`/`expandable` property — there
  is no Qt Quick equivalent. Stays a documented, warned gap everywhere.
- `role`/`tabIndex`/`aria-*` on an element that already wraps a native `Templates` control (e.g.
  `<button role="link">`): the control already carries its own role and focus chain, so these
  stay in the warn-only tier rather than fighting the control's real `Accessible.role` — use the
  control's own semantics instead.

## Example

```tsx
<div role="button" tabIndex={0} aria-label="Close" onClick={() => setOpen(false)}>
  <text>×</text>
</div>
```

Generates (abridged):

```qml
W.Div {
    activeFocusOnTab: typeof solidTabstop !== "undefined" && solidTabstop ? solidTabstop.enabled : true
    Accessible.role: Accessible.Button
    Accessible.name: "Close"
    Keys.onPressed: (event) => { if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { … } if (event.key === Qt.Key_Space) { event.accepted = true; } }
    Keys.onReleased: (event) => { if (event.key === Qt.Key_Space) { … } }
    W.Text { text: "×" }
    MouseArea { … }
}
```

See `examples/a11y-demo.tsx` for a runnable demo (a custom `role="button"` div next to a real
`<button>`, both reachable by Tab, plus a `role="switch"` div toggled with Space).
