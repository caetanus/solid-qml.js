#!/usr/bin/env bash
# Release-plan M6 ("install story from a clean machine") / M7 (CI) check.
#
# Spins up a FRESH Arch Linux container, installs ONLY the packages in the dependency list below,
# clones the public repo over HTTPS with submodules, builds + tests + installs the framework, then
# authors and runs a NEW app (tests/clean-room/hello-app) OUTSIDE the checkout against the
# installed artifacts, and grabs a screenshot offscreen as proof it rendered.
#
# Usage:
#   scripts/clean-room-check.sh                 # clone the given branch over HTTPS (default)
#   scripts/clean-room-check.sh --local          # copy THIS worktree in instead of cloning
#   REPO_BRANCH=widgets scripts/clean-room-check.sh
#
# Requires: podman. Does not touch anything outside its own container + $SCRATCH_DIR (defaults to
# a dir under the repo's .scratch/, never /tmp — see docs/getting-started.md "Clean machine" and
# the owner's RAM-ceiling / tmpfs rules).
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/caetanus/solid-qml.js.git}"
REPO_BRANCH="${REPO_BRANCH:-widgets}"
CONTAINER_NAME="${CONTAINER_NAME:-solidqml-clean-room}"
IMAGE="${IMAGE:-docker.io/library/archlinux:latest}"
MEM_LIMIT="${MEM_LIMIT:-8g}"
JOBS="${JOBS:-4}"
MODE="clone"
[ "${1:-}" = "--local" ] && MODE="local"

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH_DIR="${SCRATCH_DIR:-$SELF_DIR/.scratch}"
SHOTS_DIR="$SCRATCH_DIR/clean-room-shots"
mkdir -p "$SCRATCH_DIR/tmp" "$SHOTS_DIR"
export TMPDIR="$SCRATCH_DIR/tmp"   # never /tmp (tmpfs = RAM) on the host side

cleanup() {
  podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== clean-room-check: pulling $IMAGE =="
podman pull "$IMAGE"

echo "== clean-room-check: starting container (--memory $MEM_LIMIT) =="
cleanup
podman run -d --name "$CONTAINER_NAME" --memory "$MEM_LIMIT" "$IMAGE" sleep infinity >/dev/null
pexec() { podman exec "$CONTAINER_NAME" bash -lc "$1"; }

# Pin the toolchain to the same Arch Linux Archive snapshot as .github/workflows/ci.yml, instead
# of archlinux:latest's rolling mirror — see that workflow's comment for why (and release-plan M7
# notes: this ALSO turned out to be the actual cause of the "qml-css-engine:text font-metric
# drift" this script used to wave through as a known limitation. It wasn't fonts: it was an
# unpinned Arch pulling Qt 6.12.0 — a minor version ahead of the pinned 6.11.2 that this project's
# text-layout port is verified against — not a missing/wrong font at all).
ARCHIVE_DATE="${ARCHIVE_DATE:-2026/09/15}"
echo "== clean-room-check: pinning pacman to the Arch Linux Archive ($ARCHIVE_DATE, Qt 6.11.2) =="
pexec "echo 'Server = https://archive.archlinux.org/repos/$ARCHIVE_DATE/\$repo/os/\$arch' > /etc/pacman.d/mirrorlist && pacman -Sy --noconfirm"

# ---- Dependency inventory (release-plan M6 §1) -----------------------------------------------
# Toolchain:        base-devel (gcc/make/pkgconf/...), git, cmake, meson, ninja, nodejs, npm
# Qt 6 (6.11/6.12):  qt6-base (Core/Gui/Widgets/Network/DBus/Test + their private headers),
#                     qt6-declarative (Qml/QmlModels/Quick/QuickTemplates2 + private headers),
#                     qt6-svg (runtime: SVG icon rendering; not a build-time link dependency today)
# SolidTerm:         libvterm (hard dependency once solidterm=enabled/auto and vterm is found);
#                     wayland-protocols (>=1.45) + wayland (optional: background-blur extension —
#                     builds fine without them, blur is just unavailable)
# Fonts (runtime):    noto-fonts (+ noto-fonts-emoji) — the base Arch image ships NO fonts at all;
#                     without any, Qt's text metrics are effectively undefined and the CSS engine's
#                     pixel-oracle text tests fail hard. noto-fonts is what this project's own dev
#                     machine resolves `sans-serif` to (`fc-match sans-serif`) — match it, don't
#                     guess at ttf-dejavu or similar.
# cmake note:         meson's `cmake` module (used for the installed CMake package config, see
#                     meson.build's `cmake.write_basic_package_version_file`) shells out to the
#                     REAL cmake binary even though nothing here is built WITH CMake. Omitting it
#                     fails meson setup with "ERROR: Program 'cmake' not found" — found on the
#                     very first clean-room run of this script.
pexec "pacman -S --noconfirm base-devel git cmake meson ninja nodejs npm qt6-base qt6-declarative qt6-svg libvterm wayland-protocols noto-fonts noto-fonts-emoji"

echo "== clean-room-check: fetching the framework into the container =="
if [ "$MODE" = "local" ]; then
  # Copy the WORKING TREE's files (including uncommitted fixes being validated), not a git clone —
  # a git worktree's `.git` is a pointer file into the main repo's `.git/worktrees/...`, which does
  # not exist inside the container, so `git clone`/`cp -a .git` both fail here.
  tar -C "$SELF_DIR" --exclude=.git --exclude=build --exclude=.scratch -cf "$SCRATCH_DIR/local-src.tar" .
  podman cp "$SCRATCH_DIR/local-src.tar" "$CONTAINER_NAME:/work-app-src.tar"
  pexec "mkdir -p /work/app && tar -C /work/app -xf /work-app-src.tar && rm -f /work-app-src.tar"
  rm -f "$SCRATCH_DIR/local-src.tar"
else
  pexec "mkdir -p /work && git clone --branch '$REPO_BRANCH' --recurse-submodules '$REPO_URL' /work/app"
  pexec "cd /work/app && git log -1 --oneline && git submodule status"
fi

# The in-container steps (npm ci, typecheck, transpiler tests, native build + tests, install, and
# the "new app outside the checkout" smoke test) live in scripts/ci-steps.sh, shared with the
# release-plan M7 GitHub Actions workflow (.github/workflows/ci.yml) so the two never drift.
# qml-css-engine:text (the CssTextLayout-vs-live-QQuickText pixel/metric oracle) is a HARD
# requirement here, same as every other suite — see docs/RELEASE-PLAN.md M7 notes for the fix.
echo "== clean-room-check: delegating to scripts/ci-steps.sh inside the container =="
pexec "cd /work/app && APP_DIR=/work/app PREFIX_DIR=/opt/solidqml SMOKE_DIR=/work/myapp JOBS=$JOBS bash scripts/ci-steps.sh"
podman cp "$CONTAINER_NAME:/work/myapp/shot.png" "$SHOTS_DIR/clean-room-counter.png"

echo "== clean-room-check: PASSED. Screenshot: $SHOTS_DIR/clean-room-counter.png =="
