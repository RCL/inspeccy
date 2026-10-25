#!/bin/bash
# Builds inspeccy-emul for the browser and assembles a folder that can be
# copied onto any web server:
#
#   make_web.sh <out dir>                a player that fetches the .nex
#                                        beside it (?nex=NAME, default
#                                        program.nex) -- one build runs any
#                                        program, so drop a new .nex in and
#                                        the page picks it up
#   make_web.sh <out dir> <program.nex>  the same, with that program packed
#                                        into the page as well, for a
#                                        single self-contained bundle
#
# Needs the Emscripten SDK on PATH (source emsdk_env.sh first).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
OUT=${1:?output directory}; NEX=${2:-}
BUILD=${BUILD_DIR:-$(mktemp -d)}
emcmake cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DINSPECCY_BUILD_EMUL=ON -DINSPECCY_BUILD_VIEWER=OFF -DBUILD_TESTING=OFF \
  -DINSPECCY_TRACE_SUPPORT=OFF -DINSPECCY_EMUL_WASM_NEX="$NEX"
cmake --build "$BUILD" --target inspeccy-emul
mkdir -p "$OUT"
cp "$BUILD"/inspeccy-emul/inspeccy-emul.{html,js,wasm} "$OUT/"
[ -f "$BUILD/inspeccy-emul/inspeccy-emul.data" ] && cp "$BUILD/inspeccy-emul/inspeccy-emul.data" "$OUT/"
cp "$HERE/README.txt" "$OUT/" 2>/dev/null || true
ls -la "$OUT"
[ -n "$NEX" ] || echo "Put the program beside the page as program.nex, or pass ?nex=NAME."
