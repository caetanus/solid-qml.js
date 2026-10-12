#!/usr/bin/env bash
# macOS CI (release-plan M7's Linux pin stays the ONE supported environment; this is the
# additional "also build + test on macOS" owner directive, 2026-10-11). Mirrors the PORTABLE
# steps of scripts/ci-steps.sh (npm ci, typecheck, transpiler tests, meson build/test/install,
# an installed-artifact smoke test) but adapted for Qt installed via aqtinstall into frameworks,
# Darwin's dylib rpath syntax, and no D-Bus/Wayland.
#
# Run by .github/workflows/macos.yml inside a macos-14 (arm64) / macos-13 (x86_64) job, AFTER
# that workflow has: installed Qt via aqtinstall and exported QT_MAC_FRAMEWORKS_DIR (the ".../lib"
# dir the Qt .frameworks live in — meson.build's mac_qt_private_dep needs it, see the comment
# there) and put Qt's bin/ + a pinned Node + a pinned meson/ninja on PATH; installed libvterm via
# Homebrew.
#
# Usage:
#   scripts/ci-macos.sh                 # run every step, in order
#   scripts/ci-macos.sh native-build native-test   # run only the named steps
#
# Steps: versions npm-ci typecheck test-transpiler native-build native-test install bundle-solidterm smoke
#
# Env: APP_DIR PREFIX_DIR SMOKE_DIR JOBS (same meaning as ci-steps.sh)
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${APP_DIR:-$SELF_DIR}"
PREFIX_DIR="${PREFIX_DIR:-$APP_DIR/.scratch/install}"
SMOKE_DIR="${SMOKE_DIR:-$(cd "$APP_DIR/.." && pwd)/solidqml-ci-smoke-macos}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

cd "$APP_DIR"

step_versions() {
  echo "== ci-macos: pinned-environment versions (macOS job — see .github/workflows/macos.yml) =="
  echo "-- OS --"; sw_vers
  echo "-- arch --"; uname -m
  echo "-- Xcode clang --"; clang --version
  echo "-- Qt --"; pkg-config --modversion Qt6Core 2>/dev/null || qmake6 -query QT_VERSION 2>/dev/null || echo "unknown"
  echo "-- Qt prefix --"; qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || echo "unknown"
  echo "-- Node --"; node --version
  echo "-- npm --"; npm --version
  echo "-- meson --"; meson --version
  echo "-- ninja --"; ninja --version
  echo "-- libvterm (pkg-config) --"; pkg-config --modversion vterm 2>/dev/null || echo "not found"
  echo "-- QT_MAC_FRAMEWORKS_DIR --"; echo "${QT_MAC_FRAMEWORKS_DIR:-<unset>}"
}

step_npm_ci() {
  echo "== ci-macos: npm ci =="
  npm ci
}

step_typecheck() {
  echo "== ci-macos: npm run typecheck =="
  npm run typecheck
}

step_test_transpiler() {
  echo "== ci-macos: npm run test:transpiler =="
  npm run test:transpiler
}

step_native_build() {
  echo "== ci-macos: meson setup + ninja -j $JOBS =="
  : "${QT_MAC_FRAMEWORKS_DIR:?QT_MAC_FRAMEWORKS_DIR must be set to the dir the Qt .frameworks live in -- see the mac_qt_private_dep comment in meson.build}"
  # Every optional component forced ON: a feature that can't find its dependency on this
  # platform must fail the configure LOUDLY (meson error), never silently skip (owner directive).
  local setup_args=(
    -Dloader=enabled -Dexamples=enabled -Daot=enabled -Drichtext=enabled
    -Dsolidterm=enabled -Dembed=enabled
  )
  if [ -d build ]; then
    meson setup --reconfigure build "${setup_args[@]}"
  else
    meson setup build "${setup_args[@]}"
  fi
  ninja -C build -j "$JOBS"
}

step_native_test() {
  echo "== ci-macos: meson test (offscreen) =="
  QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
    meson test -C build --timeout-multiplier 3 --print-errorlogs
}

step_install() {
  echo "== ci-macos: meson install --destdir $PREFIX_DIR =="
  meson install -C build --destdir "$PREFIX_DIR"
}

