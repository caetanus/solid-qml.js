#!/usr/bin/env bash
# Assemble the @solid-qml/runtime-linux-x64 npm payload from a solid-qml install prefix and an
# aqtinstall Qt prefix. Run it ON THE BUILD IMAGE (manylinux_2_34 — scripts/sdk/build-runtime-linux.sh
# does that in podman): it resolves system libraries with that image's ldd/rpm, which is what pins
# the glibc floor (2.34) and keeps the payload portable.
#
#   scripts/sdk/assemble-runtime-linux.sh --prefix <dir with bin/solid-qml-loader + lib/libsolidqml.so*> \
#       --qt <…/Qt/6.11.2/gcc_64> [--out packages/runtime-linux-x64] [--commit <sha>] [--engine-commit <sha>]
#
# Output (<out>/):
#   runtime.json                manifest (versions, glibc floor, build image, commits, contents)
#   NOTICE.md                   licensing + how to relink
#   runtime/solid-qml-run       launcher: LD_LIBRARY_PATH = lib (+ per-soname fallbacks the host lacks)
#   runtime/bin/                solid-qml-loader (RUNPATH $ORIGIN/../lib) + qt.conf (Prefix=..)
#   runtime/lib/                libsolidqml + the Qt libraries the closure needs (+ ICU)
#   runtime/lib/fallback/<so>/  desktop system libraries (GL/EGL dispatch, fontconfig, X11/xcb,
#                               wayland, dbus, glib, ...) — used ONLY when the host lacks that soname
#   runtime/plugins/            Qt plugins (platforms offscreen/xcb/wayland/minimal, wayland client
#                               integrations, xcbgl, imageformats, iconengines, tls, networkinformation,
#                               platforminputcontexts, platformthemes)
#   runtime/qml/                Qt QML modules (QtQuick incl. Templates/Shapes/Effects/Window, QtQml,
#                               Qt.labs, QtCore, QtNetwork, QML)
#   runtime/fonts/              fallback font (Noto Sans) + fonts.conf, for hosts with no fonts at all
#   runtime/licenses/           license texts (MIT, LGPL/GPL-3.0, Qt SBOMs, per-package system licenses)
set -euo pipefail

PREFIX="" QT="" OUT="" COMMIT="${SOLIDQML_COMMIT:-unknown}" ENGINE_COMMIT="${ENGINE_COMMIT:-unknown}"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --qt) QT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --commit) COMMIT="$2"; shift 2 ;;
    --engine-commit) ENGINE_COMMIT="$2"; shift 2 ;;
    *) echo "assemble-runtime-linux: unknown argument $1" >&2; exit 2 ;;
  esac
done
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-$SELF_DIR/packages/runtime-linux-x64}"
[ -x "$PREFIX/bin/solid-qml-loader" ] || { echo "no $PREFIX/bin/solid-qml-loader" >&2; exit 1; }
[ -f "$QT/lib/libQt6Core.so.6" ] || { echo "no Qt at $QT" >&2; exit 1; }
QT="$(cd "$QT" && pwd)"

R="$OUT/runtime"
rm -rf "$R" "$OUT/runtime.json" "$OUT/NOTICE.md"
mkdir -p "$R/bin" "$R/lib/fallback" "$R/plugins" "$R/qml" "$R/fonts" "$R/licenses/system" "$R/licenses/qt-sbom"

# ---- 1. solid-qml itself ----------------------------------------------------------------------
cp -a "$PREFIX/bin/solid-qml-loader" "$R/bin/"
cp -a "$PREFIX"/lib/libsolidqml.so* "$R/lib/"
strip --strip-unneeded "$R/bin/solid-qml-loader" "$R"/lib/libsolidqml.so.*.*.* 2>/dev/null || true
cat > "$R/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Libraries = lib
Plugins = plugins
QmlImports = qml
EOF

