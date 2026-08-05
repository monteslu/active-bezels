# Active Bezels v1

Active Bezels are optional, ROM-specific WebAssembly companions that own the
host's complete 16:9 picture. A package can place or transform the original
game, render maps and telemetry around it, read live machine regions, and—when
the author chooses—write those regions like a trainer or Game Genie. The core
runs first, the bezel runs second against the same current machine state, and
the host presents the completed frame.

This document is the format and ABI specification. The reference runtime lives
in this repository and is consumed by more than one host: `retroemu` renders
Active Bezels to a window, and `romdev` composites them headlessly for
screenshots, recordings, and state-correlation analysis. Anything in here that
names a specific host is an example of that host's CLI, not part of the
format.

## Running and developing

Hosts expose Active Bezels their own way. With `retroemu`:

```sh
retroemu game.nes --video sdl --active-bezel enhancement.ab
retroemu game.nes --video sdl --active-bezel ./unpacked-bezel --active-bezel-dev
retroemu game.nes --active-bezel enhancement.ab --active-bezel-force
retroemu game.nes --active-bezel enhancement.ab \
  --active-bezel-config '{"show_map":true}'
```

With `romdev`, the same package is loaded alongside the ROM and composited
headlessly, so an agent can capture the composite, the raw core framebuffer, and
the guest's command stream for the same frame.

Developer mode watches an unpacked directory or archive and replaces the guest
at a frame boundary. A failed reload leaves the previous working guest active.

```sh
abtool scaffold my-bezel c
abtool scaffold my-lua-bezel lua
abtool verify my-bezel
abtool pack my-bezel my-bezel.ab
abtool inspect my-bezel.ab
```

`abtool pack` emits a deterministic stored ZIP. `.ab` is deliberately ordinary:
rename it to `.zip` to inspect it.

The C scaffold includes `active_bezel.h`, `abi.json`, readable source, and a
known-good `main.wasm`. Its README gives the freestanding wasm32 compile
command. The Lua scaffold needs no compiler: edit `app/main.lua` and reload.

## Package shape

```text
manifest.json
main.wasm
assets/...
```

The required manifest fields are:

```json
{
  "format": "active-bezel",
  "formatVersion": 1,
  "id": "org.example.my-bezel",
  "name": "My Bezel",
  "version": "1.0.0",
  "entry": "main.wasm",
  "runtime": {
    "abi": "active-bezel-1",
    "renderer": "gpu-command-v1",
    "internalResolution": [640, 360],
    "extensions": []
  },
  "games": [{
    "platform": "nes",
    "sha256": "64 lowercase hex characters"
  }],
  "universal": false,
  "requires": [{ "region": "system_ram", "minSize": 2048 }],
  "settings": []
}
```

Exact SHA-256 matches are authoritative. `compatible` rules may additionally
identify known revisions by platform, total size, and multiple byte signatures.
They are weaker and reported as such. Mismatches never auto-attach; the player
may explicitly force one.

### Universal packages

Not every bezel is about a specific game. A CRT-in-a-room, a scanline filter, a
border, a shader showcase -- these read no game state and work with anything.
Such a package declares:

```json
{ "universal": true, "games": [] }
```

and matches every ROM at level `universal`, with no force needed.

This is a **claim, not an omission**. An empty `games` list means "matches
nothing", which is indistinguishable from a package whose author forgot to
list its ROMs -- so without this flag a deliberately game-agnostic bezel could
only be loaded with force, and the host had to tell the user that a package
built to work everywhere "does not match this ROM".

`universal` is mutually exclusive with `games` and `compatible`: a package
either works with any ROM or it works with specific ones, and a manifest
claiming both is rejected at load. `universal` is also distinct from `forced`
in what it reports -- forced means the user overrode a failed match, universal
means the author said no match was ever required.

| level | meaning |
| --- | --- |
| `exact` | the ROM's SHA-256 is listed in `games` |
| `compatible` | size and byte signatures match a `compatible` rule |
| `universal` | the package declares it works with any ROM |
| `forced` | no match, loaded anyway at the user's request |
| `none` | no match; the package does not attach |

Packages are capped at 128 MiB unpacked, each entry at 64 MiB. Absolute paths,
traversal, backslashes, NUL names, and symlinks are rejected.

## Frame and display contract

The ABI canvas is always 1920×1080 logical units. This is geometry, not a demand
to shade two million CPU pixels. `runtime.internalResolution` selects a reusable
16:9 CPU surface; SDL then performs hardware presentation scaling to the actual
1080p or 4K display. There are no per-frame output allocations.

Each emulation tick is:

