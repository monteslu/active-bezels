# Changelog

## Unreleased

### Changed

- **Each GpuCompositor owns its own GL context.** native-gles is
  multi-context now; the compositor captures the handle `createContext`
  returns and binds every makeCurrent/swap/setSwapInterval/destroy to it.
  The old process-global context meant every consumer in the server (each
  session's compositor, each wasmcart cart) silently shared one context —
  harmless while everything rendered offscreen and read back CPU pixels,
  but the moment GL-direct present bound that shared context to a window,
  every other consumer's rendering presented into that window. Requires
  native-gles >= 0.6.0.
- **`migrateToWindow` attaches instead of rebuilding.** The window surface
  binds to the LIVE context (`attachWindow`), so every texture, FBO,
  surface and compiled program survives untouched and the rebuild path —
  the source of the "works headless, dies in the window" bug class — is
  bypassed entirely. The destroy/recreate dance remains only as a fallback
  for configs without a window bit, and fails non-destructively.
- **Window presents block on vsync.** The GPU blit+swap costs ~0.2ms, and
  unsynced timer-paced swaps land at random phases of the refresh — frames
  shown twice or skipped, microstutter at a nominally perfect frame rate.
  (Interval 0 was only ever right for the software present, whose 13.6ms
  blit had no room to also block.) On macOS this rides native-gles' Metal
  backend; the CGL backend it replaced ignored swap intervals entirely.

## 0.8.0

### Added

- **Three-way layer routing on the NES/GB redraw.** `draw` takes
  `bg_surface`, `solid_surface` and `spr_surface`; one draw -- one frame
  read, one sprite evaluation -- emits the empty backdrop, the solid tiles
  and the sprites onto three surfaces. The split is on the RESOLVED pattern
  index, so it separates level geometry from open sky, which the PPU draws
  as one layer and a post-processing filter therefore cannot tell apart.
  That distinction is the difference between an effect that reads as the
  world changing and one that reads as a wash laid over it.
- **`hide_cell(cx, cy)` / `hide_sprite(slot)`** -- take an 8x8 background
  cell or an OAM slot out of the render entirely. The pixels are never
  emitted, so there is nothing to erase and nothing to paint over. This is
  what a package needs to own a class of pixels (a HUD, a character) without
  screen-scraping: an 8x8 cell contains whatever is BEHIND the entity, so
  copying it out of the finished frame drags the background along and tears
  a hole in the layer underneath.
- **`isolate_sprite(slot)`** -- the inverse: emit ONLY the marked slots,
  through the normal CHR + OAM + palette path, onto whatever surface the
  draw targets. Gives one entity its own layer, properly rendered.
  Identify entities by SLOT rather than colour: a game is free to draw the
  player and several unrelated entities from one sprite palette while a
  fourth uses another, so a colour test exempts the wrong set.
- **`surface_filter` takes an optional mask texture**, bound as `u_mask`
  and sampled NEAREST, so a shader can key off structure the framebuffer
  cannot express (a per-cell tile-class map, a per-pixel entity map).
- **A readable error panel**, shared by all four runtimes. An embedded
  TrueType face (DejaVu Sans Mono, ASCII-subset, 17 KB) on an opaque
  backing across the top half of the frame, with the game still running
  underneath. The font is compiled into the runtime because the panel exists
  for when the PACKAGE is broken. Replaces a 3x5 bitmap that was close to
  unreadable at 1080p.
- **`ab_last_error` guest export**, surfaced as `runtime.status().scriptError`
  (and `BEZEL_SCRIPT_ERROR` in romdev). Script errors are caught by the
  guest runtime, so the host's tick returns NORMALLY -- the existing `error`
  field stays null and every automated health check passes while the screen
  shows a stack trace. This is the channel tooling needs.
- **Full parameter reference in all four runtime READMEs** -- every call
  with its arguments, generated from one canonical table so the four cannot
  drift apart.

### Fixed

- **`surface_filter` ran before the draws it was meant to filter.** Draw
  commands are queued and executed at frame end, but the filter ran its GL
  immediately -- so it filtered the PREVIOUS frame, or an empty surface on
  frame 1. Now queued and run in command order; compile errors still report
  synchronously as the return value.
- **`surface_filter` into the same surface did nothing.** In-place filtering
  bound one texture as both sampler source and FBO attachment, which GL
  leaves undefined. Now ping-pongs through a cached scratch surface and is
  orientation-neutral: an identity shader no longer flips the picture.
- **Surfaces did not survive a context migration.** `init()` reset the
  surface map, so opening a playtest window (which rebinds GL onto the
  window) invalidated every handle a guest was holding: `surface_target`
  bound nothing and `surface_filter` returned 0, with no error anywhere. A
  layered bezel went dead the moment a window opened while looking correct
  in headless captures.
- **`surface_target` cleared unconditionally**, so a guest could not
  re-enter a surface within a frame -- the second target wiped what the
  first pass drew. Surfaces now clear once per frame, on first target.
- **A surface drawn with `draw_texture` came out upside down** (FBO rows are
  bottom-up; uploaded textures are top-down).
- **The background emit ignored its suppress mask.** `emit_plane` hardcoded
  `draw = 1` for the background path, so a mask handed to
  `ab_nes_emit_background` was accepted and silently discarded.
- **Hide/isolate marks were never consumed.** An isolate list survived the
  draw and suppressed every sprite in every later draw -- enemies and items
  silently stopped rendering. The two lists are now consumed independently:
  clearing both together instead wiped `hide_cell` marks a later draw in the
  same frame still needed, which double-drew suppressed HUD text.
- **`TEXTURE1` was missing from the GL constant table**, so binding a second
  sampler passed `undefined` to `glActiveTexture` and surfaced as an opaque
  guest trap.
- **Python logged one class of error untagged.** `set_error_from_exception`
  filled the error buffer directly and bypassed the tagged log path, so a
  Python script error was the one case invisible to `grep AB-ERROR` even
  though the API reported it.


## 0.7.0

### Added

- **The `pre_frame` hook (ABI 2, opt-in).** A guest may export
  `int32_t ab_pre_frame(uint64_t frame)`; the host calls it before EVERY
  core frame, after physical input is known and before the core polls it.
  `ab_tick` observes the frame the core produced — `ab_pre_frame` shapes
  the frame the core is about to run: region writes land before the game's
  logic consumes them, and the new `input_override(port, device, index, id,
  value)` host import replaces what the core sees for this frame (id 256 =
  the whole joypad mask). Overrides clear before every `pre_frame`, so an
  override is one frame's statement, re-asserted each frame. `input_state`
  keeps reporting the PHYSICAL pad — the game sees the override, the bezel
  sees the truth, so a left/right swap cannot read back its own output.
  `input_override` outside `pre_frame` is refused (logged once): a
  tick-time override would ambiguously target the next frame and then be
  discarded. Frame 0 is included (post-reset, pre-execution RAM — gate on
  the frame number or a RAM signature if you need initialized state); the
  host never steps the core to "warm up". The return value is reserved:
  return 0.
- **`pre_frame(frame)` in all four scripting runtimes** (Lua, Python,
  JavaScript, Ruby — same optional global as `init`/`event`), plus
  `ab.input_override` / `AB.input_override` bindings, and
  `ab_pre_frame_defined()` so hosts skip the per-frame call entirely when
  the script defines no hook. Verified with the cross-runtime parity
  harness: identical composed frames, identical log streams AND identical
  override call sequences across all four languages.
- **Analog input surface.** `input_state` with the ANALOG device (5) reads
  stick positions (index 0/1, id 0=X/1=Y, −32768..32767) and trigger
  pressure (index 2, id 12/13, 0..32767) where the host tracks them.
  Constant tables gained `BTN.L2/R2/L3/R3` and a new `ANALOG` group
  (`LEFT/RIGHT/BUTTON/X/Y`); the C header gained the matching `AB_BTN_*`
  and `AB_ANALOG_*` defines.
- Hosts accept guest ABI versions 1 AND 2. A guest that uses neither new
  surface may keep reporting 1 and loads on old hosts; a guest that imports
  `input_override` cannot instantiate on a pre-ABI-2 host (missing import),
  which is the loud failure the versioning promises. The four shipped
  runtimes now report 2.
- `ActiveBezelRuntime.preFrame(frameNumber)` — the embedder-facing entry
  hosts call before each core frame; `status()` reports
  `preFrame: {defined, calls}` so a session can SEE that a bezel shapes
  the game (pre_frame writes are host-side pokes, invisible to core-side
  write watchpoints).

### Changed

- **Honest region flags.** Snapshot-backed regions (fill-a-buffer getters
  and per-frame capture planes) no longer carry the WRITE flag; a write to
  one used to "succeed" into a staging buffer the core never reads back.
  `region_write_u8` on them now returns 0, so a guest can tell "I changed
  the machine" from "I changed a copy".

## 0.6.0

### Added

- **Platform redraw profiles in all four runtimes.** The `nes`, `gb`, `md`,
  `snes`, `msx` and `pce` globals — previously Lua-only — are now exposed by
  the Python, JavaScript and Ruby runtimes too (Ruby spells them
  `NES`..`PCE`, as constants must be). All orchestration moved from the Lua
  binding into a language-neutral core, `runtimes/common/ab_profiles.{h,c}`,
  linked into every runtime; the per-language files
  (`ab_profiles_lua.c` / `_py.c` / `_js.c` / `_rb.c`) are marshaling only,
  so the four runtimes are pixel-identical by construction. Verified with a
  cross-runtime parity harness: one synthetic core, the identical call
  sequence in all four languages, byte-identical composed frames — and the
  pre-refactor Lua runtime (the corpus-certification build) produces the
  same frame, so the extraction is behaviour-preserving. Spot-checked live
  through the MCP host as well: two real carts per platform per language
  (48 scored runs), every one composite-vs-core 100.000% exact.
  Language-shaped differences (option dicts/objects/Hashes, raise-vs-nil
  error idioms, tuple/object/Array multi-returns) are listed in each
  runtime's README. A pure-C guest can link the same core directly (compile
  `ab_profiles.c` + the renderers into the module and call the `ab_prof_*`
  API); verified the same way, 100.000% exact on real carts at ~15 KB of
  wasm.

