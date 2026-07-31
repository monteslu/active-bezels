#!/usr/bin/env bash
# Build the prebuilt Lua Active Bezel runtime -> runtimes/lua/main.wasm
# Needs: emcc (emsdk), curl, python3.
#
# Lua is FETCHED at a pinned version, not vendored, and patched idempotently
# at build time (patch-lua.py) so upstream stays pristine on disk.
set -euo pipefail
cd "$(dirname "$0")"
LUA_VERSION=5.4.7
EMCC="${EMCC:-emcc}"
command -v "$EMCC" >/dev/null || EMCC="$HOME/code/mine/emsdk/upstream/emscripten/emcc"

if [ ! -f vendor/liblua54.a ]; then
  mkdir -p vendor
  if [ ! -d vendor/lua ]; then
    curl -sL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o vendor/lua.tar.gz
    tar xzf vendor/lua.tar.gz -C vendor
    mv "vendor/lua-${LUA_VERSION}" vendor/lua
    rm vendor/lua.tar.gz
  fi
  # Core + the libraries a bezel can safely have. NO liolib/loslib/loadlib:
  # there is no filesystem and no dynamic loading, by design.
  CORE="lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c \
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c \
        ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c \
        lmathlib.c lstrlib.c ltablib.c lutf8lib.c"
  python3 patch-lua.py vendor/lua/src/lauxlib.c
  python3 patch-lua.py vendor/lua/src/lmathlib.c
  HERE="$(pwd)"
  ( cd vendor/lua/src && \
    "$EMCC" -O2 -c $CORE -DAB_LUA_NOFILES -sSUPPORT_LONGJMP=wasm \
      -include "$HERE/abconf.h" && \
    "$HOME/code/mine/emsdk/upstream/emscripten/emar" rcs ../../liblua54.a *.o && rm -f *.o )
fi

# -sSUPPORT_LONGJMP=wasm is REQUIRED: Lua's error handling is setjmp/longjmp
# and the JS-trampoline default cannot work under a host that provides only
# the ab_host module. (Same lesson as wasmcart interpreters.) No LTO: it
# breaks the wasm-sjlj path.
"$EMCC" runtime.c ../common/ab_batteries.c ../common/ab_wasi_stubs.c vendor/liblua54.a \
  -O2 -I vendor/lua/src -I . -I ../common \
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
if (mods.wasi_snapshot_preview1) console.log('wasi imports:', mods.wasi_snapshot_preview1.join(' '));
for (const name of ['ab_abi_version','ab_init','ab_tick','ab_event','ab_shutdown'])
  if (!WebAssembly.Module.exports(m).some(e=>e.name===name)) { console.error('MISSING EXPORT '+name); process.exit(1); }
console.log('exports ok');
"
