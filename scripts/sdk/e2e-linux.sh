#!/usr/bin/env bash
# SDK phase-1 end-to-end test (Linux): the developer flow on a CLEAN machine that has only Node —
# node:26-bookworm-slim (glibc 2.36, no Qt, no compiler, no fonts, no GL, no procps).
#
# The packages come from `npm pack` tarballs (scripts/sdk/pack.mjs; the runtime payload must be
# assembled first — scripts/sdk/build-runtime-linux.sh), mounted read-only at /pack; everything
# else (solid-js, vite, …) comes from the public registry. Nothing is published. With tarballs
# instead of a registry, the two registry-only commands are run in their tarball form:
#   npm create solid-qml@latest my-app   →  npx --package=/pack/create-solid-qml-<v>.tgz create-solid-qml my-app
#   npm install                          →  npm install -D /pack/*.tgz   (then the rest from npm)
# and then, exactly as a user would:
#   npm run dev                          (Vite: page + transformed entry served)
#   npx solid-qml doctor
#   npx solid-qml dev                    (offscreen: a native frame grabbed; then hot reload of an edit)
#   npx solid-qml build --target linux   (AppImage + tar.gz)
#   ./<App>-x86_64.AppImage              (no FUSE: APPIMAGE_EXTRACT_AND_RUN=1; clicks +1, grabs) and
#   the tarball's ./AppRun               — in a fresh container
#
# Lean and sequential (owner RAM rules): ONE container at a time, each phase a fresh
# `podman run --rm` sharing only the work volume (.scratch/e2e/work); a phase starts only when host
# RAM < 77% with no compiler running, and is killed if RAM reaches 79%.
# Evidence (transcript + screenshots) lands in docs/evidence/sdk-linux/.
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRATCH="$SELF_DIR/.scratch/e2e"
EVIDENCE="$SELF_DIR/docs/evidence/sdk-linux"
NODE_IMAGE="${NODE_IMAGE:-docker.io/library/node:26-bookworm-slim}"
NAME="solidqml-e2e"
START_GATE="${START_GATE:-77}"
ABORT_AT="${ABORT_AT:-79}"
mkdir -p "$SCRATCH/tmp" "$EVIDENCE"
export TMPDIR="$SCRATCH/tmp"
TRANSCRIPT="$EVIDENCE/transcript.txt"
: > "$TRANSCRIPT"
log() { echo "$*" | tee -a "$TRANSCRIPT"; }
ram_pct() { awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}END{printf "%d", (t-a)*100/t}' /proc/meminfo; }

