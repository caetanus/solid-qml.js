#!/usr/bin/env bash
# Release-plan M6 ("install story from a clean machine") / M7 (CI) check.
#
# Spins up a FRESH container, installs ONLY the packages in the dependency list below, clones the
# public repo over HTTPS with submodules, builds + tests + installs the framework, then authors
# and runs a NEW app (tests/clean-room/hello-app) OUTSIDE the checkout against the installed
# artifacts, and grabs a screenshot offscreen as proof it rendered.
#
# Two PROVIDERS (what installs the toolchain), orthogonal to the two MODES (where the source
# comes from):
#   - (default)     Arch Linux (archive.archlinux.org, pacman) — the LOCAL dev-machine
#                    verification path documented in docs/getting-started.md. Not run in CI.
#   - --manylinux   quay.io/pypa/manylinux_2_34_x86_64 (AlmaLinux 9, glibc 2.34) via dnf +
#                    aqtinstall (Qt 6.11.2 official binaries) + a pinned Node tarball — the EXACT
#                    same provider steps as .github/workflows/ci.yml, so this is also how to
#                    reproduce a CI failure locally. See that workflow's header comment for why
#                    this image was chosen.
#
# Usage:
#   scripts/clean-room-check.sh                       # Arch, clone the given branch over HTTPS
#   scripts/clean-room-check.sh --local                # Arch, copy THIS worktree in instead
#   scripts/clean-room-check.sh --manylinux             # manylinux_2_34, clone over HTTPS
#   scripts/clean-room-check.sh --manylinux --local     # manylinux_2_34, copy THIS worktree in
#   REPO_BRANCH=widgets scripts/clean-room-check.sh
#
# Requires: podman. Does not touch anything outside its own container + $SCRATCH_DIR (defaults to
# a dir under the repo's .scratch/, never /tmp — see docs/getting-started.md "Clean machine" and
# the owner's RAM-ceiling / tmpfs rules). The manylinux provider caches its downloads (Qt, Node,
# the meson/ninja/aqtinstall venv) under $SCRATCH_DIR/manylinux-cache so repeated local runs don't
# re-download ~1.5 GB of Qt every time; delete that directory to force a fully fresh download.
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/caetanus/solid-qml.js.git}"
REPO_BRANCH="${REPO_BRANCH:-widgets}"
CONTAINER_NAME="${CONTAINER_NAME:-solidqml-clean-room}"
MEM_LIMIT="${MEM_LIMIT:-8g}"
JOBS="${JOBS:-4}"
MODE="clone"
PROVIDER="arch"
for arg in "$@"; do
  case "$arg" in
    --local) MODE="local" ;;
    --manylinux) PROVIDER="manylinux" ;;
    *) echo "clean-room-check.sh: unknown argument '$arg' (known: --local, --manylinux)" >&2; exit 2 ;;
  esac
done

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH_DIR="${SCRATCH_DIR:-$SELF_DIR/.scratch}"
SHOTS_DIR="$SCRATCH_DIR/clean-room-shots"
CACHE_DIR="$SCRATCH_DIR/manylinux-cache"
mkdir -p "$SCRATCH_DIR/tmp" "$SHOTS_DIR" "$CACHE_DIR"
export TMPDIR="$SCRATCH_DIR/tmp"   # never /tmp (tmpfs = RAM) on the host side

if [ "$PROVIDER" = "manylinux" ]; then
  IMAGE="${IMAGE:-quay.io/pypa/manylinux_2_34_x86_64:latest}"
else
  IMAGE="${IMAGE:-docker.io/library/archlinux:latest}"
fi

