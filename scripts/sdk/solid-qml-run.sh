#!/bin/sh
# solid-qml runtime launcher (ships as runtime/solid-qml-run in @solid-qml/runtime-linux-x64 and in
# every AppImage/tarball built from it). It runs bin/solid-qml-loader with:
#   - runtime/lib first on LD_LIBRARY_PATH (Qt + libsolidqml; the loader's RUNPATH covers its own
#     direct deps, this covers the system libs the Qt plugins pull);
#   - runtime/lib/fallback/<soname> appended ONLY for sonames the host does not have — so a desktop
#     uses its own GL/fontconfig/X11/wayland stack, and a bare machine still starts;
#   - the bundled fallback font configuration when the host has no fonts at all.
# SOLID_QML_LAUNCHER_INFO=1 prints that decision and runs ldd instead (`solid-qml doctor`).
R="$(dirname "$(readlink -f "$0")")"
TAB="$(printf '\t')"
HOSTLIBS="$( (ldconfig -p || /sbin/ldconfig -p || /usr/sbin/ldconfig -p) 2>/dev/null)"

has_host() {
  case "$HOSTLIBS" in *"$TAB$1 (libc6,x86-64"*) return 0 ;; esac
  for d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
    [ -e "$d/$1" ] && return 0
  done
  return 1
}

LDP="$R/lib"
FALLBACK=""
for dir in "$R"/lib/fallback/*/; do
  [ -d "$dir" ] || continue
  so="$(basename "$dir")"
  if ! has_host "$so"; then
    LDP="$LDP:${dir%/}"
    FALLBACK="$FALLBACK $so"
  fi
done

has_fonts() {
  for d in /usr/share/fonts /usr/local/share/fonts "${XDG_DATA_HOME:-$HOME/.local/share}/fonts" "$HOME/.fonts"; do
    [ -n "$(ls -A "$d" 2>/dev/null)" ] && return 0
  done
  return 1
}
NOFONTS=""
if [ -z "${FONTCONFIG_FILE:-}" ] && ! has_fonts; then
  export FONTCONFIG_FILE="$R/fonts/fonts.conf"
  NOFONTS=1
fi

# A host Qt's plugin/import paths would load plugins built against another Qt into this one.
unset QT_PLUGIN_PATH QML2_IMPORT_PATH QML_IMPORT_PATH

if [ -n "${SOLID_QML_LAUNCHER_INFO:-}" ]; then
  for so in $FALLBACK; do echo "fallback $so"; done
  [ -n "$NOFONTS" ] && echo "fonts none"
  for f in "$R/bin/solid-qml-loader" "$R"/plugins/platforms/*.so; do
    echo "ldd $f"
    LD_LIBRARY_PATH="$LDP" ldd "$f" 2>&1
  done
  exit 0
fi

export LD_LIBRARY_PATH="$LDP${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$R/bin/solid-qml-loader" "$@"