cleanup() { podman rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# phase <title> <memory> <bash script run in /work>
phase() {
  local title="$1" mem="$2" script="${3#$'\n'}"
  while [ "$(ram_pct)" -ge "$START_GATE" ] || [ "$(pgrep -c cc1plus || true)" -gt 0 ]; do
    echo "   (waiting: RAM $(ram_pct)%, cc1plus $(pgrep -c cc1plus || true))"; sleep 15
  done
  log ""
  log "-- $title (fresh $NODE_IMAGE, --memory $mem, host RAM $(ram_pct)%) --"
  log "\$ ${script//$'\n'/$'\n'\$ }"
  cleanup
  podman run --rm --name "$NAME" --memory "$mem" -v "$SCRATCH/work:/work" -v "$SCRATCH/pack:/pack:ro" \
    -v "$SCRATCH/npm-cache:/root/.npm" -w /work \
    -e QT_QPA_PLATFORM=offscreen -e npm_config_update_notifier=false -e npm_config_fund=false \
    -e npm_config_audit=false -e APPIMAGE_EXTRACT_AND_RUN=1 \
    "$NODE_IMAGE" bash -c "set -eo pipefail; $script" > "$SCRATCH/phase.log" 2>&1 &
  local pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$(ram_pct)" -ge "$ABORT_AT" ]; then
      log "!! host RAM $(ram_pct)% >= $ABORT_AT% — killing the phase"; podman kill "$NAME" >/dev/null 2>&1 || true
    fi
    sleep 2
  done
  local rc=0; wait "$pid" || rc=$?
  sed 's/\x1b\[[0-9;]*m//g' "$SCRATCH/phase.log" | grep -v '^npm notice' | tee -a "$TRANSCRIPT"
  [ "$rc" = 0 ] || { log "!! phase failed (exit $rc): $title"; exit 1; }
}

log "== e2e: pack (host) =="
rm -rf "$SCRATCH/pack" "$SCRATCH/work"
mkdir -p "$SCRATCH/work" "$SCRATCH/npm-cache"
node "$SELF_DIR/scripts/sdk/pack.mjs" "$SCRATCH/pack" 2>&1 | grep '^sdk:' | tee -a "$TRANSCRIPT"
V="$(node -p "require('$SELF_DIR/packages/solid-qml/package.json').version")"

phase "the clean machine" 1g '
node --version; npm --version; ldd --version | head -1
command -v qmake6 gcc g++ cc || echo "(no qmake6 / gcc / g++ / cc)"
ls /usr/share/fonts 2>/dev/null || echo "(no fonts)"; ls /usr/lib/x86_64-linux-gnu/libGL.so.1 2>/dev/null || echo "(no libGL)"'

phase "npm create solid-qml@latest my-app (tarball form)" 1g "
npx --yes --package=/pack/create-solid-qml-$V.tgz create-solid-qml my-app"

phase "npm install (the SDK tarballs, then the template's dependencies from npm)" 1500m "
cd my-app
npm install --no-audit -D /pack/solid-qml-$V.tgz /pack/create-solid-qml-$V.tgz /pack/solid-qml-runtime-linux-x64-$V.tgz 2>&1 | tail -2
ls node_modules/@solid-qml/ node_modules/.bin | grep -E 'runtime|solid-qml|vite\$'
du -sh node_modules/@solid-qml/runtime-linux-x64"

phase "npm run dev (Vite browser preview)" 1g '
cd my-app
(npm run dev -- --port 5173 --strictPort > vite.log 2>&1 &)
for i in $(seq 1 60); do grep -q "Local:" vite.log 2>/dev/null && break; sleep 1; done
grep -E "VITE|Local:" vite.log
node --input-type=module -e "
  const page = await (await fetch(\"http://localhost:5173/\")).text();
  const main = await (await fetch(\"http://localhost:5173/src/main.tsx\")).text();
  if (!page.includes(\"/src/main.tsx\")) throw new Error(\"index.html not served\");
  if (!main.includes(\"solid-qml\") || main.includes(\"<text\")) throw new Error(\"entry not transformed\");
  console.log(\"vite: / served; src/main.tsx compiled by vite-plugin-solid + solid-qml/vite (\" + main.length + \" bytes, <text> mapped to <span>)\");"'

phase "npx solid-qml doctor" 1g '
cd my-app && npx solid-qml doctor'

phase "npx solid-qml dev (offscreen: one native frame, then hot reload of an edit)" 1g '
cd my-app
npx solid-qml dev --grab dev.png
(npx solid-qml dev > native-dev.log 2>&1 &)
for i in $(seq 1 60); do grep -q "watching" native-dev.log && break; sleep 1; done; sleep 3
sed -i "s/Solid.js, rendered natively with Qt Quick/Hot reloaded!/" src/main.tsx
for i in $(seq 1 30); do grep -q "solid-qml-loader: reloading" native-dev.log && break; sleep 1; done; sleep 2
grep -E "\[solid-qml\]|solid-qml-loader: (reloading|wrote)|reload failed" native-dev.log
grep -q "solid-qml-loader: reloading" native-dev.log && ! grep -q "reload failed" native-dev.log
grep -q "Hot reloaded!" .solid-qml/dev/App.generated.qml && echo "hot reload: OK (edit regenerated + live window reloaded)"
sed -i "s/Hot reloaded!/Solid.js, rendered natively with Qt Quick/" src/main.tsx'
cp "$SCRATCH/work/my-app/dev.png" "$EVIDENCE/dev-offscreen.png"

phase "npx solid-qml build --target linux" 1500m '
cd my-app && npx solid-qml build --target linux && ls -la dist/linux'

phase "run the AppImage and the tarball (fresh container, offscreen, no FUSE)" 1g '
mkdir -p /run-app && cp my-app/dist/linux/* /run-app/ && cd /run-app
APP=$(ls *.AppImage); TGZ=$(ls *.tar.gz)
SQ_GRAB_MS=1200 ./$APP --click 242,220,400 --grab /work/appimage.png
tar -xzf $TGZ && cd ${TGZ%.tar.gz} && ./AppRun --grab /work/tarball.png
ls -la /work/appimage.png /work/tarball.png'
cp "$SCRATCH/work/appimage.png" "$EVIDENCE/appimage-offscreen.png"
cp "$SCRATCH/work/tarball.png" "$EVIDENCE/tarball-offscreen.png"
(cd "$SCRATCH/work/my-app/dist/linux" && du -h ./*) | tee -a "$TRANSCRIPT"

log ""
log "== e2e: PASSED — evidence in docs/evidence/sdk-linux/ =="
