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
| M1 | **License** — ✅ DONE 2026-10-11 (MIT app/transpiler/D bindings, LGPL-3.0-only engine + NOTICE for the QQuickText-derived files) | No LICENSE file; `meson.build` `LicenseRef-NONE`, `package.json` `UNLICENSED`, README "TBD"; the GitHub repo is PUBLIC without a license | LICENSE + NOTICE in the app, engine and D-binding repos; metadata aligned; Qt-derived code (csstextlayout is a literal QQuickText port) and Qt LGPL obligations inventoried |
| M2 | **No silent miscompiles** — silent-drops merged (62f76db) + 2026-10 fixes: module consts/helpers, For index, const classification, effect deps | Branch `silent-drops` (7 commits, 582/582 transpiler tests): multi-root Show/Match, `<text onClick>`, dynamic placeholder/disabled/readOnly/class, `<Switch fallback>`, fail-loud attribute contract | Merged after solid-mail and solid-player run their UIs through it; AOT emitter covered by the same contract or marked unsupported |
| M3 | **C23 decided** (row identity) — DECIDED 2026-10-11: reference identity like web by default; `key="field"` opt-in for reuse by id — ✅ IMPLEMENTED (engine `CssRepeater.key`, transpiler `<For key>`, contract in SOLID-PARITY.md; tests: engine repeater suite, `meson test for-identity for-identity-virtual`) | New objects with the same `id` reuse the row; Solid web recreates it | Owner picks a contract; executable mount/cleanup/state tests; benchmarks re-run |
| M4 | **Core behaviour validated on a real display** | Offscreen cannot validate popups/GPU; menu positioning repro waits for a live run (RESUME-HERE); Menu has functional sizing defects (clipped labels, CSS sizing ignored) | Supported demo passes: forms, list mutations, keyboard/focus, screen reader activation, resize, hide/show, menus |
| M5 | **Keyboard/a11y for essential controls** — ✅ DONE 2026-10-11: tabIndex/role/aria-* on a plain element map to `activeFocusOnTab`/`Accessible.*` (docs/keyboard-accessibility.md) + Enter/Space activation; no engine change needed (both are standard `QtQuick` facilities already used by the native widgets) | `<div role="button" tabIndex aria-label>` used to have no native effect (a warning only) | tabIndex/role/aria-label mapping (done) or documented: essential controls use native widgets |
| M6 | **Install story from a clean machine** — ✅ DONE 2026-10-11: verified end to end in a from-scratch `archlinux:latest` container (`scripts/clean-room-check.sh`, `--local` run against the fix below; evidence in `docs/evidence/m6-clean-room-counter.png`) | HTTPS submodule URL (done 2026-10-11); accurate dependency list in `docs/getting-started.md`; a NEW app (`tests/clean-room/hello-app`) transpiles and runs OUTSIDE the checkout against the INSTALLED `solid-qml-loader`, offscreen, clicked and re-rendered (count 0→1) | HTTPS submodule URL; dependency list (Qt modules incl. private Templates headers, Node version); a new app builds and runs outside the framework checkout on a clean machine |
| M7 | **CI** — locally verified 2026-10-11: `.github/workflows/ci.yml` (`.github/workflows/ci.yml`), steps factored into `scripts/ci-steps.sh` (shared with `scripts/clean-room-check.sh`); full run on a date-pinned Arch snapshot green end to end (npm ci, typecheck, transpiler tests, native build, `meson test` 14/14 incl. `qml-css-engine:text`, install, outside-the-checkout smoke test) | Engine submodule had its own CI (Debian); it was RED on master (`CMake mirror build (Debian stable)`: `Could NOT find Qt6QuickPrivate` — Debian ships no CMake config for any Qt6 `*Private` component, only the headers; fixed in the engine, `CMakeLists.txt`, with the same private-include-path convention meson's qt6 module already used). Also found + fixed: `archlinux:latest`'s rolling mirror had drifted to Qt 6.12.0, which is what was actually behind the "qml-css-engine:text font-metric drift" `scripts/clean-room-check.sh` used to wave through — not fonts. Both CI and the clean-room check now pin pacman to `archive.archlinux.org/repos/2026/09/15` (verified: exactly Qt 6.11.2, meson 1.12.0, nodejs 26.8.2) | Clean checkout + submodules, locked deps, typecheck, transpiler + native tests, an installed-artifact smoke test, on the pinned environment — all ✅ locally (podman, this container image, these steps); the first GREEN RUN ON GITHUB ITSELF still needs the owner to push `m7-ci` (app) and `m7-ci` (engine submodule) — not pushed per instruction (owner approves pushes) |
| M8 | **Versioning and release policy** — ✅ DONE 2026-10-11: one version everywhere (`0.3.0-preview.1`, checked by `scripts/check-version-consistency.mjs` in `meson test`/`npm test`); SONAME/ABI policy in `docs/release-policy.md` (`soversion: '0'` for all of 0.x, private-Qt-header rebuild statement); public name "solid-qml" applied at doc level (identifier rename table in `docs/release-policy.md`); engine bumped to its own `0.2.0` on branch `m8-version`, submodule pinned; release notes at `docs/release-notes/0.3.0-preview.1.md` | `meson.build`/`package.json` 0.2.0, `package-lock.json` 0.1.0, `docs/conf.py` 0.1.0; no SONAME policy; tags v0.1.0-alpha.1, v0.2.0-alpha.1 exist | One version everywhere; SONAME/ABI statement (private Qt headers ⇒ no cross-Qt-version promise); release notes; immutable tag (tag itself still pending — cut by the owner at release time, see `docs/release-policy.md`) |
| M9 | **Accurate docs** — ✅ DONE 2026-10-11 (pending a human external tester): `docs/architecture.md` rewritten; `docs/support-matrix.md` with generated tables (`scripts/support-matrix.mjs`, drift test `transpiler/test/support-matrix-doc.test.ts`); `docs/known-issues.md`; README front page; SOLID-PARITY/todomvc-gaps/embed README corrected; outsider walkthrough by a no-context agent, findings fixed (`docs/evidence/m9-outsider-walkthrough.md`). Found while verifying: several constructs compile without error but are wrong at runtime (listed ⚠️ in the matrix) — M2 follow-up | README/architecture describe widgets as .qml (they are C++), engine under `subprojects/` (it is `vendor/`); `examples/todomvc-gaps.md` contradicts SOLID-PARITY; embed README describes a fixed defect | Getting-started, architecture, supported TSX/CSS/widget/shim matrix, known issues; tested by someone outside the project |
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

**Decided 2026-10-11:** version **0.3.0-preview.1** (app); public name **solid-qml** (the engine and the D
bindings keep their own repo names); the engine keeps its **own semver** (the app pins the submodule SHA);
C23 = reference identity by default with `key` opt-in; copyright holder "Marcelo A Caetano".

**Still open:**

1. Preview (narrower gate, QML runtime only) or full release (original gate + AOT first)?
2. License for the framework, the engine and the D bindings (and the Qt redistribution model for apps).
3. Supported platforms for the preview (distro, Wayland/X11, GPU validation scope).
4. Channels at launch (source only, or also binary SDK / npm / bundles).
5. C23 contract for `<For>` row identity.
6. Supported vs experimental: C++ embedding, D bindings (solid-qml-d), WebView, Charts/3D, AOT.

(Version number and public name were open items 3 and 6 — resolved by the "Decided 2026-10-11"
line above and M8; removed from this list rather than renumbered away silently.)