# ---- 2. Qt plugins + QML modules (selected), copied with the Qt prefix layout so the RUNPATHs Qt
# ships ($ORIGIN/../../lib etc.) keep resolving.
for d in platforms wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration \
         xcbglintegrations imageformats iconengines tls networkinformation \
         platforminputcontexts platformthemes; do
  [ -d "$QT/plugins/$d" ] && cp -a "$QT/plugins/$d" "$R/plugins/"
done
# Platforms: desktop (xcb, wayland), headless (offscreen, minimal). Not the device ones (eglfs,
# linuxfb, vnc, vkkhrdisplay — and so no egldeviceintegrations).
rm -f "$R"/plugins/platforms/lib{qeglfs,qlinuxfb,qvnc,qvkkhrdisplay,qminimalegl}.so
for m in QtQuick QtQml Qt QtCore QtNetwork QML; do
  [ -d "$QT/qml/$m" ] && cp -a "$QT/qml/$m" "$R/qml/"
done
# Only the QML modules the runtime and its widgets import (QtQuick + Templates/Shapes/Effects/Window,
# Layouts for foreign QML, Qt.labs.platform for native menus/dialogs and a few small labs models).
# Not QtQuick.Controls/Dialogs/Particles/VectorImage — the runtime never uses them (its widgets are
# QtQuick.Templates + the CSS engine) and they pull ~25 MB of Controls style libraries.
for d in "$R"/qml/QtQuick/*/; do
  case "$(basename "$d")" in Effects|Layouts|Shapes|Templates|Window) ;; *) rm -rf "$d" ;; esac
done
for d in "$R"/qml/Qt/labs/*/; do
  case "$(basename "$d")" in platform|qmlmodels|settings|folderlistmodel|animation) ;; *) rm -rf "$d" ;; esac
done
rm -rf "$R"/qml/Qt/test
# Design-time / tooling bits Qt ships inside qml/ that an app never loads.
find "$R/qml" \( -name '*.qmltypes' -o -name 'designer' -o -name '*.metainfo' \) -prune -exec rm -rf {} + 2>/dev/null || true
# Plugins whose own dependencies the build image cannot resolve are integrations with desktop stacks
# we do not ship (e.g. the GTK3 platform theme → libgtk-3/libatk): Qt skips a plugin that fails to
# load, so drop them rather than half-ship them.
while IFS= read -r -d '' f; do
  if LD_LIBRARY_PATH="$QT/lib" ldd "$f" 2>/dev/null | grep -q "not found"; then
    echo "assemble-runtime-linux: dropping ${f#"$R"/} (needs: $(LD_LIBRARY_PATH="$QT/lib" ldd "$f" | awk '/not found/ {print $1}' | tr '\n' ' '))"
    rm -f "$f"
  fi
done < <(find "$R/plugins" "$R/qml" -type f -name '*.so' -print0)

# ---- 3. Library closure: ldd every ELF we ship (with Qt's lib dir first) and sort each dependency:
#   in Qt's prefix      → runtime/lib (with its SONAME symlinks)
#   glibc / libstdc++   → never bundled (the host provides them; the glibc floor is the contract)
#   anything else       → runtime/lib/fallback/<soname>/ (used only when the host lacks it)
is_host_core() {
  case "$1" in
    linux-vdso.so*|ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|\
    libutil.so*|libanl.so*|libnsl.so*|libBrokenLocale.so*|libmvec.so*|libstdc++.so*|libgcc_s.so*) return 0 ;;
  esac
  return 1
}
# Loaded with dlopen (invisible to ldd) — bundled as fallbacks too: TLS backend → OpenSSL 3;
# Qt's xcb/wayland EGL paths → libEGL/libGL vendor-neutral dispatch (glvnd).
DLOPENED=(libssl.so.3 libcrypto.so.3 libEGL.so.1 libGL.so.1 libGLX.so.0 libOpenGL.so.0 libGLdispatch.so.0 libwayland-egl.so.1 libxkbcommon-x11.so.0)