1. Apply current input.
2. Run the libretro core.
3. Expose the resulting live memory and core framebuffer.
4. Call `ab_tick(frame)`.
5. Execute the guest's complete composition.
6. Apply the selected picture effect at its declared scope.
7. Publish the same final composite to SDL, screenshots, recording, and remote
   consumers.

The bezel decides where the game goes. If it submits no commands at all,
the host supplies a centered aspect-correct fallback. A guest framebuffer is
treated as a complete picture, not decoration behind a host-owned layout.

### Presentation is host-neutral by contract

More than one host consumes this runtime, so nothing in the presentation
path may assume a particular one. Concretely, the compositor's window
presentation API takes an **opaque native window handle** and **pure
geometry** — never a host's window object, windowing library, or event
loop:

```
compositor.migrateToWindow(nativeHandle)                    -> 0 | 1
compositor.presentWindow(dstX, dstY, dstW, dstH, winW, winH)
```

Any host that can produce a native window handle gets GPU-direct
presentation; a host that cannot, or whose platform the GL backend does
not yet support, gets `0` from `migrateToWindow` and continues on its own
path. **That fallback is load-bearing, not a courtesy.** It is what keeps
hosts working on platforms the current backend has no branch for, so a
present implementation may be platform-specific but must never remove the
"unavailable, carry on" answer.

The same rule covers GL ownership: a host that has already loaded
`native-gles` injects its instance rather than letting this package
resolve its own (see `index.js`). Two copies of the addon in one process
means two GL contexts and silent hangs, so injection is the supported
path for any embedder.

A change that makes presentation depend on a specific host's windowing —
even one that works today for every current consumer — is a breaking
change to this contract.

## ABI

The machine-readable source of truth is `sdk/abi.json`; the C binding is
`sdk/active_bezel.h`.

Required exports:

```c
int32_t ab_abi_version(void);
int32_t ab_init(uint32_t descriptor);
void ab_tick(uint64_t frame);
```

Optional exports are `ab_event`, `ab_shutdown`, and the CPU framebuffer trio.
Lifecycle events cover reset, state load, rewind jump, live configuration,
display change, asset reload, and region relocation.

The host imports include:

- Display geometry, ABI version and current controller state.
- Region enumeration, stable IDs, byte reads/writes, size, flags, and live
  offsets, plus a generation counter after reset/state/rewind relocation.
- Typed boolean/number/string configuration.
- Package asset size/read calls.
- Clear, game placement/fitting, alpha rectangle, triangle, text, scissor, and
  reset.
- Persistent RGBA texture create/draw/destroy handles, including
  `command_draw_texture_rect` to blit a SOURCE SUB-RECTANGLE of a texture.
  This is what makes an atlas usable: without it a texture can only be drawn
  whole, so a tile renderer needs one texture per tile (or one draw command per
  pixel, which exhausts the 16,384-command budget on any busy scene). With it,
  a package bakes one sheet and spends one command per tile.
- `game_width`, `game_height` and `game_pixel(x, y)` to READ the frame being
  composited, returning `0xRRGGBBAA` (0 outside the frame). A package that
  reconstructs world graphics needs this to match the emulator's own colours:
  palette RAM gives an index, and only the core knows the RGB it decodes that
  index to. Cores disagree — NES colour `$22` is `(104,136,252)` by the common
  NTSC table and `(93,150,255)` in fceumm — so a guest that converts through
  its own table draws a visibly different shade next to the live picture.

Colors are packed `0xRRGGBBAA`. Geometry uses logical canvas coordinates.
Nearest sampling is the pixel-art default.

## Memory

`system_ram`, `save_ram`, `video_ram`, and `rtc` retain their libretro IDs.
Patched core regions use the exact stable IDs already used by Romdev: NES
nametables/palette/OAM/CHR, GB VRAM/OAM/IO/HRAM, Genesis CRAM/VSRAM/VDP state,
GBA palette/OAM/IWRAM, and the equivalent regions for every supported classic
core. `cart_source` is an immutable copy of the loaded ROM.

Beyond the flat state snapshots, cores may expose **per-scanline and
per-frame recording regions** that make mid-frame behaviour reconstructible:
per-line register/palette records (`*_reglines`, `pce_vce_pallines`),
dot-stamped write logs (`msx_vram_deltas`, `pce_paldeltas`), resolved line
buffers and placement records (`pce_vdc_linepix`, `pce_vce_xofflines`,
`pce_vce_srclines`), and the core's own snapshot of framebuffer rows the
current frame never re-rendered (`msx_fb_tail`). These are OPTIONAL: the
region table in `src/Regions.js` must list an id for a guest to find it, and
a consumer that cannot find one falls back silently to snapshot-quality
rendering. The platform redraw profiles in the Lua runtime are the reference
consumers; their result flags (`per_line`, `vram_replay`, `retained`) exist
so a test can assert which path actually ran.