step_bundle_solidterm() {
  echo "== ci-macos: wrap solidterm as a thin, unsigned SolidTerm.app =="
  local prefix="$PREFIX_DIR/usr/local"
  local bin="$prefix/bin/solidterm"
  if [ ! -x "$bin" ]; then
    echo "ci-macos: $bin not found — solidterm was not installed, nothing to bundle" >&2
    return 0
  fi
  local app="$PREFIX_DIR/SolidTerm.app"
  rm -rf "$app"
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
  # Launches the SAME installed binary (dylib rpath is @loader_path-relative — see meson.build —
  # so it resolves libsolidqml.dylib right from here) — not a self-contained, macdeployqt-style
  # bundle: Qt itself is NOT embedded, exactly like the installed prefix it wraps isn't
  # redistributable either (owner artifact spec: the prefix + this thin bundle, unsigned).
  cp "$bin" "$app/Contents/MacOS/solidterm"
  cp "$APP_DIR/apps/solidterm/solidterm.svg" "$app/Contents/Resources/solidterm.svg" 2>/dev/null || true
  cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>SolidTerm</string>
  <key>CFBundleDisplayName</key><string>SolidTerm</string>
  <key>CFBundleIdentifier</key><string>dev.solidqml.solidterm</string>
  <key>CFBundleVersion</key><string>0.3.0</string>
  <key>CFBundleShortVersionString</key><string>0.3.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>solidterm</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
  echo "ci-macos: wrote $app (unsigned; codesign --force --deep --sign - to run outside this CI's own Gatekeeper-exempt context)"
}

step_smoke() {
  local prefix="$PREFIX_DIR/usr/local"
  echo "== ci-macos: installed-artifact smoke test — a NEW app OUTSIDE the checkout, at $SMOKE_DIR =="
  rm -rf "$SMOKE_DIR"
  mkdir -p "$SMOKE_DIR"
  cp "$APP_DIR/tests/clean-room/hello-app/counter.tsx" "$SMOKE_DIR/counter.tsx"
  cp "$APP_DIR/tests/clean-room/hello-app/counter.css" "$SMOKE_DIR/counter.css"
  # Portable -i spelling: BSD sed (macOS's own /usr/bin/sed) requires the backup-suffix argument
  # ATTACHED to -i (space-separated means "no suffix, and the next arg is the script" on BSD,
  # which would misparse); GNU sed (this job also puts GNU sed's gnubin on PATH, for
  # tests/c23/run-virtual.sh) accepts the attached form the same way. Attached + always clean up
  # the backup file works identically on both.
  sed -i.bak "s#__RUNTIME__#$APP_DIR/src/solid-qml/runtime#" "$SMOKE_DIR/counter.tsx"
  rm -f "$SMOKE_DIR/counter.tsx.bak"
  (cd "$SMOKE_DIR" && node --import "$APP_DIR/node_modules/tsx/dist/loader.mjs" \
    "$APP_DIR/scripts/gen.mjs" "$SMOKE_DIR/counter.tsx" "$SMOKE_DIR/qmlout/App.generated.qml")
  (cd "$SMOKE_DIR" && QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
    DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    DYLD_FALLBACK_LIBRARY_PATH="$prefix/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}" \
    "$prefix/bin/solid-qml-loader" --qml "$SMOKE_DIR/qmlout/App.generated.qml" \
    --click 60,60,300 --grab "$SMOKE_DIR/shot.png")
  [ -s "$SMOKE_DIR/shot.png" ] || { echo "ci-macos: smoke test produced no screenshot" >&2; exit 1; }
  echo "ci-macos: smoke test PASSED. Screenshot: $SMOKE_DIR/shot.png"
}

ALL_STEPS=(versions npm-ci typecheck test-transpiler native-build native-test install bundle-solidterm smoke)
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
    bundle-solidterm) step_bundle_solidterm ;;
    smoke) step_smoke ;;
    *) echo "ci-macos: unknown step '$s' (known: ${ALL_STEPS[*]})" >&2; exit 2 ;;
  esac
done
