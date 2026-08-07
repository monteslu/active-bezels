# Ruby bezels

`main.wasm` embeds **mruby 3.4.0** and hands a script the entire Active Bezel
API. Ship this wasm as your package's `entry`, put a `main.rb` beside it, and
you have a bezel — no compiler, no build step.

```sh
cp -r node_modules/active-bezel/runtimes/ruby/start my-bezel
$EDITOR my-bezel/main.rb
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

`--active-bezel-dev` watches the directory, so saving `main.rb` reloads the
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

```ruby
def init; end            # optional; once, after the script loads
def pre_frame(frame); end # optional; BEFORE the core runs `frame`
def tick(frame); end     # required; draw the whole scene, every frame
def event(kind); end     # optional; the machine jumped (see AB::EVENT)
```
`pre_frame` (ABI 2) shapes the frame the core is ABOUT to run: region writes
land before the game's logic consumes them, and `AB.input_override(port, device, index, id, value)` replaces what the
core is polled with (`AB::BTN[:MASK]` writes the whole joypad word). Overrides
clear before every call -- re-assert each frame -- and `AB.input` keeps
reporting the PHYSICAL pad, so a remap can never read back its own output.
Calls outside `pre_frame` are refused (logged once). Frame 0 sees
post-reset, pre-execution RAM. The full contract, the analog-read forms
and the idempotence field notes live in the
[Lua runtime README](../lua/README.md#the-contract) -- one surface, four
bindings, only syntax differs.


A bezel owns the whole **1920×1080** picture, including where the game goes.
Coordinates are on that grid whatever the real output resolution is.

A broken script is a visible, fixable state, never a dead session: a syntax
error or a raise from `tick` paints an on-screen panel naming the line, and the
runtime re-reads `main.rb` on ASSETS_RELOADED.

## A first bezel

```ruby
W, H = 1920, 1080
GAME_W = H * 4 / 3          # 1440; the leftover IS the bezel

def init
  $ram = AB.region('system_ram')
end

def tick(frame)
  AB.clear(AB.rgb(14, 16, 26))
  AB.draw_game(0, 0, GAME_W, H, AB::SAMPLE[:NEAREST])

  # read the machine and show something the game never displays
  hp = AB.read_u8($ram, 0x0E)
  AB.fill_rect(GAME_W + 32, 80, 400 * (hp / 255.0), 24, AB.rgb(120, 200, 255))
  AB.text(format('HP %d', hp), GAME_W + 32, 130, 32, AB.rgb(235, 238, 250))
end
```

## The `AB` module

Colours are packed `0xRRGGBBAA`; `AB.rgb(r, g, b, a = 255)` builds one.

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
| **host** | `logical_width` `logical_height` `physical_width` `physical_height` `elapsed_ms` `delta_ms` `input` `log` `asset` `config_bool` `config_number` `config_string` `rgb` |

Constants are frozen Hashes keyed by Symbol: `AB::EVENT`, `AB::FIT`,
`AB::SAMPLE`, `AB::DEVICE`, `AB::BTN` (`AB::BTN[:START]`). `AB::GAME` is the
live-frame handle — pass it anywhere a texture is wanted.

### Ruby shapes

Where a Hash or String is the natural Ruby answer, that is what you get:

```ruby
img = AB.image('assets/logo.png')   # { texture:, width:, height: }
AB.draw_texture(img[:texture], 0, 0, img[:width], img[:height])

AB.mesh([{ x: 0, y: 0, rgba: 0xff00ffff },
         { x: 30, y: 0, rgba: 0xff00ffff },
         { x: 0, y: 30, rgba: 0xff00ffff }])   # Array of vertex Hashes

bytes = AB.read(ram, 0, 64)          # binary String
fm = AB.font_metrics(font, 40)       # { ascent:, descent:, line_height: }
```

**`AB.draw_text`, not `print`.** `print` is `Kernel#print`, which every object
already answers to, so a bare `print(...)` inside a class body would silently
reach Kernel's rather than the bezel's. There is no `AB.print` alias:
`draw_text` is the one name, spelled the same in all four runtimes.

## Platform redraw profiles

