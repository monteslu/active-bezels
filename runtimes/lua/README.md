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
function tick(frame) end     -- required; draw the whole scene, every frame
function event(kind) end     -- optional; the machine jumped (see ab.EVENT)
```

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
| **text** | `font` `print` `measure` `font_metrics` |
| **perspective** | `quad` |
| **surfaces** | `surface_create` `surface_target` `surface_end` `surface_filter` `surface_preset` |
| **shaders** | `effect_set` `effect_clear` |
| **memory** | `region` `region_find_id` `region_size` `region_flags` `region_offset` `region_count` `region_generation` `read_u8` `read_u16` `read_u24` `read_u32` `read` `write_u8` |
| **host** | `logical_width` `logical_height` `physical_width` `physical_height` `elapsed_ms` `delta_ms` `input` `log` `asset` `loadasset` `config_bool` `config_number` `config_string` `rgb` |

Constant tables, so scripts never hard-code ABI numbers: `ab.EVENT` `ab.FIT`
`ab.SAMPLE` `ab.DEVICE` `ab.BTN`. `ab.GAME` is the live-frame handle — pass it
anywhere a texture is wanted.

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

TrueType text is `ab.print`, matching Lua's own naming. `ab.text` is the
built-in 3×5 bitmap font — fine for debug, not for UI.

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

`examples/lua-native/` is a packageable starter built on a copy of this runtime.
