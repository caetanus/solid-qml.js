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
- ONE supported environment: Linux x86_64, glibc >= 2.34, pinned Qt 6.11.2 — CI/reference environment is
  manylinux_2_34 (`quay.io/pypa/manylinux_2_34_x86_64`, Qt's official aqtinstall binaries); the owner's local dev
  machine (Manjaro/Arch) is the documented day-to-day path — Wayland and X11 as tested;
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
| M7 | **CI** — moved to manylinux 2026-10-11 (owner directive: CI must run on manylinux, not Arch). `.github/workflows/ci.yml` job `linux` now runs on `quay.io/pypa/manylinux_2_34_x86_64` (AlmaLinux 9, glibc 2.34) with Qt 6.11.2 from official aqtinstall binaries (not distro packages) — matches Qt's own RHEL-9-family reference platform instead of a rolling-release snapshot that needs manual re-pinning every time either side moves. Steps factored into `scripts/ci-steps.sh` (environment-agnostic; shared with `scripts/clean-room-check.sh`), provider setup (dnf + aqtinstall + a checksum-pinned Node tarball) isolated in the workflow and in `clean-room-check.sh --manylinux`. Verified end to end locally via podman 2026-10-11: `clean-room-check.sh --manylinux --local`, 798s wall-clock warm-cache (dnf installs + full `-j3` build from a clean `build/` + 14/14 `meson test` incl. `qml-css-engine:text` + install + outside-the-checkout smoke test, screenshot captured); a separate run measured the Qt 6.11.2 `linux_gcc_64` aqtinstall download itself at 47.6s / 1.5 GB on a cold cache. Two real bugs found and fixed along the way: (1) `scripts/ci-steps.sh` was tracked as git mode 100644 (non-executable) — the first real GitHub run (caetanus/solid-qml.js#38184980004) hit this as `Permission denied` (exit 126); fixed via `git update-index --chmod=+x` plus invoking it as `bash scripts/ci-steps.sh` everywhere (belt-and-suspenders); (2) the smoke-test screenshot/log upload used a `../` relative path, which `actions/upload-artifact` rejects outright — fixed by copying those artifacts into `ci-artifacts/` under the workspace before upload (`if-no-files-found: warn`). Also found + fixed at the environment level (manylinux, not Arch-specific): aqtinstall's Qt is self-contained and not registered with `ldconfig` — the *installed* `solid-qml-loader` (plain `meson install --destdir` copy, no rpath back to the Qt prefix) needs `LD_LIBRARY_PATH` pointed at Qt's `lib/` dir to find `libQt6Core.so.6` at runtime; the build tree itself needs no such thing (meson's qt6 dependency already sets an rpath there). The Arch path (`scripts/clean-room-check.sh`, no flag) was re-run after this move to confirm it still passes unchanged; it remains the supported LOCAL dev-machine verification path, documented in `docs/getting-started.md`, but is no longer what CI runs. | Pending from this same move: macOS and Windows CI (owner decision 2026-10-11, build + tests + an unsigned artifact each) — two other agents own separate workflow files (`.github/workflows/macos.yml`, `windows.yml`) and their own step scripts; this job was renamed to `linux` (name: "linux (manylinux_2_34, Qt 6.11.2)") and `scripts/ci-steps.sh`'s portable steps (`npm-ci`, `typecheck`, `test-transpiler`) are marked for them to mirror. The first GREEN RUN ON GITHUB ITSELF (this manylinux workflow) still needs the owner to push — not pushed per instruction (owner approves pushes) |
| M8 | **Versioning and release policy** — ✅ DONE 2026-10-11: one version everywhere (`0.3.0-preview.1`, checked by `scripts/check-version-consistency.mjs` in `meson test`/`npm test`); SONAME/ABI policy in `docs/release-policy.md` (`soversion: '0'` for all of 0.x, private-Qt-header rebuild statement); public name "solid-qml" applied at doc level (identifier rename table in `docs/release-policy.md`); engine bumped to its own `0.2.0` on branch `m8-version`, submodule pinned; release notes at `docs/release-notes/0.3.0-preview.1.md` | `meson.build`/`package.json` 0.2.0, `package-lock.json` 0.1.0, `docs/conf.py` 0.1.0; no SONAME policy; tags v0.1.0-alpha.1, v0.2.0-alpha.1 exist | One version everywhere; SONAME/ABI statement (private Qt headers ⇒ no cross-Qt-version promise); release notes; immutable tag (tag itself still pending — cut by the owner at release time, see `docs/release-policy.md`) |
| M9 | **Accurate docs** | README/architecture describe widgets as .qml (they are C++), engine under `subprojects/` (it is `vendor/`); `examples/todomvc-gaps.md` contradicts SOLID-PARITY; embed README describes a fixed defect | Getting-started, architecture, supported TSX/CSS/widget/shim matrix, known issues; tested by someone outside the project |
| M10 | **Honest performance claims** — ✅ DONE 2026-10-12: methodology + results in `docs/performance.md` (generated from `bench/results/2026-10-12-numenor*.json` by `scripts/bench/report.mjs`); one runner `scripts/bench/run.mjs` (native + headless Chromium, load-gated, ≥ 20 warm samples/op, median/p95/min, sync + next tick + presented frame, PSS/row, startup); README and release-notes claims scoped to it | Measured (release build, null RHI, Chromium 153): scrolling list (virtualized) faster on 6/6 @1000 (4–15× sync, also to the presented frame) and @10000 (6.8–16×); eager list slower on create/append/update-without-key/clear @1000 (2.6×/2.6×/2.6×/1.4×) and on 6/6 @10000; ~21 vs ~11 KB PSS per materialized row; GPU presentation and Electron not measured | Published methodology + raw results; claim scoped ("in our 1,000-row scrolling-list benchmark…"); limits stated (virtualization does the work; eager lists and per-row memory still behind; sync timings, not presented frames) |

## SHOULD (preview quality)

- Starter template repo; one realistic supported demo (SolidTerm or a lists+forms app) with tests.
- ✅ Matched presented-frame timing on web (double rAF) next to the native frameSwapped numbers (M10, `docs/performance.md`).
- Presented-frame / frame-time numbers on a real GPU (both sides) — M10 measured null-RHI vs headless Chromium only.
- Known-issues table: Menu theming limits, WebView black after hide/show (or fix it), eager-list costs.
- Second clean environment (another distro or VM) and one external developer completing the tutorial.
- Startup time and installed-size numbers before any footprint comparison with Electron (M10 has bench-app startup vs headless Chromium only — no Electron app measured, no installed size).

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
