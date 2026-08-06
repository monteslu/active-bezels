# Lua bezels

`main.wasm` embeds **Lua 5.4.7** and hands a script the entire Active Bezel
API. Ship this wasm as your package's `entry`, put a `main.lua` beside it, and
you have a bezel — no compiler, no build step.

```sh
cp -r node_modules/active-bezel/runtimes/lua/start my-bezel
$EDITOR my-bezel/main.lua
```

`start/` holds exactly what you need and nothing else: the runtime, the
scaffold, and a ready `manifest.json`.

The scaffold is a **working bezel**, not a stub: a commented example of every
capability, which you edit down to what you want.

## Running it

A bezel is not run on its own -- a host loads it alongside a ROM. Two do:

**[retroemu](https://github.com/monteslu/retroemu)** renders to a window:

```sh
retroemu game.nes --video sdl --active-bezel ./my-bezel --active-bezel-dev
```

`--active-bezel-dev` watches the directory, so saving `main.lua` reloads the
bezel in place. That is the whole authoring loop: edit, save, look.

**[romdev](https://github.com/monteslu/romdev)** composites headlessly and is
what you want for scripted iteration -- it can capture the composite, the raw
core framebuffer and the guest's command stream for the same frame:

```js
loadMedia({ platform: 'nes', path: 'game.nes',
            useActiveBezel: true, activeBezelPath: './my-bezel',
            activeBezelForce: true })
```

Both take an unpacked directory, so nothing needs packing until you ship:

```sh
npx abtool verify my-bezel
npx abtool pack   my-bezel my-bezel.ab
```

## The contract

```lua
function init() end          -- optional; once, after the script loads
function pre_frame(frame) end -- optional; BEFORE the core runs `frame`
function tick(frame) end     -- required; draw the whole scene, every frame
function event(kind) end     -- optional; the machine jumped (see ab.EVENT)
```

`pre_frame` (ABI 2) shapes the frame the core is ABOUT to run: region
writes land before the game's logic consumes them, and
`ab.input_override(port, device, index, id, value)` replaces what the core
is polled with (`ab.BTN.MASK` writes the whole joypad word). Overrides
clear before every call — re-assert each frame — and `ab.input` keeps
reporting the PHYSICAL pad, so a remap can never read back its own output.
`input_override` outside `pre_frame` is refused (logged once). Frame 0
sees post-reset, pre-execution RAM — gate on the frame number or a RAM
signature if you need initialized state. Defining the function is the only
opt-in; without it nothing is called and nothing costs.

Two field lessons for `pre_frame` transforms (learned on real games):
they run once per EMULATED frame, but game state does not always advance
with the frame (lag frames skip rebuilds; capture timing jitters) — so a
transform must be **stateless per frame or idempotent**, never a bare
toggle. And on cores that store RAM as native 16-bit words (Genesis), the
bezel's region view is RAW — logical byte `n` lives at offset `n ~ 1`.

A bezel owns the whole **1920×1080** picture, including where the game goes.
Coordinates are on that grid whatever the real output resolution is.

A broken script is a visible, fixable state, never a dead session: load and
runtime errors paint an on-screen panel naming the line, and the runtime
re-reads `main.lua` on ASSETS_RELOADED.

## A first bezel

```lua
local W, H = 1920, 1080
local GAME_W = math.floor(H * 4 / 3)   -- 1440; the leftover IS the bezel
local ram

function init()
  ram = ab.region('system_ram')
end

function tick(frame)
  ab.clear(ab.rgb(14, 16, 26))
  ab.draw_game(0, 0, GAME_W, H, ab.SAMPLE.NEAREST)

  -- read the machine and show something the game never displays
  local hp = ab.read_u8(ram, 0x0E)
  ab.fill_rect(GAME_W + 32, 80, 400 * (hp / 255), 24, ab.rgb(120, 200, 255))
  ab.text(string.format('HP %d', hp), GAME_W + 32, 130, 32, ab.rgb(235, 238, 250))
end
```

## The `ab` table

Colours are packed `0xRRGGBBAA`; `ab.rgb(r, g, b[, a])` builds one.

| area | functions |
| --- | --- |
| **draw** | `clear` `fill_rect` `triangle` `mesh` `text` `scissor` `scissor_reset` |
| **the game** | `draw_game` `draw_game_fit` `game_width` `game_height` `game_pixel` |
| **transform** | `push_transform` `pop_transform` `reset_transform` `translate` `scale` `rotate` `skew` `transform2d` |
| **textures** | `texture_create` `texture_destroy` `draw_texture` `draw_texture_rect` `image` `image_data` |
| **text** | `font` `draw_text` `measure` `font_metrics` |
| **perspective** | `quad` |
| **surfaces** | `surface_create` `surface_target` `surface_end` `surface_filter` `surface_preset` |
| **shaders** | `effect_set` `effect_clear` |
| **memory** | `region` `region_find_id` `region_size` `region_flags` `region_offset` `region_count` `region_generation` `read_u8` `read_u16` `read_u24` `read_u32` `read` `write_u8` |
| **host** | `logical_width` `logical_height` `physical_width` `physical_height` `elapsed_ms` `delta_ms` `input` `input_override` `log` `asset` `loadasset` `config_bool` `config_number` `config_string` `rgb` |

Constant tables, so scripts never hard-code ABI numbers: `ab.EVENT` `ab.FIT`
`ab.SAMPLE` `ab.DEVICE` `ab.BTN` (now through `L2/R2/L3/R3`) `ab.ANALOG`.
`ab.GAME` is the live-frame handle — pass it anywhere a texture is wanted.

Analog reads, where the host tracks them:
`ab.input(port, ab.DEVICE.ANALOG, ab.ANALOG.LEFT, ab.ANALOG.X)` → stick
axis −32768..32767; `ab.input(port, ab.DEVICE.ANALOG, ab.ANALOG.BUTTON,
ab.BTN.L2)` → trigger pressure 0..32767.

### Lua shapes

```lua
local img = ab.image('assets/logo.png')     -- { texture=, width=, height= }
ab.draw_texture(img.texture, 0, 0, img.width, img.height)

ab.mesh({                                    -- array of { x=, y=, rgba=, u=, v= }
  { x = 0,  y = 0,  rgba = ab.rgb(255, 0, 0) },
  { x = 64, y = 0,  rgba = ab.rgb(0, 255, 0) },
  { x = 0,  y = 64, rgba = ab.rgb(0, 0, 255) },
})

-- four arbitrary corners, perspective-correct: a receding plane reads as
-- depth rather than a PS1-style warp
ab.quad({ {x=100,y=0}, {x=300,y=20}, {x=320,y=200}, {x=80,y=180} }, img.texture)
```

`ab.read(id, off, len)` returns a Lua string; index it with `string.byte`.
`ab.region(name)` returns `nil` when the platform has no such region, so guard
it.

TrueType text is `ab.draw_text` (same name in all four runtimes). `ab.text`
is the built-in 3×5 bitmap font — fine for debug, not for UI.

## Platform redraw profiles

Every runtime exposes one global per supported platform: `nes`, `gb`, `md`,
`snes`, `msx`, `pce` (Ruby spells them `NES`..`PCE`). Each is a **redraw
profile**: instead of sampling the core's framebuffer with `ab.draw_game`,
it re-renders the game picture from the machine's live memory regions (VRAM,
registers, palettes, per-scanline records) through the same draw-command
batch every other `ab.*` call uses. The renderers and all orchestration are
one shared C core (`../common/ab_profiles.c`) linked into all four runtimes;
the per-language bindings are marshaling only, so the same call produces the
same pixels in every language. This section is the canonical API reference;
the other runtimes' READMEs list only their language-shaped differences. A
pure-C guest can skip the interpreter entirely and call the same core
through `../common/ab_profiles.h` -- see the main README's "With a compiler"
section.

Why bother, when `draw_game` already shows the picture? Because a redraw is a
*scene*, not a bitmap: the bezel can substitute sprite art, restyle layers,
extend the playfield, or draw the world at a different scale, while everything
it does not touch stays **pixel-identical to the emulator** on real games.
That exactness is measured, not assumed: each profile is scored composite vs
core, pixel for pixel, across a real-cart corpus, and the test suite carries a
must-fail control for every rule the renderers encode
([`common/tests/run.sh`](../common/tests/run.sh), 22 controls).

```lua
function tick(frame)
  ab.clear(ab.rgb(10, 10, 14))
  local r = msx.bind()          -- once; finds the regions, allocates buffers
  local res = msx.draw({ x = 240, y = 60, scale = 4 })
  -- res.quads, res.mode, res.width (272 or 544 on MSX),
  -- res.per_line     -- true when per-scanline records drove the frame
  -- res.vram_replay  -- true when the VRAM write log replayed mid-frame writes
  -- res.retained     -- true when fossil rows came from the core snapshot
end
```

### The five sprite profiles

`nes`, `gb`, `md`, `msx` and `pce` share one surface:

| call | what |
| --- | --- |
| `bind()` | resolve regions + allocate, once; `true`, or `nil` + reason |
| `draw{x=, y=, scale=, ...}` | draw the frame; returns the counters below |
| `replace_sprite{...}` | register HD sprite substitution; returns a rule id |
| `remove_replacement(id)`, `clear_replacements()` | manage rules |
| `sprite_bounds()` | live screen bounds of the matched metasprite, or `nil` |

`replace_sprite` takes `tiles` (the sprite tile ids to substitute -- the key
is `patterns` on PCE, with `tiles` accepted as a ported-bezel alias), `image`
(the value `ab.image()` returns), optional `anchor_exclude` (tiles that are
suppressed but must not stretch the art -- shadow/filler tiles), and
`base_w` / `base_h` / `ring` (the footprint the art was cut for, plus its
transparent overhang in source pixels).

Draw options and results per platform:

- `nes.draw{x, y, scale, bg_surface=, spr_surface=}` ->
  `{bg_quads, spr_quads, hd_drawn, sprites_replaced}`
- `gb.draw{x, y, scale, bg_surface=, spr_surface=}` -> the same shape as NES
- `md.draw{x, y, scale}` -> `{quads, hd_drawn, sprites_replaced}`
  (Genesis, SMS and Game Gear all bind through this one profile)
- `msx.draw{x, y, scale, fit_width=}` -> `{quads, hd_drawn,
  sprites_replaced, supported, mode, width, per_line, vram_replay,
  retained}`. `supported = false` means the screen mode is not implemented
  and NOTHING was drawn -- branch on it rather than trusting a blank.
  `fit_width` squeezes the 512-wide SCREEN 6/7 modes into the narrow-mode
  footprint. Extras: `msx.mode()` -> mode, description (or `nil` + reason
  when unsupported) and `msx.sprites()` -> array of
  `{index, x, y, pattern, colour}`.
- `pce.draw{x, y, scale, height=, bg=, sprites=, fb_width=}` ->
  `{quads, hd_drawn, sprites_replaced, width, height}`. Pass `height` (the
  VDC registers cannot say how many lines were captured; default 224) and
  `fb_width` = the CORE's framebuffer width from `ab.game_width()` -- the
  VDC's display window is not the frame, and on clipped-window games the
  two disagree. `bg` / `sprites` force a layer on or off. Extra:
  `pce.geometry()` ->
  `{width, bg, sprites, bat_w, bat_h, scroll_x, scroll_y}`.

### The SNES profile

`snes` has its own shape -- Mode 7 HD re-projection rather than sprite
substitution:

- `snes.bind()` -> `true`, or `nil` + reason
- `snes.frame_size()` -> width, lines (or `nil`) -- geometry without drawing
- `snes.draw{x, y, scale}` -> `{w, h, quads}` or `nil` -- FAITHFUL
  reconstruction from the capture, no re-projection, no substitution; this
  is the corpus-certification path
- `snes.set_hd_tiles(blob)` -> indexed_count, rgba_count -- loads a
  tiles.bin v2 painted-tile blob for the re-projection
- `snes.tick{compare=}` -> `nil` (hi-res / no frame yet), `{}` (no Mode 7
  span this frame; the plain frame was drawn), or
  `{w, h, m7start, m7stop, plane_rebuilt}` -- the whole HD Mode 7 tick:
  streaming plane texture, palette animation, per-line UV mesh, sprite
  runs, and the optional side-by-side compare view

### The layer split: shading the world and the actors differently

`nes.draw` and `gb.draw` take two optional surface handles:

```lua
local r = nes.draw{ x = GAME_X, y = 0, scale = SCALE,
                    bg_surface = surf_bg, spr_surface = surf_spr }
ab.surface_filter(surf_bg,  surf_bg,  MELT)   -- the world liquefies
ab.surface_filter(surf_spr, surf_spr, GLOW)   -- the actors stay sharp
ab.draw_texture(surf_bg,  0, 0, 1920, 1080)
ab.draw_texture(surf_spr, 0, 0, 1920, 1080)
```

A redraw already emits the background and the sprites as separate
batches; these route them to separate offscreen surfaces in ONE draw --
one frame read, one sprite evaluation. That is the difference from
calling `draw` twice with layer toggles, which re-does both for what is a
single machine state.

What it buys: the two layers can get *different* effects. A shader over
the composited frame sees one flat bitmap -- the character and the wall
behind them are the same pixels -- so any effect hits both identically.
Split, the background can melt while sprites stay legible, and an edge
effect becomes possible at all: alpha on the sprite surface IS the sprite
mask, so a rim glow follows the character's outline. HD replacement art
follows the sprite layer, since substituted art replaces sprites.

Handle `0` (the default) means "draw wherever the guest already was", so
omitting both reproduces the old single-destination behaviour exactly.
One-sided splits are legal: `spr_surface` alone diverts the sprites and
leaves the background on the scene.

Only NES and GB support it. MD, MSX and PCE consume the core's RESOLVED
per-pixel planes -- priority, shadow and highlight are already applied
per pixel -- so there is no separable sprite batch to redirect, and those
profiles REFUSE the option (`nil` + reason in Lua, an exception in
Python/JS/Ruby) instead of ignoring it. A silently-dropped layer request
looks exactly like a working one until you notice both surfaces hold the
same complete picture.

Two things to know about surfaces when you use this:

- **Surfaces share the 1920x1080 logical canvas.** Geometry is projected
  against it regardless of the target's pixel size, so a smaller surface
  shows the top-left corner of the layout rather than a scaled copy.
  Make layer surfaces full logical size and position the game inside
  them.
- **A surface clears once per frame, on first target.** Re-entering one
  within a frame accumulates, which is what lets the two layer brackets
  of a single `draw` land on the same surface if you point both at it.
  Call `ab.clear` yourself when you want a blank one.

**The silent-fallback trap.** The profiles consume OPTIONAL per-line regions
from the core (per-scanline register records, VRAM/palette write logs,
resolved line buffers). When a region is missing, a profile falls back to the
end-of-frame snapshot with **no error anywhere**: the picture still draws,
just measurably worse on games that change state mid-frame. The result flags
above exist so a bezel (or a test) can assert on the path it got instead of
silently shipping the degraded one. `msx.draw` in particular reads:

| region | what it adds |
| --- | --- |
| `msx_vdp_reglines` | per-scanline registers + palette: raster splits |
| `msx_vram_deltas` | dot-stamped VRAM/reg/palette write log: mid-frame and mid-LINE rewrites |
| `msx_fb_tail` | the core's own snapshot of rows the frame never re-rendered (no state-only renderer can produce them) |

and `pce.draw` reads `pce_vdc_reglines`, `pce_vce_pallines`,
`pce_vdc_linepix`, `pce_vce_xofflines`, `pce_vce_srclines` and
`pce_paldeltas` (dot-stamped palette writes: mid-line recolours split the row
at the recorded pixel).

## Shader presets

```lua
local tube = ab.surface_create(512, 448)
ab.surface_preset(ab.GAME, tube, 'crt-lottes.glslp')   -- a whole multi-pass chain
ab.draw_texture(tube, 240, 0, 1440, 1080)
```

No shaders ship with this package. See
[the preset docs](../../docs/ACTIVE_BEZELS.md#shader-presets-glslp) for where to
get them and which ones work.

## Building the runtime

```sh
./build.sh   # needs emcc; fetches Lua 5.4.7 pinned, patches idempotently
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no WASI)
and exports the five ABI entry points. Lua's file loading and its wall-clock
RNG seed are compiled out — there is no filesystem and no wall clock in a
bezel, and either would drag WASI imports into the wasm.
