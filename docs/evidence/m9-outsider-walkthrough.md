# M9 outsider walkthrough (2026-10-11)

Release-plan M9 asks for the docs to be "tested by someone outside the project". As a stand-in,
a fresh agent with no project context was given only the docs (README, getting-started,
architecture, support matrix, known issues, keyboard & accessibility) and asked to (1) follow
"Your first app outside the checkout" literally, (2) build a small todo app choosing features from
the support matrix, and (3) try constructs the matrix says are rejected. It used a prebuilt,
uninstalled `solid-qml-loader` instead of an installed one (no rebuild on a RAM-limited machine);
that substitution is not a doc finding. Paths below are shortened to `CHECKOUT`.

## Findings (as reported) and what was done

| # | Finding | Severity (reported) | Fix |
|---|---|---|---|
| 1 | getting-started's run command omits `--css`; `--help` says it "defaults to the bundled sheet", which is ambiguous (it did load the app's `App.generated.css`). | minor | getting-started now says the loader loads the sidecar `App.generated.css` next to the entry, on top of the built-in base sheet (`src/loader.cpp`). |
| 2 | No documented way to simulate interaction; `--click` was only found via `--help`. No way to inject keystrokes headlessly. | blocker for headless text-input tests | getting-started documents `--click x,y,delayMs` and `--help`; known-issues lists "headless interaction testing is mouse-only". |
| 3 | The `onInput` handler signature (`e.target.value`) appears in none of the docs; had to read `examples/widgets.tsx`. | confusing | support matrix: "Input events carry the element as `e.currentTarget` (and `e.target`)" with examples (verified in the emitted QML). |
| 4 | Store reads are not calls (`items.length`, not `items()`), unlike signals; nothing says so, and the wrong form is not caught. | blocker | support matrix: "Signals are read by calling them, stores are not." |
| 5 | `<input type="checkbox">` renders with zero size; nothing says form controls need CSS. Clicking where it should be did not toggle. | blocker | Verified: with `width`/`height` and `.indicator` rules it renders and a `--click` toggles it (checked state and `classList` strike-through update). Snippet added to the support matrix; known-issues lists "form controls have no default look". |
| 6 | Transpile errors are raw Node stack traces from `scripts/gen.mjs`; the documented message strings are accurate substrings. | minor | `scripts/gen.mjs` now prints `[gen] <file>: <message>` and exits 1 (`SQ_GEN_STACK=1` keeps the stack). |

## What worked (as reported)

- The literal "first app" recipe worked on the first try (paths, the `tsx` loader workaround,
  `gen.mjs`, `--grab`); the PNG showed the dark window, "count: 0" and the green button.
- The element and Solid-API tables were accurate and sufficient for the todo app (`div`/`text`/
  `button`/`input`, `<For key>`, `<Show fallback>`, `createMemo`, `createStore` value and
  `(pred, "key", updater)` setters, `classList`); add and remove were verified with `--click`.
- All three deliberately rejected constructs produced exactly the documented errors: block-bodied
  `onClick`, `a?.b`, `while`.
- Flex layout, `:hover` and a `classList`-driven class behaved as documented.