When a core exposes its `WebAssembly.Memory`, a guest may import the identical
object as `ab_core.memory`; no serialized game-state object or full-RAM copy is
created. Named accessors remain available for portability and for core regions
that are not slices of that memory. Writes are intentional and immediate. An
author who enables them owns the consequences.

## Picture effects

`pictureEffect` is `none`, `game`, `scene`, or `composite`.

- `game`: CPU filters run on the original core picture before composition.
- `scene`/`composite`: the effect runs over the completed 16:9 output.
- `none`: the bezel requests an unfiltered result.

Existing `.glslp` presets can render either the original game to an offscreen
GPU target before composition (`game`) or the completed Active Bezel scene
after composition (`scene`/`composite`). `none` suppresses the configured
picture effect. CPU filters follow the same ordering. The offscreen shader
result is read back into the authoritative RGBA composition so screenshots,
remote play, overlays and the SDL presenter all observe the same pixels.

## Shader presets (`.glslp`)

`surface_filter` takes a single fragment shader. `surface_preset` takes a
RetroArch **`.glslp` preset**: a chain of passes, each rendering into its own
buffer at its own resolution, with later passes able to sample several earlier
ones. That is what the serious CRT shaders are, and it cannot be flattened into
one shader at any size.

```lua
local tube = ab.surface_create(w, h)
ab.surface_preset(ab.GAME, tube, 'shaders/crt-lottes.glslp')
```

The destination surface acts as the preset's **viewport**, so a `viewport`-scaled
pass fills the thing being rendered into rather than the display behind it. The
same preset therefore serves a full-screen picture or a small on-screen tube.

### Where to get presets

**This package ships no shaders.** Point it at a copy of libretro's GLSL shader
library:

- <https://github.com/libretro/glsl-shaders> — the upstream repository
- an existing RetroArch install already has them, typically at
  `~/.config/retroarch/shaders/shaders_glsl/`, or under
  `/usr/share/libretro/shaders/shaders_glsl/`
- distro packages: `libretro-shaders-glsl` on Debian/Ubuntu

Copy the presets you want into your package. A preset's `shaderN` paths resolve
relative to the preset file, so keep the directory structure it came with -- a
preset that says `../blurs/blur9fast-vertical.glsl` needs that sibling
directory present.

Lookup textures (`textures = "..."`) are decoded by the host; PNG, JPEG, BMP and
TGA all work.

### Which presets work

The renderer targets **GLES 3 / WebGL2**, because that is what runs unchanged in
a browser and on a handheld. Presets are used as published; their source is
never edited.

Measured against the 609 `.glslp` presets shipped by libretro at the time of
writing:

| | count |
| --- | ---: |
| **Run** | **491** (81%) |
| Will not compile on GLES 3 | 116 |
| `#reference` presets (unsupported form) | 1 |
| Reference a `.glsl` absent from the distribution | 1 |
| **Total** | **609** |

Of the 377 multi-pass presets, 309 run.

By category, where the count is meaningful:

| category | total | run |
| --- | ---: | ---: |
| handheld | 150 | 150 |
| crt | 75 | 61 |
| presets | 82 | 52 |
| borders | 35 | 25 |
| misc | 25 | 21 |
| ntsc | 20 | 15 |
| xbr | 19 | 15 |
| interpolation | 19 | 15 |
| procedural | 12 | 12 |
| cel | 15 | 2 |

**What fails, and why it is not fixable here.** The 116 use desktop-OpenGL
constructs that GLES 3 rejects: `##` token pasting (24), `fwidth` and other
derivative builtins needing an extension pragma (18), implicit int/float
conversion (9), and non-constant global initializers. Each would need a change
to the shader's own source. This package translates the `#version` header and
the `texture2D`/`attribute`/`varying` spellings -- what any GLES loader does --
and stops there. Patching upstream shaders would mean maintaining a private
fork of the shader library, which is a worse problem than an unsupported preset.

`crt-royale` is in the failing group. It declares globals initialised from other
globals, which desktop GL accepts and GLES 3 does not. RetroArch runs it on
GLES via the **slang** pipeline (Vulkan GLSL compiled through SPIRV-Cross), not
through these GLSL files; that pipeline is out of scope here.

If a preset fails, `surface_preset` returns 0 and the compositor records the
compiler's message -- it never renders a partial chain, because a half-applied
CRT preset looks like it worked.