- **MSX redraw profile** (`msx` global in the Lua runtime, `ab_msx.{c,h}`):
  pixel-exact reconstruction of the V99x8 modes (SCREEN 0-8, including the
  512-wide SCREEN 6/7 and interleaved SCREEN 7/8 VRAM), TMS and V9938 sprite
  planes with the emulator's one-line sprite-buffer delay, colour-0 backdrop
  and per-register palettes. Verified against a real-cart corpus: every
  renderable cart scores 100.000% composite-vs-core through the bezel path.
- **MSX per-scanline machinery**, consumed with strict validity gates and
  silent-fallback protection:
  - `msx_vdp_reglines` (per-line registers + palette: raster splits),
  - `msx_vram_deltas` (dot-stamped VRAM/register/palette write log with old
    AND new values; undo/redo replay reconstructs what each line rendered
    from, and same-line events split the row at the recorded 8px block),
  - `msx_fb_tail` (the core's own snapshot of rows the frame never
    re-rendered, captured at the end of `retro_run`). Rows past the
    frame-end cut are retained framebuffer memory that no state-only
    renderer can produce; a caller-side prior-composite fallback exists for
    per-frame composers, but live bezel ticks fire per COMPOSE, so the core
    snapshot is the only live-correct source.
  - Sub-line sprite fill points are honoured: V9938 modes latch the next
    line's sprites at block 24, TMS modes at block 33, and write-log events
    stamped at or past the fill point are excluded from the next row's
    sprite evaluation.
