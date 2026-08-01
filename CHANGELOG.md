# Changelog

## 0.4.1

`abtool scaffold` worked for `lua` and `c` and failed for `js`, `python` and
`ruby` -- a leftover `language must be c or lua` check rejected them before
reaching the scaffold itself. 0.4.0 shipped with three of five targets broken.

Scaffolds now come from `runtimes/<lang>/start/` for every language: the
runtime, a commented source file that is already a working bezel, and a
manifest named after the destination directory. `runtimes/c/start/` is new
(source, header, build.sh). `examples/` is gone -- three packages that
duplicated or contradicted what `runtimes/` already ships, two of which did
not load.

## 0.4.0

### Multi-pass shader presets

`surface_preset(source, destination, "preset.glslp")` runs a RetroArch `.glslp`
preset -- a chain of passes, each rendering into its own buffer at its own
resolution, with later passes able to sample several earlier ones. This is what
the serious CRT shaders are; `surface_filter` takes a single fragment shader and
cannot express them at any size.

The destination surface acts as the preset's viewport, so the same preset serves
a full-screen picture or a small on-screen tube.

**No shaders are bundled.** Point at
[libretro/glsl-shaders](https://github.com/libretro/glsl-shaders) or an existing
RetroArch install. See
[docs/ACTIVE_BEZELS.md](docs/ACTIVE_BEZELS.md#shader-presets-glslp) for where to
get presets, which ones work, and why licensing keeps them out of the package.

Measured against the 609 presets libretro ships: **491 run (81%)**, including
309 of 377 multi-pass presets, 150/150 handheld and 61/75 crt. The remaining 116
use desktop-OpenGL constructs GLES 3 rejects (`##` token pasting, `fwidth`,
non-constant global initializers). Presets are used exactly as published --
their source is never edited -- so those stay unsupported. `crt-royale` is one
of them.

### Host-side image decoding

Lookup textures (`textures = "..."` in a preset) are now decoded by the host,
which took preset coverage from 328 to 491. `src/imgdec.wasm` is the same
`stb_image` already vendored in `runtimes/common` and linked into the four
language runtimes, built standalone -- one image implementation in the tree, no
native dependency. PNG, JPEG, BMP and TGA.

### Offscreen surfaces

`surface_create`, `surface_target`, `surface_end` and `surface_filter` give a
guest real render targets: allocate one, draw into it, filter it with its own
shader, reuse it as a texture, keep it across frames. Filtering into a surface
runs the shader flat at the source's own scale, so a CRT shader behaves as
written and any geometry happens once, afterwards.

### Tilt, skew and perspective

`skew(x, y)` and `transform2d(a..f)` shear the transform stack. `quad(corners,
handle, rgba)` maps a texture onto four arbitrary corners with a
perspective-correct divisor, so a receding plane reads as depth rather than a
PS1-style warp. Both compositors interpolate `u/w`, `v/w` and `1/w`; the GPU
path premultiplies `gl_Position`.

### Fixes

- `surface_preset` is bound in all four scripting runtimes, not just the C SDK
  (`ab_host` imports 54 -> 55). It was previously unreachable from Lua, JS,
  Python and Ruby, which are the normal authoring paths.
- `reloadAssets()` re-opens the package from disk before firing
  `ASSETS_RELOADED`; hot reload previously served the entries cached at open.
- CI ran the test suite twice and inspected only the second run, which used
  `npm test | tee log || exit 1` -- a pipeline's status is `tee`'s, always 0, so
  a failing suite would have gone green. Now one run under `pipefail`, checking
  exit status and skip count together.
- GPU/CPU texel parity: a UV half-pixel shift plus a small tie-break bias, so
  the two compositors agree exactly.

### Notes

- On the pinned emsdk (4.0.18) all four runtime wasms rebuild byte-identical to
  the committed artifacts, so a drift warning in CI means something real.
- `package.json` exports `./package.json` and `./runtimes/*`.

## 0.3.0

Never published; its contents are folded into 0.4.0 above.
