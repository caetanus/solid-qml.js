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