The profiles are modules spelled the way Ruby constants must be: `NES`,
`GB`, `MD`, `SNES`, `MSX`, `PCE`. The API is documented once, in
[the Lua README](../lua/README.md#platform-redraw-profiles); the renderers
are the same shared C core, so only the spelling differs here:

- options are a Hash, symbol or string keys alike:
  `MSX.draw({ x: 240, y: 60, scale: 4 })`
- configuration failures raise (`RuntimeError` for a missing binding or
  region set, `ArgumentError` for malformed arguments); transient
  conditions return `nil` (`draw` on a frame-read failure, `SNES.tick` /
  `SNES.draw` before the core has a frame)
- multi-value returns are Arrays: `sprite_bounds` is `[x0, y0, x1, y1]`,
  `MSX.mode` is `[mode, description]` with `mode` set to `nil` when the
  screen mode is unsupported, `SNES.frame_size` is `[w, h]`,
  `SNES.set_hd_tiles(blob)` is `[indexed_count, rgba_count]`
- draw results are Hashes with symbol keys, same names as the Lua tables

```ruby
def init
  NES.bind
  $hero = AB.image('assets/hero.png')
  NES.replace_sprite({ tiles: [0x53, 0x54], image: $hero,
                       base_w: 15, base_h: 16, ring: 4 })
end

def tick(frame)
  AB.clear(AB.rgb(0, 0, 0))
  r = NES.draw({ x: 0, y: 92, scale: 4 })
end
```

## Shader presets

```ruby
tube = AB.surface_create(512, 448)
AB.surface_preset(AB::GAME, tube, 'crt-lottes.glslp')   # a whole multi-pass chain
AB.draw_texture(tube, 240, 0, 1440, 1080)
```

No shaders ship with this package. See
[the preset docs](../../docs/ACTIVE_BEZELS.md#shader-presets-glslp) for where to
get them and which ones work.

## Building the runtime

```sh
./build.sh   # needs emcc + a host ruby/rake; fetches mruby 3.4.0 pinned
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no WASI)
and exports the five ABI entry points.

Two build settings are load-bearing, both learned from a failure:

- **`-sSUPPORT_LONGJMP=wasm` on cc AND linker.** mruby's exception handling is
  setjmp/longjmp, and emscripten's default JS-trampoline form cannot work
  under a host that provides only the `ab_host` module.
- **`MRB_INT64` in the mruby config AND `-DMRB_INT64` when compiling
  `runtime.c`.** mruby's word boxing on a 32-bit target caps fixnums at
  `INT32_MAX >> 1` (~1.07e9), so `mrb_int_value` *raises* `RangeError` on any
  `0xRRGGBBAA` color and on `ab_tick`'s uint64 frame counter. The two defines
  must match: `mrb_int` is part of the ABI of nearly every mruby entry point,
  and a mismatch links with "function signature mismatch" warnings and then
  corrupts arguments at runtime.

The gem set is deliberately lean (no `mruby-onig-regexp`, no `mruby-json`): a
bezel draws from emulator memory rather than parsing text, and dropping Onigmo
alone saves several hundred KB.

## Function reference

Every call, with its parameters. `[x]` is optional. Colours are packed
`0xRRGGBBAA`; geometry is in logical units on the 1920x1080 canvas.


### draw

| call | parameters | notes |
| --- | --- | --- |
| `AB.clear` | `rgba` | Fill the whole target. |
| `AB.fill_rect` | `x, y, w, h, rgba` | Axis-aligned rect. |
| `AB.triangle` | `x1, y1, x2, y2, x3, y3, rgba` |  |
| `AB.mesh` | `verts[, texture]` | verts: {x,y[,u,v][,rgba]}. texture may be ab.GAME. |
| `AB.text` | `str, x, y, size, rgba` | Built-in 3x5 debug font. Use draw_text for UI. |
| `AB.scissor` | `x, y, w, h` | Clip subsequent draws. |
| `AB.scissor_reset` | — |  |

### the game

| call | parameters | notes |
| --- | --- | --- |
| `AB.draw_game` | `x, y, w, h[, sampling]` | sampling: ab.SAMPLE.NEAREST|LINEAR. |
| `AB.draw_game_fit` | `[mode[, alignX[, alignY[, sampling]]]]` | mode: ab.FIT.* |
| `AB.game_width` | — | -> px |
| `AB.game_height` | — | -> px |
| `AB.game_pixel` | `x, y` | -> 0xRRGGBBAA of the live frame; 0 outside. |

### transform

| call | parameters | notes |
| --- | --- | --- |
| `AB.push_transform` | — |  |
| `AB.pop_transform` | — |  |
| `AB.reset_transform` | — |  |
| `AB.translate` | `x, y` |  |
| `AB.scale` | `x, y` |  |
| `AB.rotate` | `radians` |  |
| `AB.skew` | `x, y` |  |
| `AB.transform2d` | `a, b, c, d, e, f` | Full 2x3 matrix. |

### textures

| call | parameters | notes |
| --- | --- | --- |
| `AB.texture_create` | `rgba_bytes, w, h` | -> handle. Needs w*h*4 bytes. |
| `AB.texture_destroy` | `handle` |  |
| `AB.draw_texture` | `handle, x, y, w, h` | handle may be a SURFACE. |
| `AB.draw_texture_rect` | `handle, x, y, w, h, sx, sy, sw, sh` | Source sub-rect: atlases. |
| `AB.image` | `asset_name` | -> {texture, width, height} (decoded PNG/JPG/GIF/BMP). |
| `AB.image_data` | `asset_name` | -> raw decoded pixels + dimensions. |

### text

| call | parameters | notes |
| --- | --- | --- |
| `AB.font` | `asset_name` | -> font handle (TrueType). |
| `AB.draw_text` | `font, str, x, y, size, rgba` | Anti-aliased. NOT `print`. |
| `AB.measure` | `font, str, size` | -> width in logical units. |
| `AB.font_metrics` | `font, size` | -> ascent, descent, line height. |

### perspective

| call | parameters | notes |
| --- | --- | --- |
| `AB.quad` | `corners, texture` | corners: 4x {x,y} TL,TR,BR,BL. Perspective-correct. |

### surfaces

| call | parameters | notes |
| --- | --- | --- |
| `AB.surface_create` | `w, h` | -> handle. Shares the 1920x1080 LOGICAL canvas: geometry is projected against it whatever the pixel size. |
| `AB.surface_target` | `handle` | Redirect draws into it. Does NOT nest -- an end closes the innermost open surface. |
| `AB.surface_end` | — | Clears ONCE per surface per frame, on first target; re-entry accumulates. |
| `AB.surface_filter` | `source, destination, shader[, mask_texture]` | GLSL ES 3.00 fragment shader. source/destination may be equal (in-place). mask_texture arrives as `u_mask`, sampled NEAREST. |
| `AB.surface_preset` | `source, destination, preset_name` | Multi-pass .glslp chain. |

### shaders

| call | parameters | notes |
| --- | --- | --- |
| `AB.effect_set` | `shader_source` | -> false when there is no GPU backend. |
| `AB.effect_clear` | — |  |

### memory

| call | parameters | notes |
| --- | --- | --- |
| `AB.region` | `name` | -> index or nil. Re-resolve after a machine jump. |
| `AB.region_find_id` | `id` | -> index or nil. |
| `AB.region_size` | `index` | -> bytes |
| `AB.region_flags` | `index` | -> bit flags (read/write/snapshot). |
| `AB.region_offset` | `index` | -> host offset |
| `AB.region_count` | — |  |
| `AB.region_generation` | — | Bumps on reset/state load. |
| `AB.read_u8` | `index, offset` |  |
| `AB.read_u16` | `index, offset[, big_endian]` |  |
| `AB.read_u24` | `index, offset[, big_endian]` |  |
| `AB.read_u32` | `index, offset[, big_endian]` |  |
| `AB.read` | `index, offset, length` | -> bulk bytes. ONE crossing instead of N. |
| `AB.write_u8` | `index, offset, value` | -> 0 on a snapshot region (refuses). |

### host

| call | parameters | notes |
| --- | --- | --- |
| `AB.logical_width` | — | 1920 |
| `AB.logical_height` | — | 1080 |
| `AB.physical_width` | — |  |
| `AB.physical_height` | — |  |
| `AB.elapsed_ms` | — | Since the first tick. |
| `AB.delta_ms` | — | Clamped to 250. |
| `AB.input` | `port, device, index, id` | PHYSICAL pad, always. |
| `AB.input_override` | `port, device, index, id, value` | pre_frame ONLY. id 256 = whole joypad mask. |
| `AB.log` | `message` |  |
| `AB.asset` | `name` | -> bytes or nil. |
| `AB.config_bool` | `key, default` |  |
| `AB.config_number` | `key, default` |  |
| `AB.config_string` | `key, default` |  |
| `AB.rgb` | `r, g, b[, a]` | -> packed 0xRRGGBBAA. |

### redraw profiles

On `NES` and the other platform globals.

| call | parameters | notes |
| --- | --- | --- |
| `NES.bind` | — | -> true, or nil + reason. Call in init(). |
| `NES.draw` | `{x=, y=, scale=[, bg_surface=][, solid_surface=][, spr_surface=]}` | -> {bg_quads, spr_quads, hd_drawn, sprites_replaced}. The three surfaces route the emitted batches apart: backdrop, solid tiles, sprites. |
| `NES.hide_cell` | `cx, cy` | Do not draw this 8x8 BACKGROUND cell. Consumed by each draw. |
| `NES.hide_sprite` | `slot` | Do not draw this OAM slot. |
| `NES.isolate_sprite` | `slot` | Draw ONLY the marked slots this pass. Wins over hide_sprite. |
| `NES.replace_sprite` | `{tiles=, image=[, anchor_exclude=][, base_w=][, base_h=][, ring=]}` | -> rule id. |
| `NES.remove_replacement` | `id` |  |
| `NES.clear_replacements` | — |  |
| `NES.sprite_bounds` | — | -> x0,y0,x1,y1 of the matched metasprite, or nil. |

### Script errors

A script error is caught by the runtime, drawn on an on-screen panel, and
logged with an `AB-ERROR:` prefix. It is ALSO exported to the host through
`ab_last_error`, which is what makes it visible to tooling: the host's tick
call returns normally for this class of failure, so a host-side `error`
field stays null while the screen shows a stack trace. In romdev it surfaces
as `BEZEL_SCRIPT_ERROR` in the bezel status. A reload clears the latch, so a
fixed script recovers without restarting the host.
