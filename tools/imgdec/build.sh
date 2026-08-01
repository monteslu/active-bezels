#!/usr/bin/env bash
# Build the host-side image decoder from the stb_image already vendored in
# runtimes/common -- the same decoder the four language runtimes link for
# ab.image(). Rebuild after changing imgdec.c; the wasm is committed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EMCC="${EMCC:-emcc}"
command -v "$EMCC" >/dev/null 2>&1 || EMCC="$HOME/code/mine/emsdk/upstream/emscripten/emcc"
OUT="$HERE/../../src/imgdec.wasm"

"$EMCC" -O2 -DNDEBUG \
  -I"$HERE/../../runtimes/common" \
  -s STANDALONE_WASM=1 -s PURE_WASI=0 -s ALLOW_MEMORY_GROWTH=1 \
  -sSUPPORT_LONGJMP=wasm --no-entry \
  "$HERE/imgdec.c" "$HERE/../../runtimes/common/ab_wasi_stubs.c" \
  -o "$OUT"
echo "built src/imgdec.wasm ($(wc -c < "$OUT") bytes)"
