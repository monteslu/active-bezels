#!/usr/bin/env bash
# Build this bezel. Needs clang with a wasm32 target (emsdk's works).
set -euo pipefail
cd "$(dirname "$0")"
# emsdk's clang needs its own bin/ on PATH to find wasm-ld.
EMBIN="${EMBIN:-$HOME/code/mine/emsdk/upstream/bin}"
[ -x "$EMBIN/clang" ] && PATH="$EMBIN:$PATH"
clang --target=wasm32 -O2 -nostdlib -fno-builtin \
  -Wl,--no-entry -Wl,--allow-undefined \
  -Wl,--export=ab_abi_version -Wl,--export=ab_init -Wl,--export=ab_tick \
  -Wl,--export=ab_event -Wl,--export=ab_shutdown \
  main.c -o main.wasm
echo "built main.wasm ($(wc -c < main.wasm) bytes)"
