#!/usr/bin/env bash
# Build the @solid-qml/runtime-linux-x64 payload reproducibly: a manylinux_2_34 container (the same
# provider as .github/workflows/ci.yml — AlmaLinux 9, glibc 2.34, GCC 14, aqtinstall Qt 6.11.2)
# compiles ONLY what the runtime ships from this worktree (release, no QML debugger): the
# solid-qml-loader target and what it links (libsolidqml = engine + widgets + shims + the opt-in
# widget modules baked into it) — not the test tree; the full `meson test` suite is CI's job
# (.github/workflows/ci.yml, same image). It then stages the two binaries with their install
# RUNPATHs and scripts/sdk/assemble-runtime-linux.sh collects the runtime into
# packages/runtime-linux-x64/. `--full-tests` builds everything and runs `meson test` offscreen.
#
#   scripts/sdk/build-runtime-linux.sh [--full-tests]
#
# Env: JOBS (default 1), RAM_GATE (default 74), MEM_LIMIT (default 6g), CACHE_DIR (Qt/Node/venv cache, default
# .scratch/manylinux-cache — the same layout scripts/clean-room-check.sh --manylinux uses, so one
# cache serves both; mounted read-only when CACHE_RO=1), IMAGE.
# Never writes outside the worktree (.scratch/ for scratch; TMPDIR is set there too).
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRATCH="$SELF_DIR/.scratch"
CACHE_DIR="${CACHE_DIR:-$SCRATCH/manylinux-cache}"
IMAGE="${IMAGE:-quay.io/pypa/manylinux_2_34_x86_64:latest}"
NAME="${CONTAINER_NAME:-solidqml-sdk-runtime}"
JOBS="${JOBS:-1}"
MEM_LIMIT="${MEM_LIMIT:-6g}"
QT_VERSION="6.11.2"
NODE_VERSION="26.8.2"
NODE_SHA256="40e1d3225c1c9ae9a2671c98ecb9857e4d5555026394f348645676798840d5c5"
FULL_TESTS=""
[ "${1:-}" = "--full-tests" ] && FULL_TESTS=1
# .scratch/runtime-work persists /work (source, build dir, stage) across runs: a rerun (e.g. after an
# assembly fix) is incremental instead of recompiling the runtime.
mkdir -p "$SCRATCH/tmp" "$CACHE_DIR" "$SCRATCH/runtime-work"
export TMPDIR="$SCRATCH/tmp"

COMMIT="$(git -C "$SELF_DIR" rev-parse --short=12 HEAD)$(git -C "$SELF_DIR" diff --quiet HEAD -- . ':!packages' || echo -dirty)"
ENGINE_COMMIT="$(git -C "$SELF_DIR/vendor/qml-css-engine" rev-parse --short=12 HEAD)"

# Host RAM guard (owner rule, 2026-10-12: the desktop's idle baseline is ~72%; start a compile only
# below RAM_GATE (74%) with no other compiler running, one TU at a time; a host guard kills the newest
# cc1plus at 77%, so ninja is retried). Polls instead of failing, so the script can be left running.
ram_pct() { awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}END{printf "%d", (t-a)*100/t}' /proc/meminfo; }
wait_for_ram() {
  while [ "$(ram_pct)" -ge "${RAM_GATE:-74}" ] || [ "$(pgrep -c cc1plus || true)" -gt 0 ]; do
    echo "   (waiting: RAM $(ram_pct)%, cc1plus $(pgrep -c cc1plus || true))"; sleep 30
  done
}

