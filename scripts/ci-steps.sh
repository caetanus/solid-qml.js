#!/usr/bin/env bash
# Release-plan M7 ("CI") — the steps that run INSIDE a build environment once the repo
# and its submodules are already checked out: print pinned-tool versions, npm ci,
# typecheck, the transpiler test suite, the native (meson) build + tests, an installed
# -artifact smoke test (meson install to a prefix, then author + transpile + run a tiny
# app OUTSIDE the checkout against the installed artifacts).
#
# Factored out so TWO callers never drift from each other:
#   - .github/workflows/ci.yml   runs this directly inside its `archlinux:latest` job container.
#   - scripts/clean-room-check.sh (release-plan M6) runs this inside its own podman container,
#     via `podman exec`, as the clean-machine proof for an end user following the docs.
#
# Usage:
#   scripts/ci-steps.sh                 # run every step, in order
#   scripts/ci-steps.sh test-transpiler install smoke   # run only the named steps
#
# Steps: versions npm-ci typecheck test-transpiler native-build native-test install smoke
#
# Env:
#   APP_DIR     checkout root (default: this script's own repo root)
#   PREFIX_DIR  meson install destdir (default: $APP_DIR/.scratch/install)
#   SMOKE_DIR   where the "outside the checkout" tiny app is authored + run (default:
#               a directory NEXT TO $APP_DIR, never inside it, never /tmp)
#   JOBS        ninja -j / ctest parallelism (default: nproc)
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${APP_DIR:-$SELF_DIR}"
PREFIX_DIR="${PREFIX_DIR:-$APP_DIR/.scratch/install}"
SMOKE_DIR="${SMOKE_DIR:-$(cd "$APP_DIR/.." && pwd)/solidqml-ci-smoke}"
JOBS="${JOBS:-$(nproc)}"

cd "$APP_DIR"

step_versions() {
  echo "== ci-steps: pinned-environment versions (preview scope: Linux x86_64, Arch, Qt 6.11.2, Node 26, meson>=1.1 — see docs/RELEASE-PLAN.md) =="
  echo "-- OS --"; (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || uname -a
  echo "-- Qt --"; pkg-config --modversion Qt6Core 2>/dev/null || qmake6 -query QT_VERSION 2>/dev/null || echo "unknown"
  echo "-- Node --"; node --version
  echo "-- npm --"; npm --version
  echo "-- meson --"; meson --version
  echo "-- ninja --"; ninja --version
  echo "-- gcc --"; gcc --version | head -1
}

step_npm_ci() {
  echo "== ci-steps: npm ci =="
  npm ci
}

step_typecheck() {
  echo "== ci-steps: npm run typecheck =="
  npm run typecheck
}

step_test_transpiler() {
  echo "== ci-steps: npm run test:transpiler =="
  npm run test:transpiler
}

step_native_build() {
  echo "== ci-steps: meson setup + ninja -j $JOBS =="
  [ -d build ] && meson setup --reinit build || meson setup build
  ninja -C build -j "$JOBS"
}

step_native_test() {
  echo "== ci-steps: meson test (offscreen) =="
  # Every suite is a hard requirement — no allowed-failures. qml-css-engine:text (the
  # CssTextLayout-vs-live-QQuickText pixel/metric oracle) used to be waved through here
  # as "known font drift"; that was wrong (owner directive 2026-10-11) and the actual
  # cause (see docs/RELEASE-PLAN.md M7 notes) is fixed at the environment/engine level,
  # not papered over in CI.
  QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
    meson test -C build --timeout-multiplier 3 --print-errorlogs
}

step_install() {
  echo "== ci-steps: meson install --destdir $PREFIX_DIR =="
  meson install -C build --destdir "$PREFIX_DIR"
}

step_smoke() {
  local prefix="$PREFIX_DIR/usr/local"
  echo "== ci-steps: installed-artifact smoke test — a NEW app OUTSIDE the checkout, at $SMOKE_DIR =="
  rm -rf "$SMOKE_DIR"
  mkdir -p "$SMOKE_DIR"
  cp "$APP_DIR/tests/clean-room/hello-app/counter.tsx" "$SMOKE_DIR/counter.tsx"
  cp "$APP_DIR/tests/clean-room/hello-app/counter.css" "$SMOKE_DIR/counter.css"
  sed -i "s#__RUNTIME__#$APP_DIR/src/solid-qml/runtime#" "$SMOKE_DIR/counter.tsx"
  # Absolute path to tsx's loader: a bare `--import tsx` resolves starting at the
  # INVOKING file's own directory, which is outside the checkout here (release-plan M6
  # clean-room finding).
  (cd "$SMOKE_DIR" && node --import "$APP_DIR/node_modules/tsx/dist/loader.mjs" \
    "$APP_DIR/scripts/gen.mjs" "$SMOKE_DIR/counter.tsx" "$SMOKE_DIR/qmlout/App.generated.qml")
  (cd "$SMOKE_DIR" && QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
    LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$prefix/bin/solid-qml-loader" --qml "$SMOKE_DIR/qmlout/App.generated.qml" \
    --click 60,60,300 --grab "$SMOKE_DIR/shot.png")
  [ -s "$SMOKE_DIR/shot.png" ] || { echo "ci-steps: smoke test produced no screenshot" >&2; exit 1; }
  echo "ci-steps: smoke test PASSED. Screenshot: $SMOKE_DIR/shot.png"
}

ALL_STEPS=(versions npm-ci typecheck test-transpiler native-build native-test install smoke)
steps=("$@")
[ ${#steps[@]} -eq 0 ] && steps=("${ALL_STEPS[@]}")

for s in "${steps[@]}"; do
  case "$s" in
    versions) step_versions ;;
    npm-ci) step_npm_ci ;;
    typecheck) step_typecheck ;;
    test-transpiler) step_test_transpiler ;;
    native-build) step_native_build ;;
    native-test) step_native_test ;;
    install) step_install ;;
    smoke) step_smoke ;;
    *) echo "ci-steps: unknown step '$s' (known: ${ALL_STEPS[*]})" >&2; exit 2 ;;
  esac
done
