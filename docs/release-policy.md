# Release policy

Status: 2026-10-11 (release-plan M8). This is the policy, not a specific release's notes — those
live under `docs/release-notes/`.

## Versioning scheme

- **Scheme:** [SemVer](https://semver.org) with an optional prerelease suffix:
  `MAJOR.MINOR.PATCH[-preview.N]` (also `-alpha.N` / `-rc.N` as the project matures past preview).
  Current: **0.3.0-preview.1**.
- **One source of truth:** the app's version is the literal `version:` string on the top-level
  `project()` call in `meson.build`. Everything else that reports or records a version is either
  *derived* from it at build time or *checked* against it:
  - `-DSOLID_QML_VERSION="<meson.project_version()>"` (a compiler define, `meson.build`) feeds
    `NodeShims::solidQmlVersion()` (`src/shims/nodeshims.cpp`), which is what
    `process.versions.solidQml` reports to app code at runtime (the Electron-idiom probe) — this
    is derived automatically, there is nothing to keep in sync by hand.
  - `package.json` `version`, `package-lock.json` (`version` and `packages[""].version`),
    `docs/conf.py` `release`, and `docs/v4-engine/conf.py` `release` are plain strings that must
    be kept equal to the `meson.build` version by hand, and are **checked** by
    `scripts/check-version-consistency.mjs`, wired into `meson test` as the `version-consistency`
    test (also runnable as `npm run check:version`). A release is not cut while that test fails.
  - `package-lock.json`'s root `name`/`version` are regenerated with
    `npm install --package-lock-only` after bumping `package.json` — don't hand-edit the lock file.
  - The **engine** (`vendor/qml-css-engine`) deliberately does **not** follow the app's version —
    see "The engine's own semver" below. It is excluded from the consistency check on purpose.
- **Prerelease suffix compatibility** (why this is split instead of one literal everywhere):
  - `meson.project_version()`, `package.json`'s `version`, and the two Sphinx `release` strings
    are free text — `0.3.0-preview.1` is fine there, and that is what they carry.
  - **pkg-config**'s generated `solidqml.pc` `Version:` field is also free text as far as
    `pkg.generate()` is concerned (it is written verbatim); verified by configuring with the
    `-preview.1` suffix in place — meson raises no error. Consumers doing a strict
    `pkg-config --atleast-version` comparison against a prerelease string get pkg-config's
    plain dot-separated comparison, which is not full SemVer precedence — not a concern for a
    0.x preview with no other package depending on a minimum version yet.
  - **CMake's generated package-version file does not accept the suffix.**
    `cmake.write_basic_package_version_file()` writes a `SolidQmlConfigVersion.cmake` that
    CMake's own `AnyNewerVersion` compatibility check parses as a numeric
    `major[.minor[.patch[.tweak]]]`; a `-preview.1` suffix breaks that parse. `meson.build`
    therefore passes it a **separate, numeric-only `'0.3.0'`**, not `meson.project_version()`.
    Anyone consuming `solid-qml` via `find_package(SolidQml)` sees `0.3.0`, not
    `0.3.0-preview.1` — documented here so the two don't look like drift.
  - The **library's own `version`/`soversion`** (next section) are also numeric-only, for the
    same reason (ELF/`.so` filename suffixes are conventionally numeric) and are deliberately
    decoupled from the project's marketing version.

## SONAME / ABI statement for `libsolidqml`

- `libsolidqml` (built by the top-level `shared_library('solidqml', ...)` in `meson.build`) ships
  with **`soversion: '0'`** for the entire 0.x series, and a numeric **`version: '0.3.0'`**
  (so `libsolidqml.so.0.3.0`, `SONAME` `libsolidqml.so.0`).
- **No ABI stability is promised in 0.x.** `soversion` stays `'0'` across every 0.x release;
  bumping it to `'1'` is reserved for a deliberate, documented ABI break, not automatic per
  release. Do not link against `libsolidqml` expecting a minor bump to be ABI-compatible.
- **It links against Qt's private headers** (`QtQuickTemplates2/private`, Qt's `Core`
  `private` headers via `core_private_deps` in `meson.build`) to reach `QQuickItemPrivate`
  and the Templates internals the widgets and the a11y path need. Private headers carry
  **no compatibility guarantee from Qt itself**, even across Qt patch releases. Consequently:
  - `libsolidqml` is built for **exactly one Qt minor version** — currently **Qt 6.11** (pinned
    per `docs/getting-started.md`'s supported environment).
  - **Rebuild `libsolidqml` (and anything statically linking its object code) on any Qt upgrade
    (including a patch release) or any `solid-qml` upgrade.** There is no cross-Qt-version or
    cross-solid-qml-version binary compatibility promise, in either direction.
  - A distribution packaging `solidqml` must treat it the same way it treats a Qt-private-header
    consumer internally — i.e. rebuild it alongside Qt, not independently.
- The CSS engine (`vendor/qml-css-engine`) builds as a **static library**
  (`qml_css_engine_lib = static_library(...)` in its `meson.build`) — it has no `.so`/SONAME of
  its own; it is compiled into `libsolidqml`.

## The engine's own semver

- `vendor/qml-css-engine` is a separate repository (consumed as a git submodule, resolved by
  meson as `subproject('qml-css-engine')`) with its **own** `meson.build` `project()` version,
  bumped on its own schedule by its own maintainers — currently **0.2.0** (bumped from 0.1.0 as
  part of this release; see its `CHANGELOG.md`).
- The app does **not** mirror or re-export the engine's version. It pins the submodule to a
  specific commit SHA (`git submodule status` / the gitlink recorded in the app's tree), which is
  the only thing a release commit needs to record about the engine (see "Cutting a release"
  below) — not a version number.
- Consumers who need the engine's version at runtime can read it from its own
  `meson.project_version()` if they build against it directly (e.g. via its CMake package,
  `find_package(qml-css-engine)`); `solid-qml`'s own `process.versions.solidQml` reports the
  **app's** version only.

## Public name vs. identifiers that stay as they are

The owner decided (2026-10-11, `docs/RELEASE-PLAN.md`) the public-facing name is **solid-qml**
(the engine, "qml-css-engine", and the D bindings, "solid-qml-d", keep their own repo names).
"Public-facing" means prose, titles and metadata a user reads — not every internal identifier.
Renaming an identifier that's load-bearing in a build system, an install path, or a consumer's
`import`/`find_package`/`#include` is a breaking change with real blast radius, so each one below
was evaluated on its own rather than renamed by reflex:

| Identifier | Where | Recommendation | Why |
|---|---|---|---|
| `package.json` `"name"` | `package.json` | **Renamed** `solid-qml-native` → `solid-qml` | Explicitly in scope (release-plan M8 item 2); the package is `"private": true` (never published), so nothing resolves it by name — zero blast radius. |
| README/docs titles and prose | `README.md`, `docs/index.md`, `docs/architecture.md`, `ROADMAP.md`, `docs/conf.py`, `docs/v4-engine/conf.py` | **Renamed** `solid-qml.js`/`solid-qml-native` → `solid-qml` | Pure prose/doc-title; no code reads these strings. |
| GitHub repo slug | `git remote` → `github.com/caetanus/solid-qml.js` | **Keep** (explicitly out of scope for this task) | The repo is *already* named `solid-qml.js`, one slug off from the new public name; every clone URL in the docs (`docs/getting-started.md`, `scripts/clean-room-check.sh`) correctly points at it and was left untouched. Renaming a GitHub repo is a one-way, user-visible action (old URLs 301 but every fork/clone/bookmark/CI config pinned to the old slug is affected) — do only with the owner, at release time, not as a side effect of a docs pass. |
| meson project name | `meson.build` → `project('solid-qml-native', 'cpp', ...)` | **Keep** | Purely internal to the build (subproject name other projects `subproject('solid-qml-native')` against, if any ever vendor this the way this app vendors the engine); not user-facing. Renaming forces every consumer's build file to change in lockstep with no user-visible benefit. |
| Shared library name | `libsolidqml` (`shared_library('solidqml', ...)`) | **Keep** | Breaking: changes the `.so` filename, every `-lsolidqml` link line, `install_rpath`/`dependencies` consumers, and the loader's own link step. No public-name confusion today (nothing public is called "libsolidqml" yet). |
| pkg-config package name | `solidqml` (`pkg.generate(name: 'solidqml', ...)` → `solidqml.pc`) | **Keep** | Breaking for any out-of-tree `pkg-config --cflags solidqml` / `dependency('solidqml')` consumer once this package is installed (`-Dembed=enabled`, the default). In-tree consumers (`examples/embed-host`, the loader, SolidTerm) link `solidqml_dep` directly and would not notice a rename, but installed consumers outside this build would. |
| CMake package name | `SolidQml` (`cmake.write_basic_package_version_file(name: 'SolidQml', ...)`, `cmake/SolidQmlConfig.cmake.in`) | **Keep** | Breaking for any `find_package(SolidQml)` consumer, same reasoning as pkg-config. |
| Install paths | `get_option('libdir')`, `subdir: 'solidqml'` (headers), `datadir / 'solidqml'` | **Keep** | Breaking for anything that already found headers/data under `.../solidqml/`; low value to move since the library/package names above are staying too (would create a mismatch: package `SolidQml` installing under a renamed path). |
| npm scripts | `package.json` `"scripts"` (`dev`, `build`, `gen:cpp`, `twin-render`, …) | **Keep** | Internal dev commands, not published, not user-facing; renaming buys nothing and touches CI (M7, out of scope here) and every doc that tells a developer to run `npm run gen:cpp`. |
| Loader binary name | `solid-qml-loader` (`executable('solid-qml-loader', ...)`) | **Keep** | Already matches the new public name (`solid-qml-loader`, not `solid-qml-native-loader`); no change needed. |
| Repo checkout directory name | the local clone directory, literally `solid-qml-native` on disk (unrelated to the GitHub slug) | **Keep, documented as a known mismatch** | Cosmetic only — a few comments/examples refer to "the solid-qml-native checkout" by its folder name, which is accurate for whoever cloned it under that name. Not a published identifier; left alone rather than rewritten to match a name the repo itself isn't checked out as. |

Only the first two rows (package.json `name` + doc-level prose) were applied in this pass, per
the M8 scope ("apply only doc-level/user-facing renames now").

## Cutting a release (no automation required)

1. Decide the version (this policy's scheme) and update `meson.build`'s `project()` version —
   the single source of truth.
2. Update anything the consistency check covers to match
   (`package.json`, then `npm install --package-lock-only` to refresh the lock file;
   `docs/conf.py`; `docs/v4-engine/conf.py`). Run `npm run check:version` (or `meson test
   version-consistency`) until it's green.
3. If the engine (`vendor/qml-css-engine`) changed: bump *its own* version in *its own*
   `meson.build` and `CHANGELOG.md` on its own branch/commit (independent semver — see above),
   land that commit, then update the app's submodule gitlink to pin that exact SHA
   (`git -C vendor/qml-css-engine checkout <sha> && git add vendor/qml-css-engine`). Record the
   pinned SHA in the release notes.
4. Write `docs/release-notes/<version>.md`: what changed since the last release (scoped,
   factual — no claims beyond what's measured; link the benchmark methodology or mark a number
   "to be published"), known limits, supported environment.
5. Run the full gate: `QT_QPA_PLATFORM=offscreen meson test -C <build>` (full suite; the
   `qml-css` subproject suite can time out under load — rerun it alone if so), the transpiler
   tests, `npm run typecheck`, and `version-consistency`.
6. Produce checksummed artifacts for the release (source tarball at minimum for a 0.x source
   release; see `docs/RELEASE-PLAN.md` for the supported-channel scope) — `sha256sum` alongside
   each artifact.
7. Commit the version bump (this policy's step 1–2) with a message naming the version.
8. **Tag** — an **immutable** tag (e.g. `v0.3.0-preview.1`) made by the owner at release time
   together with the pinned submodule SHA and the checksummed artifacts from step 6. This is
   deliberately not scripted here: no agent should create or move a release tag unattended.
