#!/usr/bin/env bash
# Build the prebuilt JavaScript Active Bezel runtime -> runtimes/js/main.wasm
# Needs: emcc (emsdk), and either a vendored QuickJS or git+network.
#
# QuickJS is quickjs-ng, seeded by COPYING the known-good checkout from
# wasmcart-jsgame rather than re-cloning: that tree is already pinned, already
# builds under emcc, and a fresh clone of HEAD is exactly the "nothing was
# actually pinned" trap that bit setup_quickjs.sh. Override with QUICKJS_SRC.
#
# The fallback clone is pinned to the same RELEASE the donor carries. It used
# to clone HEAD, so a machine with the donor and a machine without it (CI)
# compiled different QuickJS source -- 21 KB of unexplained drift in the
# committed wasm, and exactly the trap the paragraph above warns about.
set -euo pipefail
cd "$(dirname "$0")"
EMCC="${EMCC:-emcc}"
command -v "$EMCC" >/dev/null || EMCC="$HOME/code/mine/emsdk/upstream/emscripten/emcc"
QUICKJS_SRC="${QUICKJS_SRC:-vendor/quickjs}"
QUICKJS_DONOR="${QUICKJS_DONOR:-$HOME/code/cliemu/wasmcart-jsgame/vendor/quickjs}"
# quickjs-ng v0.15.1, the version the donor tree carries (QJS_VERSION_* in
# quickjs.h) and that the committed main.wasm was built from.
QUICKJS_COMMIT="${QUICKJS_COMMIT:-fd0a0210b7be00957751871e7e01b8291268fc29}"

if [ ! -f "$QUICKJS_SRC/quickjs.c" ]; then
  mkdir -p vendor
  if [ -f "$QUICKJS_DONOR/quickjs.c" ]; then
    echo "seeding QuickJS from $QUICKJS_DONOR"
    rm -rf "$QUICKJS_SRC"
    cp -r "$QUICKJS_DONOR" "$QUICKJS_SRC"
    rm -rf "$QUICKJS_SRC/.git" "$QUICKJS_SRC/test262" "$QUICKJS_SRC/tests"
  else
    echo "cloning quickjs-ng at $QUICKJS_COMMIT (no donor at $QUICKJS_DONOR)"
    git init -q "$QUICKJS_SRC"
    git -C "$QUICKJS_SRC" remote add origin https://github.com/quickjs-ng/quickjs.git
    git -C "$QUICKJS_SRC" fetch -q --depth 1 origin "$QUICKJS_COMMIT"
    git -C "$QUICKJS_SRC" checkout -q FETCH_HEAD
    rm -rf "$QUICKJS_SRC/.git" "$QUICKJS_SRC/test262" "$QUICKJS_SRC/tests"
  fi
fi

# -Oz on the engine, not -O2: QuickJS is the overwhelming majority of the
# binary and the interpreter loop is not the bottleneck in a bezel that draws
# a few hundred commands a frame.
#
# The -D switches below are all size, and all safe HERE specifically because a
# bezel has no filesystem, no threads and no wall clock:
#   NO_BIGNUM         -- no BigInt/BigFloat tables (region values are <= 32b)
#   NO_ATOMICS        -- single-threaded by construction
#   NO_WORKER_LOOP    -- there is no event loop; nothing pumps jobs
# -ffile-prefix-map keeps the BUILDER'S absolute path out of the shipped wasm:
# assert() and __FILE__ otherwise bake /home/<user>/code/... into every copy.
QJS_CFLAGS="-Oz -DCONFIG_VERSION=\"ab\" \
  -DCONFIG_NO_BIGNUM=1 -DCONFIG_NO_ATOMICS=1 -DCONFIG_NO_WORKER_LOOP=1 \
  -D_GNU_SOURCE -DEMSCRIPTEN -DNDEBUG \
  -ffile-prefix-map=$(cd "$QUICKJS_SRC" && pwd)=quickjs \
  -sSUPPORT_LONGJMP=wasm \
  -Wno-implicit-function-declaration -Wno-sign-compare \
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function"

mkdir -p obj
for unit in quickjs libregexp libunicode cutils dtoa; do
  src="$QUICKJS_SRC/$unit.c"
  # cutils.c was merged away in quickjs-ng >= 0.11 and dtoa.c only exists
  # there; compile whichever the checkout actually has so either engine works.
  [ -f "$src" ] || { rm -f "obj/$unit.o"; continue; }
  [ "obj/$unit.o" -nt "$src" ] && [ "obj/$unit.o" -nt build.sh ] && continue
  echo "cc $unit.c"
  $EMCC $QJS_CFLAGS -c "$src" -o "obj/$unit.o"
done
# Only the objects that exist -- an ORPHANED .o left over from another engine
# would link silently and produce a phantom crash (wasmcart-jsgame scar).
QJS_OBJS=""
for unit in quickjs libregexp libunicode cutils dtoa; do
  [ -f "$QUICKJS_SRC/$unit.c" ] && QJS_OBJS="$QJS_OBJS obj/$unit.o"
done

# -sSUPPORT_LONGJMP=wasm is REQUIRED: QuickJS's exception path is
# setjmp/longjmp and the JS-trampoline default cannot work under a host that
# provides only the ab_host module. (Same lesson as the Lua runtime and every
# wasmcart interpreter.) No LTO: it breaks the wasm-sjlj path.
$EMCC runtime.c ../common/ab_batteries.c ../common/ab_wasi_stubs.c $QJS_OBJS \
  -Oz -I "$QUICKJS_SRC" -I . -I ../common \
  -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=16777216 -s STACK_SIZE=1048576 \
  -ffile-prefix-map="$(pwd)"=js-runtime \
  -o main.wasm
echo "built main.wasm ($(wc -c < main.wasm) bytes)"
node -e "
const m = new WebAssembly.Module(require('fs').readFileSync('main.wasm'));
const mods = {};
for (const i of WebAssembly.Module.imports(m)) (mods[i.module] ??= []).push(i.name);
console.log('imports by module:', Object.fromEntries(Object.entries(mods).map(([k,v])=>[k,v.length])));
// The host provides ab_host and nothing else: any other import module means
// the module fails to instantiate in a real session, so fail the build here.
const extra = Object.keys(mods).filter((k) => k !== 'ab_host');
if (extra.length) {
  for (const k of extra) console.error('UNEXPECTED IMPORT MODULE ' + k + ': ' + mods[k].join(' '));
  process.exit(1);
}
for (const name of ['ab_abi_version','ab_init','ab_tick','ab_event','ab_shutdown'])
  if (!WebAssembly.Module.exports(m).some(e=>e.name===name)) { console.error('MISSING EXPORT '+name); process.exit(1); }
console.log('imports ok (ab_host only), exports ok');
"
