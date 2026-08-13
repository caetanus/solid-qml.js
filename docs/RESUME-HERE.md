# Resume here — open threads, exact next steps

Written at the end of the 2026-08-13 session. Two tracks are open: the **native-behavior pass**
(current) and the **AOT C++ back-end** (earlier in the session). Everything below is committed on
branch `widgets`; nothing is pushed.

---

## Track A — "native is the default, custom is an override" (current)

The owner's principle: **platform behavior is the default; any custom behavior is an explicit
override.** Full findings: [`docs/native-behavior-audit.md`](native-behavior-audit.md).

**Key correction to keep in mind:** the first read ("the widget layer re-implements everything") was
WRONG. Most widgets already embed a real `QtQuick.Templates` control and override only visuals — the
principle was already the dominant idiom. The violations were *localized*, plus some blank omissions.

### Landed (all verified, meson test 4/4 + typecheck + AOT fixtures pixel-identical)

| Commit | What |
|---|---|
| `14798d3` | **Combo popup native positioning** — dropped the forced `popupType: Popup.Window` + hand-computed `y` flip from `select.cpp` |
| engine `1315375` + `d7e5948` | **CSS `outline`** in the engine (paints outside the box, no layout impact) |
| `c7c7527` | **Per-control focus ring** — `:focus { outline }` in `base.css`; deleted the 16 ms-polled overlay (`focusring.*`) |
| `87f39c4` | **Button → T.Button** — the one genuine control re-implementation, gone (~70 lines); gains a11y/keyboard/auto-repeat |
| `4590c95` | **a11y for the CSS primitives** — Heading / StaticText / Graphic + accessible names |
| `97ce9bc` | **`navigator.clipboard`** over `QClipboard` (there was no app-level clipboard) |

### Pending — needs a LIVE Wayland session (offscreen does not reproduce these)

**A1. Menu popup positioning.** *Diagnosed, not fixed.*
Unlike the combo, a menu SHOULD escape the window — `QQuickMenuPrivate::resolvedPopupType()` is an
override that can resolve to `Native`. The bug is the **anchor workaround** in
`transpiler/src/emit/native/menus.ts:253-262`: it moves a 1×1 host via `mapFromItem(null, x, y)` and
calls `popup(0, 0)`, because "on Wayland the popup anchors to the parentItem rect and IGNORES the
`popup(x,y)` offset". That `mapFromItem` is where the CSS layout's item nesting skews the position.

The native way is to let Qt build the `xdg_popup` anchor from the trigger itself:
`menu.popup(triggerItem)` (or `menu.popup(triggerItem, point)`).

**Run this on the desktop and report whether the menu opens directly below the button:**
```bash
cat > /tmp/m.qml <<'EOF'
import QtQuick
import QtQuick.Controls
ApplicationWindow { width:500; height:400; visible:true
  Button { id:b; text:"Open"; x:200; y:170; onClicked: m.popup(b) }
  Menu { id:m; MenuItem{text:"Cut"} MenuItem{text:"Copy"} MenuItem{text:"Paste"} }
}
EOF
qml /tmp/m.qml    # click "Open"
```
- **Positions correctly** → refactor the three emit call-sites (`trigger`, `ref`, `ContextMenu`) to
  pass the trigger item instead of moving a host, and drop the 1×1 host entirely.
- **Also mispositions** → the problem is deeper (coordinate mapping through the CSS layout) and
  needs its own investigation.