### Licensing

Shaders are **not** redistributed with this package, deliberately. The libretro
GLSL repository has no repository-level licence and the terms are per file: of
the 491 that run, 194 are GPL-2.0/3.0, 226 carry no licence grant at all (189
with no header, 37 with a bare copyright line), and only 71 are MIT or public
domain. Bundling that mix inside an MIT package is not something this project
can do cleanly, so it does not try.

Loading a preset at runtime from a copy the user already has keeps the licensing
question where it belongs -- between the user and the shader author -- and has
the side benefit that users get the whole library and its updates rather than a
frozen subset. If you redistribute a package containing presets you copied in,
their licences travel with them and are yours to honour.

## Scripted bezels

Most bezels are scripts, not C. See [The prebuilt runtimes](#the-prebuilt-runtimes)
below for the four languages, the shared API, and the authoring loop.

There is also an older path: `runtime.language: lua54-wasmcart` embeds the
wasmcart-lua engine, whose framebuffer becomes the background composition with
the game placed over it. It predates the prebuilt runtimes and cannot read
machine regions. New work should use `runtimes/lua/` instead, which speaks the
ab ABI directly. `npx abtool scaffold my-bezel lua` starts one.

## Reference packages

- `diagnostic`: package/lifecycle/composition smoke test. Requires only
  `system_ram` and composes a full scene, so it exercises the whole pipeline
  without asserting anything about a specific game's RAM map. `abtool init`
  scaffolds from it.
- `lua-starter`: reusable Lua authoring proof.

Both are deliberately generic. They declare no ROM compatibility, and the
matcher treats that as matching *nothing* rather than everything — a package
that silently accepted any ROM would compose a map keyed to some other game's
RAM layout. Load them against a ROM with `force` to see the plumbing run.

Game-specific packages are the point of the format, but they belong beside the
ROM they were authored against, not in this repository: a profile is a claim
about one exact ROM revision's memory, and shipping one here would invite it
being force-loaded against something it says nothing true about.

## Failure behavior

Invalid or mismatched packages fail before the game loop. A guest trap disables
only the bezel and immediately returns ordinary game video. Romdeck retains a
trusted disable operation outside guest control. Hot reload is transactional.
Normal sessions that do not attach a bezel do not instantiate this subsystem
and retain the pre-existing fast paths.

## The prebuilt runtimes

A bezel does not have to be compiled. The package ships four complete guest
runtimes, each a wasm that embeds a scripting language and exposes the entire
host ABI to it:

| Runtime | Language | Size | Script | Entry point | Docs |
|---|---|---|---|---|---|
| `runtimes/lua/` | Lua 5.4.7 | 337 KB | `main.lua` | global `ab` table | [Lua bezels](../runtimes/lua/README.md) |
| `runtimes/python/` | MicroPython 1.24.1 | 237 KB | `main.py` | global `ab` module | [Python bezels](../runtimes/python/README.md) |
| `runtimes/js/` | QuickJS 0.15.1 | 532 KB | `main.js` | global `ab` object | [JavaScript bezels](../runtimes/js/README.md) |
| `runtimes/ruby/` | mruby 3.4.0 | 574 KB | `main.rb` | `AB` module | [Ruby bezels](../runtimes/ruby/README.md) |

All four expose the same 65 functions with the same names and semantics; each
README above lists them by area and covers only where that language differs.
[Overview of all four](../runtimes/README.md).

Authoring a bezel is copying one directory:

```
my-bezel/                       <- cp -r runtimes/<lang>/start/ my-bezel/
  manifest.json      entry: "main.wasm"
  main.wasm          the runtime
  main.py            the scaffold, then edited
  assets/            fonts, images, whatever the script reads (add your own)
```

Every runtime ships a commented scaffold script beside its wasm. The scaffold
is a working bezel: game on the left, panel on the right, and one numbered
example of each capability (2D shapes, the live game, TrueType and bitmap
text, a live memory readout, transforms, a decoded PNG, a per-vertex mesh, and
a gated GLSL effect). Copy it, delete what you do not need.

All four runtimes also carry the platform redraw profiles (`nes`, `gb`,
`md`, `snes`, `msx`, `pce`; Ruby spells them `NES`..`PCE`); see
[runtimes/lua/README.md](../runtimes/lua/README.md#platform-redraw-profiles).

### The script contract

Three functions, only one required. The names are the same in all four
languages:

```
init()        optional -- once, after the script loads
tick(frame)   REQUIRED -- once per emulated frame; draw the whole scene
event(kind)   optional -- host lifecycle events (see the EVENT table)
```

### The API

One surface, four bindings. Names and semantics match across languages; only
syntax and the container types differ.

**Drawing** `clear, draw_game, draw_game_fit, fill_rect, triangle, text,
scissor, scissor_reset, mesh`
**Transforms** `push_transform, pop_transform, reset_transform, translate,
scale, rotate`
**Textures and images** `texture_create, texture_destroy, draw_texture,
draw_texture_rect, image, image_data`
**Text** `font, draw_text, measure, font_metrics`
**Effects** `effect_set, effect_clear`
**The machine** `region, region_find_id, region_size, region_flags,
region_offset, region_generation, region_count, read_u8, write_u8, read,
read_u16, read_u24, read_u32, game_width, game_height, game_pixel`
**Host** `logical_width, logical_height, physical_width, physical_height,
elapsed_ms, delta_ms, input, log, asset, config_bool, config_number,
config_string, rgb`
**Constants** `EVENT, FIT, SAMPLE, DEVICE, BTN`

Language-shaped differences, all of them deliberate:

| | Lua | Python | JavaScript | Ruby |
|---|---|---|---|---|
| image returns | table | dict | object | Hash |
| mesh vertex | table | dict or tuple | object | Hash |
| `read` returns | string | bytes | Uint8Array | String |
| constants | `ab.BTN.START` | `ab.BTN['START']` | `ab.BTN.START` | `AB::BTN[:START]` |
| missing region | `nil` | `None` | `null` | `nil` |
| redraw profiles | `nes`..`pce` | `nes`..`pce` | `nes`..`pce` | `NES`..`PCE` |
| profile config error | `nil, reason` | raises | throws | raises |
| `sprite_bounds` | 4 values | tuple | `{x0,y0,x1,y1}` | Array |

TTF drawing is `draw_text` in every runtime. It is deliberately not `print`:
Ruby would silently reach `Kernel#print` from a bare `print(...)` in a class
body, and in Python shadowing the builtin is worse than a different name. One
name that is safe everywhere beats a name that needs a per-language footnote.

Two runtimes add one convenience each on top of the shared surface:
`ab.loadasset(name)` in Lua compiles an asset as a Lua chunk (modules and data
without a filesystem), and `ab.asset_text(name)` in JavaScript returns an asset
as a string rather than a `Uint8Array`. Both are additions, not alternative
spellings of a shared call. Everything else is identical, and a script that
sticks to the table above ports between all four languages by changing syntax
alone.

### Batteries

Every runtime links the same C services from `runtimes/common/`, so a PNG
decodes identically and text rasterises identically no matter the language:

- **Images** (`stb_image`): PNG, JPG, GIF, BMP, decoded straight to a texture.
- **TrueType** (`stb_truetype`): one WHITE glyph atlas per (font, size), drawn
  as a textured mesh whose vertex colour carries the tint. A colour costs
  nothing extra and a frame of text is a couple of draw calls, not hundreds.
- **Multi-byte region reads**, little or big endian.
- **Colour packing** into the `0xRRGGBBAA` every command wants.

### Shader effects

`effect_set(source)` takes a GLSL ES 3.00 fragment shader and runs it over the
finished scene. Single-pass RetroArch shaders port almost verbatim: rename
`Texture` to `u_texture`, `vTexCoord` to `v_uv`, `FragColor` to `out_color`,
add the `#version 300 es` header. Gate on `v_uv` to treat only part of the
picture -- a bezel typically wants the CRT or LCD look on the game rect while
its own panels stay modern. Uniforms available: `u_texture` (the scene),
`u_resolution`, `u_time` (seconds).

Effects need the GPU backend. On the CPU reference compositor `effect_set`
returns false and the scene renders unfiltered, so a bezel should treat it as
an enhancement rather than a requirement.

### Iteration

The runtime re-reads its script when the host reloads assets
(`ActiveBezelRuntime.reloadAssets()`, which re-opens the package from disk and
then fires `ASSETS_RELOADED`). romdev loads an unpacked directory directly, so
the loop is: edit the script, reload, look. No compile, no repack until you
ship.

Load or runtime errors never kill the session. The runtime logs the message,
draws it on an on-screen panel with the failing line, and keeps ticking so the
next reload can fix it.

### Building a runtime from source

Consumers never need to: the wasms are committed. To rebuild one, run its
`build.sh` with emsdk on PATH. Each build fetches its engine at a pinned
version, applies whatever patches that engine needs to live without a
filesystem, and then asserts the artifact imports **only** `ab_host` (no
`env`, no WASI) and exports the five ABI entry points. A build that would
produce an unloadable guest fails at build time instead of in someone's
emulator.
