# solid-qml — release plan (what is missing to launch)

Status: 2026-10-10. Sources: a repository inventory (README, ROADMAP, CRITICS, RESUME-HERE, native-behavior audit,
SOLID-PARITY, build files, git state) and a Claude×Codex adversarial round (refute-first, converged). Every item
below cites what is in the repo today; nothing here is shipped until the owner decides the open points.

## Recommended scope: a 0.x developer preview, not 1.0

The internal performance gate ("beat Chromium on all six operations, equivalent behaviour and accessibility,
< 15.9 KB per materialized row") is met for scrolling lists but NOT for eager lists or per-row memory, and the AOT
(C++) back-end is not a production path yet. A preview is honest only if the owner explicitly accepts a narrower
gate. Recommended preview scope:

- the QML runtime path (TSX → QML + C++ CSS engine + widgets + loader + embed library);
- ONE supported environment: Linux x86_64, pinned Qt 6.11.2, a named distro (owner's Manjaro/Arch first), Wayland
  and X11 as tested;
- source release (git tag + tarball) with a starter template; no npm/AppImage/mobile/Windows/macOS promises;
- AOT/C++ emitter, WebView, Charts/3D, flattened rows: experimental or excluded, labelled as such.

## MUST before the preview (blocking)

| # | Item | Today (evidence) | Done when |
|---|---|---|---|
| M1 | **License** | No LICENSE file; `meson.build` `LicenseRef-NONE`, `package.json` `UNLICENSED`, README "TBD"; the GitHub repo is PUBLIC without a license | LICENSE + NOTICE in the app, engine and D-binding repos; metadata aligned; Qt-derived code (csstextlayout is a literal QQuickText port) and Qt LGPL obligations inventoried |
| M2 | **No silent miscompiles** | Branch `silent-drops` (7 commits, 582/582 transpiler tests): multi-root Show/Match, `<text onClick>`, dynamic placeholder/disabled/readOnly/class, `<Switch fallback>`, fail-loud attribute contract | Merged after solid-mail and solid-player run their UIs through it; AOT emitter covered by the same contract or marked unsupported |
| M3 | **C23 decided** (row identity) | New objects with the same `id` reuse the row; Solid web recreates it | Owner picks a contract; executable mount/cleanup/state tests; benchmarks re-run |
| M4 | **Core behaviour validated on a real display** | Offscreen cannot validate popups/GPU; menu positioning repro waits for a live run (RESUME-HERE); Menu has functional sizing defects (clipped labels, CSS sizing ignored) | Supported demo passes: forms, list mutations, keyboard/focus, screen reader activation, resize, hide/show, menus |
| M5 | **Keyboard/a11y for essential controls** | `<div role="button" tabIndex aria-label>` has no native effect (now a warning) | tabIndex/role/aria-label mapping (queued) or documented: essential controls use native widgets |
| M6 | **Install story from a clean machine** | Meson installs lib + header + pkg-config + CMake + loader; submodule URL is SSH (`.gitmodules`); getting-started says "Qt 6" and `subprojects/` (stale) | HTTPS submodule URL; dependency list (Qt modules incl. private Templates headers, Node version); a new app builds and runs outside the framework checkout on a clean machine |
| M7 | **CI** | None in the app repo (engine submodule has CI) | Clean checkout + submodules, locked deps, typecheck, transpiler + native tests, an installed-artifact smoke test, on the pinned environment |
| M8 | **Versioning and release policy** | `meson.build`/`package.json` 0.2.0, `package-lock.json` 0.1.0, `docs/conf.py` 0.1.0; no SONAME policy; tags v0.1.0-alpha.1, v0.2.0-alpha.1 exist | One version everywhere; SONAME/ABI statement (private Qt headers ⇒ no cross-Qt-version promise); release notes; immutable tag |
| M9 | **Accurate docs** | README/architecture describe widgets as .qml (they are C++), engine under `subprojects/` (it is `vendor/`); `examples/todomvc-gaps.md` contradicts SOLID-PARITY; embed README describes a fixed defect | Getting-started, architecture, supported TSX/CSS/widget/shim matrix, known issues; tested by someone outside the project |
| M10 | **Honest performance claims** | Bench beats Chromium on 6/6 sync ops @1000 for scrolling lists via automatic virtualization | Published methodology + raw results; claim scoped ("in our 1,000-row scrolling-list benchmark…"); limits stated (virtualization does the work; eager lists and per-row memory still behind; sync timings, not presented frames) |

## SHOULD (preview quality)

- Starter template repo; one realistic supported demo (SolidTerm or a lists+forms app) with tests.
- Matched presented-frame timing on web (double rAF) next to the native frameSwapped numbers.
- Known-issues table: Menu theming limits, WebView black after hide/show (or fix it), eager-list costs.
- Second clean environment (another distro or VM) and one external developer completing the tutorial.
- Startup time and installed-size numbers before any footprint comparison with Electron.

## LATER (explicitly outside the preview)

npm package, AppImage/AUR/SDK bundles, Windows/macOS/Android/iOS, production AOT (needs C7 — shared reconciler —
and the QML/C++ handler-dialect unification from CRITICS §A), flattened rows, full CSS coverage, broad npm
compatibility claims, virtualization while a screen reader is active, a11y-complete widget set.

## Dependency-ordered phases

1. **Decide** (owner): scope/gate exception, license, version, platforms, channels, naming, C23, supported surface.
2. **Rights:** LICENSE/NOTICE + metadata in all shipped repos.
3. **Correctness:** merge `silent-drops`; C23 implementation + lifecycle tests; Menu functional fixes; keyboard/a11y
   mapping or documented boundary.
4. **Validation on a real display:** the supported demo + the live checks offscreen cannot do.
5. **Installability:** HTTPS submodule, dependency docs, starter, clean-machine build of a new app.
6. **CI:** the M7 pipeline green on the release candidate without local files or credentials.
7. **Evidence:** re-run scrolling/eager/AT benchmarks and PSS; publish methodology and raw data.
8. **Docs:** rewrite launch-facing docs; external tutorial test.
9. **Release:** freeze candidate from `widgets`, pin submodule SHAs, checksummed artifacts, release notes, tag.

## Owner decisions

**Decided 2026-10-10:** (1) scope = **0.x developer preview** (QML runtime path; narrower gate accepted: wins on
scrolling lists, eager lists and per-row memory documented as limits); (2) license = **MIT** for the framework,
transpiler and D bindings, **LGPL-3.0 for the C++ CSS engine** (it contains code derived from Qt, e.g. the literal
QQuickText port in csstextlayout).

**Still open:**

1. Preview (narrower gate, QML runtime only) or full release (original gate + AOT first)?
2. License for the framework, the engine and the D bindings (and the Qt redistribution model for apps).
3. Version number (next unused prerelease, e.g. 0.3.0-preview.1).
4. Supported platforms for the preview (distro, Wayland/X11, GPU validation scope).
5. Channels at launch (source only, or also binary SDK / npm / bundles).
6. Public name: "solid-qml", "solid-qml.js", "solid-qml-native" — one identity for repos and packages.
7. C23 contract for `<For>` row identity.
8. Supported vs experimental: C++ embedding, D bindings (solid-qml-d), WebView, Charts/3D, AOT.