**A2. Tooltips (`title` → platform tooltip).** *Mechanism proven, wiring not shipped.*
The `ToolTip` attached type works on our widgets (verified: `b.ToolTip.visible === true`, text set,
the style's box appears). What blocked shipping: offscreen renders the tooltip as a narrow strip with
no text — **and it does the same on a plain `Item` with no CSS engine involved**, i.e. a harness
artifact, not our bug. Remaining work: map the HTML `title` attribute to `ToolTip.text` +
`ToolTip.visible: <hover>` in both back-ends, then verify on a real desktop. Decide there whether the
tooltip visuals come from `QtQuick.Controls` (platform-styled, the native default) or a CSS-styled
`contentItem`.

### Pending — doable headless (no live session needed)

- **A3. Alt-mnemonics are cosmetic.** `MenuItem::__mnemonicMarkup` (`menuwidgets.cpp:148-169`)
  underlines `&X` but binds no accelerator; there is no F10/Alt menu-bar access either.
- **A4. Drag-and-drop** — no `Drag`/`DropArea` primitive exists (a `jsx.d.ts` type alludes to it).
- **A5. Scroll → native `ScrollView`** (audit P4). `cssscroll.h` composes a bare `Flickable` then
  hand-rolls the scrollbar (two `MouseArea`s), the wheel (discrete steps, **no kinetic fling**) and
  focus-follows-scroll. **Missing: keyboard scrolling (PageUp/PageDown/Home/End) and horizontal
  scroll.** Wrapping the content holder in a `ScrollView` inherits all of it; its `ScrollBar` stays
  CSS-themable through `contentItem`/`background`.
- **A6. Data grids** (audit P5, weakest violation): `TableView`/`TreeView`/`Details`/`MonthGrid`
  compose plain `Item` trees with `MouseArea`s instead of a native item view. Revisit last; document
  the trade-off if kept (Qt's delegate model is genuinely hard to CSS-style).

### Deliberately NOT changed (verified legitimate)

- **DateField's hand-computed popup `y`** — `allowVerticalFlip` is PRIVATE (no Q_PROPERTY) and a bare
  `T.Popup` has no native flip (only `ComboBox` sets it). The manual flip is the only way.
- **Dialog chrome** (Esc / Enter-fires-default / focus-on-open on a raw `Window`) — forced by the
  owner's decision that `<dialog>` is a real top-level Window.
- **The `SolidTabstop` shim** — it drives `activeFocusOnTab`, i.e. it *cooperates with* Qt's native
  focus chain. Only the polled ring overlay was removed.

### The guardrail (keep new code on the native side)

> A widget MUST inherit its behavior from the nearest `QtQuick.Templates` control — compose it with
> `background: null` / `contentItem: null`, or subclass the `QQuick*` class — and override **only**
> its visual slots with CSS-styled items. Hand-writing input events (`mousePressEvent`,
> `keyPressEvent`, `hoverEnterEvent`, manual focus/state) is allowed **only** where no native control
> has the behavior (a bare interactive `<div>`, the CSS box model, the `:hover` cascade). If you find
> yourself re-deriving click / keyboard activation / press-hover-focus state / a11y — stop and
> compose the `T.*` control instead.

---

## Track B — AOT C++ back-end (M-AOT)

A second transpiler back-end (`transpiler/src/emit/cpp/`) emits QtQuick C++ from the same AST as the
QML back-end, dropping V4 from the hot path. **The whole standard Solid surface is covered.**

- **16 views twin-render pixel-identical** (counter, props, derived, show, index-list, for-list,
  input, resource, textarea, image, switch, select, children, context, checkbox, + hello/multi at
  99.9% — sub-pixel antialiasing only).
- **11 of 15 real gallery examples generate valid C++.**
- Covered: state/bindings/handlers, `<Show>`/`<Switch>`/`<For>`/`<Index>`/`<Suspense>`, forms,
  components (incl. stateless + **multi-file**), mergeProps/splitProps, derived, `props.children`,
  **createResource via a QJSEngine sidecar** (the "dynamic frontier"), **createContext with
  cross-boundary reactivity**.
- Preliminary M-AOT-2 numbers (`npm run bench`): AOT 590K / 172 ms / 60.9 MiB vs loader 707K /
  190 ms / 62.3 MiB — 117K smaller, ~9% faster start, 1.4 MiB less RSS.

**Remaining, each a focused session:** the store API (`createStore`/`produce` — blocks todomvc),
the native-widget tag set (TabBar/TreeView/… — blocks the `native` example), `<Dynamic>`, radio
(needs `T.RadioButton` + `ButtonGroup`), and the real M-AOT-2 deliverable: full-gallery + Electron
comparison numbers.

Useful commands: `npm run gen:cpp`, `npm run twin-render <entry.tsx> [maxDiffPct]`, `npm run bench`.

---

## Test-harness traps (each cost a debugging round — do not relearn them)

1. **`--click` on the loader takes THREE parts: `x,y,delayMs`.** With only `x,y` it does
   `parts.size() < 3 → continue` and the click is **silently skipped**. (The AOT binary's own
   `--click` takes two parts — they differ.)
2. **Children of a Css box are repositioned by the layout engine** — explicit `x`/`y` in a test QML
   is overwritten, so clicking the "declared" coordinate misses. Click the **rendered** position.
3. **Offscreen has no MultipleWindows**, so every popup resolves to `Popup.Item` — the `Window` vs
   `Item` difference (and thus popup mispositioning) only shows on a real desktop.
4. **Tooltips render as an empty strip offscreen**, even on a plain `Item`. Not our bug.
5. The loader is not a generic QML runner (it chokes on `ApplicationWindow`/Controls scenes); use
   `qml` for scratch scenes — but note `qml` failed to load `QtQuick.Controls` offscreen here.
6. Attached properties (a11y, tooltip) must be created via
   `qmlAttachedPropertiesObject<T>(obj, /*create=*/true)` — that is where Qt looks them up — and
   wired in `componentComplete()`, not the constructor.
