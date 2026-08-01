# imgdec

`src/imgdec.wasm` is stb_image compiled to a standalone wasm module, so the
HOST can decode shader lookup textures. Guests already decode their own images
inside their runtime; this is the same decoder, reachable from Node.

## Source

`runtimes/common/stb_image.h` — the copy this repo already vendors and links
into all four language runtimes for `ab.image()`. There is deliberately one
image implementation here, not two.

Formats compiled in: PNG, JPEG, BMP, TGA. PNG covers the lookup textures;
JPEG appears in a few overlay presets.

## Rebuilding

    ./build.sh          # writes ../../src/imgdec.wasm

Needs emsdk. The wasm is committed, so consumers never build it.

Two flags are load-bearing:

- `-sSUPPORT_LONGJMP=wasm` — stb_image's error handling is setjmp/longjmp.
  Without it the module imports `env._emscripten_throw_longjmp` and cannot be
  instantiated with an empty import object.
- linking `runtimes/common/ab_wasi_stubs.c` — otherwise the module imports
  `wasi_snapshot_preview1.fd_write` and friends.

Together these make the module import NOTHING, which is what lets
`src/decodeImage.js` instantiate it with `{}` and no WASI shim.