cleanup() { podman rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT
wait_for_ram
cleanup
ro=""; [ "${CACHE_RO:-}" = 1 ] && ro=":ro"
podman run -d --name "$NAME" --memory "$MEM_LIMIT" \
  -v "$CACHE_DIR:/cache$ro" -v "$SELF_DIR/packages/runtime-linux-x64:/out" -v "$SCRATCH/runtime-work:/work" \
  "$IMAGE" sleep infinity >/dev/null
pexec() { podman exec "$NAME" bash -lc "$1"; }

echo "== build-runtime-linux: system packages (dnf) =="
pexec "dnf -y -q install mesa-libGL-devel mesa-libEGL-devel libglvnd-opengl libxkbcommon-devel libxkbcommon-x11-devel \
  wayland-devel wayland-protocols-devel dbus-devel fontconfig-devel freetype-devel harfbuzz-devel \
  openssl-libs xcb-util-cursor xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil \
  google-noto-sans-fonts which xz tar >/dev/null"

echo "== build-runtime-linux: toolchain (cached under /cache) =="
pexec "test -x /cache/node-$NODE_VERSION/bin/node || { curl -fsSL -o /node.tar.xz https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz && echo '$NODE_SHA256  /node.tar.xz' | sha256sum -c - && mkdir -p /cache/node-$NODE_VERSION && tar -xJf /node.tar.xz -C /cache/node-$NODE_VERSION --strip-components=1; }"
pexec "test -x /cache/pyci/bin/aqt || { python3.12 -m venv /cache/pyci && /cache/pyci/bin/pip install --quiet meson==1.12.0 ninja==1.13.0 aqtinstall==3.3.0; }"
pexec "test -d /cache/Qt/$QT_VERSION/gcc_64 || /cache/pyci/bin/aqt install-qt linux desktop $QT_VERSION linux_gcc_64 -m qtshadertools --outputdir /cache/Qt"

echo "== build-runtime-linux: source =="
tar -C "$SELF_DIR" --exclude=./.git --exclude=./build --exclude=./node_modules --exclude=./.scratch \
  --exclude=./packages/runtime-linux-x64/runtime -cf "$SCRATCH/runtime-src.tar" .
podman cp "$SCRATCH/runtime-src.tar" "$NAME:/src.tar"
rm -f "$SCRATCH/runtime-src.tar"
pexec "rm -rf /work/src /work/stage && mkdir -p /work/src && tar -C /work/src -xf /src.tar && rm /src.tar"

QT_PREFIX="/cache/Qt/$QT_VERSION/gcc_64"
ENVS="PATH=/cache/pyci/bin:/cache/node-$NODE_VERSION/bin:$QT_PREFIX/bin:\$PATH PKG_CONFIG_PATH=$QT_PREFIX/lib/pkgconfig CMAKE_PREFIX_PATH=$QT_PREFIX"

[ -n "$FULL_TESTS" ] && pexec "cd /work/src && env $ENVS npm ci --no-audit --no-fund >/dev/null"  # meson's transpiler/c23 tests


echo "== build-runtime-linux: meson (release, -j$JOBS) =="
pexec "cd /work/src && { test -f /work/build/build.ninja || env $ENVS meson setup /work/build --prefix=/usr --libdir=lib \
  --buildtype=release -Dqml_debug=false -Dsolidterm=disabled -Dexamples=disabled -Daot=disabled -Dloader=enabled -Dembed=enabled; }"
TARGETS="solid-qml-loader"; [ -n "$FULL_TESTS" ] && TARGETS=""
# A compiler killed by the host's RAM guard just fails the step; ninja resumes where it stopped.
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  wait_for_ram
  pexec "env $ENVS ninja -C /work/build -j $JOBS $TARGETS" && break
  [ "$attempt" = 10 ] && { echo "build-runtime-linux: ninja failed 10 times" >&2; exit 1; }
  echo "   (ninja failed — retrying after the RAM guard clears)"; sleep 20
done
if [ -n "$FULL_TESTS" ]; then
  echo "== build-runtime-linux: meson test (offscreen) =="
  pexec "cd /work/build && env $ENVS LD_LIBRARY_PATH=$QT_PREFIX/lib QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
    meson test --num-processes 2 --timeout-multiplier 3 --print-errorlogs"
fi
# Stage like `meson install` would (which needs the whole tree built): the two shipped binaries with
# their install RUNPATHs (loader: $ORIGIN/../lib, as meson.build's install_rpath; lib: $ORIGIN).
pexec "mkdir -p /work/stage/usr/bin /work/stage/usr/lib && cp /work/build/solid-qml-loader /work/stage/usr/bin/ \
  && cp -a /work/build/libsolidqml.so /work/build/libsolidqml.so.0 /work/build/libsolidqml.so.0.3.0 /work/stage/usr/lib/ \
  && patchelf --set-rpath '\$ORIGIN/../lib' /work/stage/usr/bin/solid-qml-loader \
  && patchelf --set-rpath '\$ORIGIN' /work/stage/usr/lib/libsolidqml.so.0.3.0 \
  && readelf -d /work/stage/usr/bin/solid-qml-loader | grep -E 'RUNPATH|RPATH'"
echo "== build-runtime-linux: loader starts (offscreen, --help) =="
pexec "LD_LIBRARY_PATH=$QT_PREFIX/lib QT_QPA_PLATFORM=offscreen /work/stage/usr/bin/solid-qml-loader --help | head -3"

echo "== build-runtime-linux: assemble =="
pexec "cd /work/src && env $ENVS bash scripts/sdk/assemble-runtime-linux.sh --prefix /work/stage/usr --qt $QT_PREFIX \
  --out /out --commit $COMMIT --engine-commit $ENGINE_COMMIT"
pexec "chown -R $(id -u):$(id -g) /out/runtime /out/runtime.json /out/NOTICE.md" 2>/dev/null || true
echo "== build-runtime-linux: done — $SELF_DIR/packages/runtime-linux-x64 =="