- **PCE mid-line palette splits** (`pce_paldeltas`): a dot-stamped VCE
  colour-table write log (old and new values per write). `pce.draw` and the
  scoring harness undo a row's events to reach its line-start table and
  re-apply them at the recorded pixel, closing the last sub-scanline class
  on the PCE corpus (270/270 renderable carts exact).
- Region table entries for `msx_vdp_reglines` (0x1c6), `msx_vram_deltas`
  (0x1c7), `msx_fb_tail` (0x1c8) and `pce_paldeltas` (0x1ab), with comments
  documenting the silent-fallback trap each one guards against.
- `msx.draw` result flags `per_line`, `vram_replay` and `retained`, so a
  bezel or a test can assert which rendering path actually ran.
- The suite now carries 22 must-fail controls
  (`runtimes/common/tests/run.sh`): every per-line/replay/retention/split
  rule fails the suite when disabled.

- **`region_read` host import**: bulk-copy a region span straight into guest
  memory, one host crossing per region instead of one per byte (a C guest
  snapshotting the NES resolved planes made 122,880 `region_read_u8` calls a
  frame; this collapses that to two). Hosts predating the import resolve it
  to a stub returning 0, so callers must keep the `region_read_u8` fallback;
  `ab_region_slurp` in `runtimes/common/ab_render.c` is the canonical shape.