declare -A seen=()
mapfile -t elfs < <(find "$R/bin" "$R/lib" "$R/plugins" "$R/qml" -type f \( -name '*.so*' -o -perm -u+x \) -print)
deps_file="$(mktemp)"
for f in "${elfs[@]}"; do
  [ "$(od -An -N4 -c "$f" 2>/dev/null | tr -d ' ')" = '177ELF' ] || continue
  LD_LIBRARY_PATH="$QT/lib:$R/lib" ldd "$f" 2>/dev/null | awk '/=>/ { print $1, $3 }' >> "$deps_file" || true
done
for so in "${DLOPENED[@]}"; do
  p="$(ldconfig -p | awk -v s="$so" '$1==s && /x86-64/ { print $NF; exit }')"
  [ -n "$p" ] && echo "$so $p" >> "$deps_file"
done
# Close the fallback set over its own dependencies (a dlopened lib's deps aren't in any ldd above).
pending="$(sort -u "$deps_file")"
fallbacks=()
qtlibs=()
while [ -n "$pending" ]; do
  next=""
  while read -r soname path; do
    [ -n "$soname" ] || continue
    [ -z "${seen[$soname]:-}" ] || continue
    seen[$soname]=1
    if is_host_core "$soname"; then continue; fi
    if [ "$path" = "not" ] || [ ! -e "$path" ]; then echo "assemble-runtime-linux: UNRESOLVED $soname" >&2; exit 1; fi
    case "$(readlink -f "$path")" in
      "$QT"/lib/*|"$R"/lib/*)
        if [ ! -e "$R/lib/$soname" ]; then
          base="${soname%%.so*}"
          cp -a "$QT/lib/$base".so.* "$R/lib/" 2>/dev/null || cp -aL "$path" "$R/lib/$soname"
          qtlibs+=("$soname")
          next+="$(LD_LIBRARY_PATH="$QT/lib" ldd "$path" | awk '/=>/ { print $1, $3 }')"$'\n'
        fi ;;
      *)
        mkdir -p "$R/lib/fallback/$soname"
        cp -L "$path" "$R/lib/fallback/$soname/$soname"
        fallbacks+=("$soname")
        next+="$(ldd "$path" | awk '/=>/ { print $1, $3 }')"$'\n'
        # License of the package that owns it (rpm-based build image).
        pkg="$(rpm -qf --qf '%{NAME}\n' "$(readlink -f "$path")" 2>/dev/null | head -1 || true)"
        if [ -n "$pkg" ] && [ ! -d "$R/licenses/system/$pkg" ] && [ -d "/usr/share/licenses/$pkg" ]; then
          cp -a "/usr/share/licenses/$pkg" "$R/licenses/system/$pkg"
        fi
        if [ -n "$pkg" ]; then echo "$soname $pkg $(rpm -q --qf '%{VERSION}-%{RELEASE}' "$pkg")" >> "$R/licenses/system/PACKAGES.txt"; fi ;;
    esac
  done <<< "$pending"
  pending="$(printf '%s' "$next" | sort -u)"
done
rm -f "$deps_file"
sort -u -o "$R/licenses/system/PACKAGES.txt" "$R/licenses/system/PACKAGES.txt" 2>/dev/null || true
# Strip debug info from the fallback system libraries (Qt's own libs come stripped).
find "$R/lib/fallback" -type f -name '*.so*' -exec strip --strip-unneeded {} + 2>/dev/null || true

# npm tarballs cannot carry symlinks (npm pack drops them), so flatten the SONAME chains: each library
# is stored once, under its SONAME (libQt6Core.so.6 — what the dynamic loader looks up); the
# development links (libX.so) and the fully versioned names (libX.so.6.11.2) go away.
find "$R" -type l -name '*.so' -delete
while IFS= read -r -d '' link; do
  target="$(readlink -f "$link")"
  rm "$link"
  mv "$target" "$link"
done < <(find "$R" -type l -print0)
[ -z "$(find "$R" -type l)" ] || { echo "assemble-runtime-linux: symlinks left in the payload" >&2; exit 1; }

# ---- 4. Fallback font (for hosts without any fonts — bare containers) ------------------------
for f in NotoSans-Regular.ttf NotoSans-Bold.ttf; do
  src="$(find /usr/share/fonts -name "$f" | head -1)"
  [ -n "$src" ] && cp "$src" "$R/fonts/"
done
ls "$R"/fonts/*.ttf >/dev/null 2>&1 || { echo "assemble-runtime-linux: no Noto Sans on the build image (dnf install google-noto-sans-fonts)" >&2; exit 1; }
nfpkg="$(rpm -qf --qf '%{NAME}\n' "$(find /usr/share/fonts -name NotoSans-Regular.ttf | head -1)" | head -1)"
[ -d "/usr/share/licenses/$nfpkg" ] && cp -a "/usr/share/licenses/$nfpkg" "$R/licenses/system/$nfpkg"
cat > "$R/fonts/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- solid-qml fallback font configuration: used by solid-qml-run only on hosts with no fonts. -->
<fontconfig>
  <dir prefix="relative">.</dir>
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <dir prefix="xdg">fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>sans-serif</family><prefer><family>Noto Sans</family></prefer></alias>
  <alias><family>serif</family><prefer><family>Noto Sans</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>Noto Sans</family></prefer></alias>
</fontconfig>
EOF

# ---- 5. Launcher -----------------------------------------------------------------------------
cp "$SELF_DIR/scripts/sdk/solid-qml-run.sh" "$R/solid-qml-run"
chmod 755 "$R/solid-qml-run"

# ---- 6. Licenses -----------------------------------------------------------------------------
cp "$SELF_DIR/LICENSE" "$R/licenses/solid-qml-MIT.txt"
cp "$SELF_DIR/vendor/qml-css-engine/LICENSE" "$R/licenses/qml-css-engine-LICENSE.txt"
cp "$SELF_DIR/vendor/qml-css-engine/NOTICE" "$R/licenses/qml-css-engine-NOTICE.txt"
cp "$SELF_DIR/vendor/qml-css-engine/LICENSES/LGPL-3.0-only.txt" "$R/licenses/LGPL-3.0-only.txt"
cp "$SELF_DIR/vendor/qml-css-engine/LICENSES/GPL-3.0-only.txt" "$R/licenses/GPL-3.0-only.txt"
for m in qtbase qtdeclarative qtsvg qtwayland; do
  [ -f "$QT/sbom/$m-6.11.2.cdx.json" ] && cp "$QT/sbom/$m-6.11.2.cdx.json" "$R/licenses/qt-sbom/"
done

# ---- 7. Manifest + NOTICE ---------------------------------------------------------------------
QT_VERSION="$("$QT/bin/qmake6" -query QT_VERSION 2>/dev/null || basename "$(dirname "$QT")")"
GLIBC_FLOOR="$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
# The real floor is the highest GLIBC_x.y symbol version any shipped binary references.
GLIBC_NEEDED="$(find "$R" -type f -name '*.so*' -o -type f -perm -u+x | { xargs -r objdump -T 2>/dev/null || true; } | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t. -k2,2n -u | tail -1 | sed 's/GLIBC_//')"
GLIBCXX_NEEDED="$(find "$R" -type f -name '*.so*' -o -type f -perm -u+x | { xargs -r objdump -T 2>/dev/null || true; } | grep -oE 'GLIBCXX_[0-9]+\.[0-9]+\.[0-9]+' | sort -t. -k3,3n -u | tail -1 | sed 's/GLIBCXX_//')"
OS_NAME="$(. /etc/os-release && echo "$PRETTY_NAME")"
GCC="$(gcc --version | head -1)"
VERSION="$(node -p "require('$OUT/package.json').version" 2>/dev/null || grep -m1 '"version"' "$OUT/package.json" | cut -d'"' -f4)"
SIZE_KB="$(du -sk "$R" | cut -f1)"
json_list() { local first=1; printf '['; for x in "$@"; do [ $first = 1 ] || printf ', '; printf '"%s"' "$x"; first=0; done; printf ']'; }
mapfile -t fb_sorted < <(printf '%s\n' "${fallbacks[@]}" | sort -u)
mapfile -t qt_sorted < <(ls "$R/lib" | grep -E '\.so\.[0-9]+$' | sort)
cat > "$OUT/runtime.json" <<EOF
{
  "name": "@solid-qml/runtime-linux-x64",
  "version": "$VERSION",
  "platform": "linux",
  "arch": "x64",
  "solidQml": "$VERSION",
  "commit": "$COMMIT",
  "engineCommit": "$ENGINE_COMMIT",
  "qt": "$QT_VERSION",
  "glibc": "$GLIBC_NEEDED",
  "glibcxx": "$GLIBCXX_NEEDED",
  "buildImageGlibc": "$GLIBC_FLOOR",
  "builtOn": "manylinux_2_34_x86_64 ($OS_NAME)",
  "compiler": "$GCC",
  "root": "runtime",
  "launcher": "runtime/solid-qml-run",
  "loader": "runtime/bin/solid-qml-loader",
  "sizeKB": $SIZE_KB,
  "libs": $(json_list "${qt_sorted[@]}"),
  "fallbackLibs": $(json_list "${fb_sorted[@]}")
}
EOF
cat > "$OUT/NOTICE.md" <<EOF
# @solid-qml/runtime-linux-x64 — licensing

This package redistributes, unmodified and as shared libraries:

| Component | License | Where |
| --- | --- | --- |
| solid-qml (loader, libsolidqml, widgets, shims) | MIT | \`runtime/bin\`, \`runtime/lib/libsolidqml.so*\` — \`licenses/solid-qml-MIT.txt\` |
| qml-css-engine (statically linked into libsolidqml) | LGPL-3.0-only | \`licenses/qml-css-engine-*\`, \`licenses/LGPL-3.0-only.txt\`, \`licenses/GPL-3.0-only.txt\` |
| Qt $QT_VERSION (official binaries via aqtinstall) | LGPL-3.0-only (Qt's open-source option) | \`runtime/lib/libQt6*\`, \`runtime/plugins\`, \`runtime/qml\` — SBOMs in \`licenses/qt-sbom/\` |
| System libraries (fallbacks, AlmaLinux 9 / manylinux_2_34 packages) | per package | \`runtime/lib/fallback\` — \`licenses/system/<package>/\`, list in \`licenses/system/PACKAGES.txt\` |
| Noto Sans (fallback font) | OFL-1.1 | \`runtime/fonts\` — \`licenses/system/$nfpkg/\` |

## Replacing / relinking (LGPL-3.0 §4)

Qt and the solid-qml libraries are dynamically linked shared objects: replace any \`libQt6*.so.6\`
in \`runtime/lib\` with an ABI-compatible build (Qt $QT_VERSION — the engine uses Qt private API,
so the same Qt minor is required) and the loader uses it. qml-css-engine is linked statically into
\`libsolidqml.so.0\`; its complete source and the build scripts to relink \`libsolidqml.so.0\` are at
https://github.com/caetanus/solid-qml.js (commit $COMMIT, engine submodule $ENGINE_COMMIT):
\`meson setup build && ninja -C build\` against your Qt produces a drop-in \`libsolidqml.so.0\`.
Qt's sources: https://download.qt.io/official_releases/qt/ (version $QT_VERSION).
EOF

echo "assemble-runtime-linux: $R — $(du -sh "$R" | cut -f1), ${#qt_sorted[@]} libs in lib/, ${#fb_sorted[@]} fallbacks, glibc >= $GLIBC_NEEDED"