cleanup() {
  podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== clean-room-check: pulling $IMAGE =="
podman pull "$IMAGE"

echo "== clean-room-check: starting container (--memory $MEM_LIMIT, provider=$PROVIDER) =="
cleanup
if [ "$PROVIDER" = "manylinux" ]; then
  podman run -d --name "$CONTAINER_NAME" --memory "$MEM_LIMIT" -v "$CACHE_DIR:/cache:Z" "$IMAGE" sleep infinity >/dev/null
else
  podman run -d --name "$CONTAINER_NAME" --memory "$MEM_LIMIT" "$IMAGE" sleep infinity >/dev/null
fi
pexec() { podman exec "$CONTAINER_NAME" bash -lc "$1"; }

ENV_PREFIX=""

if [ "$PROVIDER" = "manylinux" ]; then
  # ---- manylinux_2_34 (AlmaLinux 9, glibc 2.34) provider — mirrors .github/workflows/ci.yml ----
  QT_VERSION="${QT_VERSION:-6.11.2}"
  QT_ARCH="${QT_ARCH:-linux_gcc_64}"
  QT_MODULES="${QT_MODULES:-qtshadertools}"
  NODE_VERSION="${NODE_VERSION:-26.8.2}"
  NODE_SHA256="${NODE_SHA256:-40e1d3225c1c9ae9a2671c98ecb9857e4d5555026394f348645676798840d5c5}"
  MESON_VERSION="${MESON_VERSION:-1.12.0}"
  NINJA_VERSION="${NINJA_VERSION:-1.13.0}"
  AQTINSTALL_VERSION="${AQTINSTALL_VERSION:-3.3.0}"

  # ---- Dependency inventory (release-plan M6 §1, manylinux provider) -------------------------
  # Same rationale as the Arch branch below, dnf package names instead of pacman's. ccache: meson
  # auto-wraps the compiler with it when found on PATH. google-noto-sans-fonts: the sans-serif
  # family the CSS engine's text-layout pixel-oracle test is verified against (see
  # docs/getting-started.md Troubleshooting). libvterm(-devel): SolidTerm. wayland-protocols-devel:
  # manylinux_2_34/EPEL ships 1.37, below this repo's >= 1.45 need for SolidTerm's background-blur
  # extension — the build configures cleanly without it, blur is just unavailable (same situation
  # as a bare Arch image missing it).
  echo "== clean-room-check: installing manylinux system dependencies (dnf) =="
  pexec "dnf -y -q install mesa-libGL-devel mesa-libEGL-devel libxkbcommon-devel libxkbcommon-x11-devel wayland-devel wayland-protocols-devel dbus-devel fontconfig-devel freetype-devel harfbuzz-devel libvterm-devel google-noto-sans-fonts google-noto-emoji-color-fonts ccache git which xz tar patch"

  echo "== clean-room-check: Node $NODE_VERSION (official tarball, checksum-pinned, cached under /cache) =="
  pexec "test -x /cache/node-$NODE_VERSION/bin/node || { curl -fsSL -o /tmp-node.tar.xz https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz && echo '$NODE_SHA256  /tmp-node.tar.xz' | sha256sum -c - && mkdir -p /cache/node-$NODE_VERSION && tar -xJf /tmp-node.tar.xz -C /cache/node-$NODE_VERSION --strip-components=1 && rm -f /tmp-node.tar.xz; }"

  echo "== clean-room-check: Python toolchain (meson/ninja/aqtinstall, pinned, cached under /cache) =="
  pexec "test -x /cache/pyci/bin/aqt || { python3.12 -m venv /cache/pyci && /cache/pyci/bin/pip install --quiet 'meson==$MESON_VERSION' 'ninja==$NINJA_VERSION' 'aqtinstall==$AQTINSTALL_VERSION'; }"

  echo "== clean-room-check: Qt $QT_VERSION ($QT_ARCH, modules: $QT_MODULES) via aqtinstall, cached under /cache =="
  pexec "test -d /cache/Qt/$QT_VERSION/gcc_64 || /cache/pyci/bin/aqt install-qt linux desktop $QT_VERSION $QT_ARCH -m $QT_MODULES --outputdir /cache/Qt"

  pexec "/cache/Qt/$QT_VERSION/gcc_64/bin/qmake6 -query QT_VERSION; /cache/node-$NODE_VERSION/bin/node --version; /cache/pyci/bin/meson --version; gcc --version | head -1"

  QT_PREFIX="/cache/Qt/$QT_VERSION/gcc_64"
  # aqtinstall's Qt is self-contained (not registered with ldconfig like a distro package) — the
  # BUILD tree gets an rpath from meson's qt6 dependency, but the INSTALLED solid-qml-loader
  # (step_smoke, a plain `meson install --destdir` copy) needs Qt's lib dir on LD_LIBRARY_PATH to
  # find libQt6Core.so.6 etc. at runtime. Same fix as .github/workflows/ci.yml.
  ENV_PREFIX="PATH=/cache/pyci/bin:/cache/node-$NODE_VERSION/bin:$QT_PREFIX/bin:\$PATH PKG_CONFIG_PATH=$QT_PREFIX/lib/pkgconfig CMAKE_PREFIX_PATH=$QT_PREFIX LD_LIBRARY_PATH=$QT_PREFIX/lib"
else
  # ---- Arch Linux Archive provider (release-plan M6 §1 / docs/getting-started.md, mirrored) ---
  # Pinned to a fixed archive.archlinux.org snapshot date rather than archlinux:latest's rolling
  # mirror, for the SAME reason the manylinux provider exists at all: an unpinned rolling mirror
  # silently drifted ahead of this project's pinned Qt 6.11.2 once already (release-plan M7 notes —
  # it was mistaken for a font-metric bug at the time). This provider stays as the supported LOCAL
  # dev-machine verification path; CI itself now runs the manylinux provider instead.
  #
  # Toolchain:        base-devel (gcc/make/pkgconf/...), git, cmake, meson, ninja, nodejs, npm
  # Qt 6 (6.11/6.12):  qt6-base (Core/Gui/Widgets/Network/DBus/Test + their private headers),
  #                     qt6-declarative (Qml/QmlModels/Quick/QuickTemplates2 + private headers),
  #                     qt6-svg (runtime: SVG icon rendering; not a build-time link dependency today)
  # SolidTerm:         libvterm (hard dependency once solidterm=enabled/auto and vterm is found);
  #                     wayland-protocols (>=1.45) + wayland (optional: background-blur extension —
  #                     builds fine without them, blur is just unavailable)
  # Fonts (runtime):    noto-fonts (+ noto-fonts-emoji) — the base Arch image ships NO fonts at all;
  #                     without any, Qt's text metrics are effectively undefined and the CSS engine's
  #                     pixel-oracle text tests fail hard.
  # cmake note:         meson's `cmake` module (used for the installed CMake package config, see
  #                     meson.build's `cmake.write_basic_package_version_file`) shells out to the
  #                     REAL cmake binary even though nothing here is built WITH CMake. Omitting it
  #                     fails meson setup with "ERROR: Program 'cmake' not found".
  ARCHIVE_DATE="${ARCHIVE_DATE:-2026/09/15}"
  echo "== clean-room-check: pinning pacman to the Arch Linux Archive ($ARCHIVE_DATE, Qt 6.11.2) =="
  pexec "echo 'Server = https://archive.archlinux.org/repos/$ARCHIVE_DATE/\$repo/os/\$arch' > /etc/pacman.d/mirrorlist && pacman -Sy --noconfirm"
  pexec "pacman -S --noconfirm base-devel git cmake meson ninja nodejs npm qt6-base qt6-declarative qt6-svg libvterm wayland-protocols noto-fonts noto-fonts-emoji"
fi

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
# `bash scripts/ci-steps.sh` (not a bare `scripts/ci-steps.sh`): same belt-and-suspenders as the
# workflow, independent of the script's git executable bit.
echo "== clean-room-check: delegating to scripts/ci-steps.sh inside the container (provider=$PROVIDER) =="
pexec "cd /work/app && env $ENV_PREFIX APP_DIR=/work/app PREFIX_DIR=/opt/solidqml SMOKE_DIR=/work/myapp JOBS=$JOBS bash scripts/ci-steps.sh"
podman cp "$CONTAINER_NAME:/work/myapp/shot.png" "$SHOTS_DIR/clean-room-counter.png"

echo "== clean-room-check: PASSED (provider=$PROVIDER). Screenshot: $SHOTS_DIR/clean-room-counter.png =="