- **Texture ABI**: `texture_filter` (0 nearest, 1 linear, 2 bicubic,
  3 palette-indexed bicubic, +16 REPEAT wrap; the CPU backend stays nearest
  by documented divergence), `texture_palette` (bind a 256x1 RGBA palette to
  an index texture so static planes animate through a 1KB palette upload),
  and `texture_update` (patch a sub-rectangle of a persistent texture in
  place, for streaming tilemaps that would otherwise re-upload whole planes).
- **GL-direct window presentation** on the GPU compositor:
  `migrateToWindow(nativeHandle)` and
  `presentWindow(dstX, dstY, dstW, dstH, winW, winH)`. Host-neutral by
  contract (see `docs/ACTIVE_BEZELS.md`): `migrateToWindow` returning 0 means
  the host keeps its own present path.
- **`setGlModule(mod)`** export: a host injects its already-loaded
  `native-gles` instance so a symlinked copy of this package can never load
  a second native addon (two GL contexts in one process hang silently).
- Snapshot-backed regions (NES CHR / APU / CPU regs) refresh at the top of
  every tick, so a guest reading them per frame sees current data instead of
  the boot-time buffer.

- `docs/HD_TEXT.md`: recipe for replacing a game's 8x8 font tiles
  with real letterforms, one glyph per tile cell. Needs no runtime
  support beyond `ab.draw_text` and a font asset.
- `examples/hd-text/`: worked bezel plus `tools/make_tile_font.py`,
  which bakes the horizontal stretch the technique depends on.

### Changed

- TTF drawing is `draw_text` in every runtime; the `print` alias is gone
  from Lua, JavaScript and Ruby. One name that is safe in all four beats a
  name needing a per-language footnote — Ruby's bare `print(...)` reaches
  `Kernel#print` and Python's shadows the builtin. All call sites in this
  repo were converted.
- `native-gles` moved from a hard dependency to an optional peer dependency
  (plus a devDependency for this repo's own tests). Hosts that present on
  the GPU either inject their instance via `setGlModule` or depend on
  `native-gles` themselves; pure-CPU embedders no longer install a native
  addon at all.

### Documented

- `docs/ACTIVE_BEZELS.md`: presentation is host-neutral **by contract**.
  The window-presentation API takes an opaque native handle and pure
  geometry, and `migrateToWindow` returning `0` (so the host keeps its own
  path) is load-bearing for hosts on platforms the GL backend has no
  branch for. A present implementation may be platform-specific; removing
  the "unavailable, carry on" answer is a breaking change.

## 0.5.0

### Universal packages

A bezel can now declare that it works with **any** ROM:

```json
{ "universal": true, "games": [] }
```

and it matches every ROM at a new level, `universal`, with no force needed.

Not every bezel is about a specific game. A CRT-in-a-room, a scanline filter,
a border, a shader showcase -- these read no game state and work with
anything. Until now the only way to say so was an empty `games` list, which
means "matches nothing" and is indistinguishable from a package whose author
forgot to list its ROMs. So a deliberately game-agnostic bezel could only be
loaded with `force`, and the host had to tell users that a package built to
work everywhere "does not match this ROM".

`universal` is mutually exclusive with `games` and `compatible` -- a package
either works with any ROM or it works with specific ones -- and a manifest
claiming both is rejected when it loads rather than surfacing as a confusing
match later. It is also distinct from `forced`: forced means the user
overrode a failed match, universal means the author said no match was ever
required, and a host can report the difference honestly.

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
