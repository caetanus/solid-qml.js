#!/bin/sh
# Bake the terminal's glyph shaders to .qsb (embedded via shaders.qrc).
#
# CRITICAL: the VERTEX shader MUST be baked with --batchable. QtQuick's default (batching) renderer
# draws custom-material geometry with the *batchable* vertex variant; without it the pipeline fails
# to build ("No GLSL shader code found") and the glyphs silently don't render. The fragment shader
# does not need it.
set -e
cd "$(dirname "$0")"
QSB="${QSB:-/usr/lib/qt6/bin/qsb}"
"$QSB" --batchable --glsl "100es,120,150" --hlsl 50 --msl 12 -o glyph.vert.qsb glyph.vert
"$QSB"             --glsl "100es,120,150" --hlsl 50 --msl 12 -o glyph.frag.qsb glyph.frag
echo "baked glyph.vert.qsb (batchable) + glyph.frag.qsb"
