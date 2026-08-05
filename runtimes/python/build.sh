#!/usr/bin/env bash
# Build the prebuilt MicroPython Active Bezel runtime -> runtimes/python/main.wasm
# Needs: emcc (emsdk), git, make, python3.
#
# MicroPython is FETCHED at a pinned tag, not vendored. The `embed` port turns
# the interpreter into a set of plain .c files with no OS assumptions, which is
# exactly the shape a bezel needs: no filesystem, no syscalls, no WASI.
set -euo pipefail
cd "$(dirname "$0")"
MPY_VERSION=v1.24.1
EMCC="${EMCC:-emcc}"
command -v "$EMCC" >/dev/null || EMCC="$HOME/code/mine/emsdk/upstream/emscripten/emcc"

if [ ! -d vendor/micropython ]; then
  mkdir -p vendor
  git clone --depth 1 --branch "$MPY_VERSION" \
    https://github.com/micropython/micropython.git vendor/micropython
fi

# Generate the embed sources against OUR mpconfigport.h (float, string
# formatting, no VFS). Regenerated when the config changes.
if [ ! -d micropython_embed ] || [ mpconfigport.h -nt micropython_embed ]; then
  make -f micropython_embed.mk MICROPY_EMBED_DIR=micropython_embed >/dev/null
fi

# port/mphalport.c is EXCLUDED: its stdout hook is printf, which pulls WASI
# fd_* imports in. runtime.c defines the ab_log version instead.
SRC=$(find micropython_embed -name '*.c' ! -name 'mphalport.c' | sort)

# -sSUPPORT_LONGJMP=wasm is REQUIRED: MicroPython's exception handling is
# setjmp/longjmp (nlr), and the default JS-trampoline form cannot work under a
# host that provides only the ab_host module. No LTO: it breaks wasm-sjlj.
"$EMCC" runtime.c ab_profiles_py.c \
  ../common/ab_batteries.c ../common/ab_render.c ../common/ab_profiles.c \
  ../common/ab_nes.c ../common/ab_gb.c ../common/ab_md.c ../common/ab_snes.c \
  ../common/ab_msx.c ../common/ab_pce.c \
  ../common/ab_wasi_stubs.c $SRC \
  -Os -DNDEBUG \
  -I . -I micropython_embed -I micropython_embed/port -I ../common \
  -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=16777216 -s STACK_SIZE=1048576 \
  -o main.wasm

echo "built main.wasm ($(wc -c < main.wasm) bytes)"

# The contract: this artifact imports ONLY ab_host and exports the five ABI
# entry points. An `env` or WASI import means the module will not instantiate
# in a host that provides just ab_host, so fail the build here rather than at
# load time in someone's emulator.
node -e "
const m = new WebAssembly.Module(require('fs').readFileSync('main.wasm'));
const mods = {};
for (const i of WebAssembly.Module.imports(m)) (mods[i.module] ??= []).push(i.name);
console.log('imports by module:', Object.fromEntries(Object.entries(mods).map(([k,v])=>[k,v.length])));
const bad = Object.keys(mods).filter(k => k !== 'ab_host');
if (bad.length) {
  for (const k of bad) console.error('UNEXPECTED import module ' + k + ': ' + mods[k].join(' '));
  process.exit(1);
}
for (const name of ['ab_abi_version','ab_init','ab_tick','ab_event','ab_shutdown'])
  if (!WebAssembly.Module.exports(m).some(e=>e.name===name)) { console.error('MISSING EXPORT '+name); process.exit(1); }
console.log('exports ok');
"
