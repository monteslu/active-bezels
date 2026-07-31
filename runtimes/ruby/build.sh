#!/usr/bin/env bash
# Build the prebuilt Ruby Active Bezel runtime -> runtimes/ruby/main.wasm
# Needs: emcc (emsdk), git, and a host ruby + rake (first mruby build only).
#
# mruby is FETCHED at a pinned tag, not vendored, and cross-built with
# mruby_wasm_config.rb so upstream stays pristine on disk.
set -euo pipefail
cd "$(dirname "$0")"
MRUBY_TAG=3.4.0
EMSDK_BIN="$HOME/code/mine/emsdk/upstream/emscripten"
EMCC="${EMCC:-emcc}"
command -v "$EMCC" >/dev/null || EMCC="$EMSDK_BIN/emcc"
EMAR="${EMAR:-emar}"
command -v "$EMAR" >/dev/null || EMAR="$EMSDK_BIN/emar"

MRUBY_LIB=vendor/mruby/build/emscripten/lib/libmruby.a
if [ ! -f "$MRUBY_LIB" ]; then
  mkdir -p vendor
  [ -d vendor/mruby ] || git clone --depth 1 --branch "$MRUBY_TAG" \
    https://github.com/mruby/mruby.git vendor/mruby
  # mruby's rake shells out to plain `emcc`/`emar`, so the emsdk directory has
  # to be on PATH for the cross-build even when this script was given absolute
  # tool paths.
  CFG="$(pwd)/mruby_wasm_config.rb"
  ( export PATH="$(dirname "$EMCC"):$PATH"
    cd vendor/mruby && rake MRUBY_CONFIG="$CFG" )
fi

# -sSUPPORT_LONGJMP=wasm is REQUIRED: mruby's exception handling is
# setjmp/longjmp and the JS-trampoline default cannot work under a host that
# provides only the ab_host module. (Same lesson as the Lua runtime and the
# wasmcart interpreters.) No LTO: it breaks the wasm-sjlj path.
#
# -Oz over -O2: mruby's VM plus the compiler is far bigger than Lua's, and the
# size target for a runtime every bezel ships is under 1MB.
# -DMRB_INT64 must match mruby_wasm_config.rb EXACTLY. mrb_int is part of the
# ABI of nearly every mruby entry point, so a mismatch is not a warning that
# can be ignored: wasm-ld reports "function signature mismatch" for
# mrb_get_args/mrb_funcall/mrb_int_value and the calls corrupt their arguments
# at runtime. (Found by a build whose every binding read garbage.)
"$EMCC" runtime.c ../common/ab_batteries.c ../common/ab_wasi_stubs.c "$MRUBY_LIB" \
  -Oz -DMRB_INT64 -I vendor/mruby/include -I . -I ../common \
  -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=16777216 -s STACK_SIZE=1048576 \
  -o main.wasm
echo "built main.wasm ($(wc -c < main.wasm) bytes)"
node -e "
const m = new WebAssembly.Module(require('fs').readFileSync('main.wasm'));
const mods = {};
for (const i of WebAssembly.Module.imports(m)) (mods[i.module] ??= []).push(i.name);
console.log('imports by module:', Object.fromEntries(Object.entries(mods).map(([k,v])=>[k,v.length])));
// The host provides ab_host and nothing else, so any other import module is a
// module that will not instantiate -- fail the build, do not ship it.
const stray = Object.keys(mods).filter((k) => k !== 'ab_host');
if (stray.length) {
  for (const k of stray) console.error('STRAY IMPORT MODULE ' + k + ':', mods[k].join(' '));
  process.exit(1);
}
for (const name of ['ab_abi_version','ab_init','ab_tick','ab_event','ab_shutdown'])
  if (!WebAssembly.Module.exports(m).some(e=>e.name===name)) { console.error('MISSING EXPORT '+name); process.exit(1); }
console.log('exports ok');
"
