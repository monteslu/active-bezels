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
def pre_render(frame); end # optional; BEFORE the core runs `frame`
def tick(frame); end     # required; draw the whole scene, every frame
def event(kind); end     # optional; the machine jumped (see AB::EVENT)
```
`pre_render` (ABI 2) shapes the frame the core is ABOUT to run: region writes
land before the game's logic consumes them, and `AB.input_override(port, device, index, id, value)` replaces what the
core is polled with (`AB::BTN[:MASK]` writes the whole joypad word). Overrides
clear before every call -- re-assert each frame -- and `AB.input` keeps
reporting the PHYSICAL pad, so a remap can never read back its own output.
Calls outside `pre_render` are refused (logged once). Frame 0 sees
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
