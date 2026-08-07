# Python bezels

`main.wasm` embeds **MicroPython 1.24.1** and hands a script the entire Active
Bezel API. At 237 KB it is the smallest of the four runtimes. Ship this wasm as
your package's `entry`, put a `main.py` beside it, and you have a bezel — no
compiler, no build step.

```sh
cp -r node_modules/active-bezel/runtimes/python/start my-bezel
$EDITOR my-bezel/main.py
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

`--active-bezel-dev` watches the directory, so saving `main.py` reloads the
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

```python
def init():         pass    # optional; once, after the script loads
def pre_frame(frame): pass # optional; BEFORE the core runs `frame`
def tick(frame):    pass    # required; draw the whole scene, every frame
def event(kind):    pass    # optional; the machine jumped (see ab.EVENT)
```
`pre_frame` (ABI 2) shapes the frame the core is ABOUT to run: region writes
land before the game's logic consumes them, and `ab.input_override(port, device, index, id, value)` replaces what the
core is polled with (`ab.BTN['MASK']` writes the whole joypad word). Overrides
clear before every call -- re-assert each frame -- and `ab.input` keeps
reporting the PHYSICAL pad, so a remap can never read back its own output.
Calls outside `pre_frame` are refused (logged once). Frame 0 sees
post-reset, pre-execution RAM. The full contract, the analog-read forms
and the idempotence field notes live in the
[Lua runtime README](../lua/README.md#the-contract) -- one surface, four
bindings, only syntax differs.


A bezel owns the whole **1920×1080** picture, including where the game goes.
Coordinates are on that grid whatever the real output resolution is.

A broken script is a visible, fixable state, never a dead session: a
`SyntaxError` or a traceback from `tick` paints an on-screen panel naming the
line, and the runtime re-reads `main.py` on ASSETS_RELOADED.

## A first bezel

```python
W, H = 1920, 1080
GAME_W = H * 4 // 3          # 1440; the leftover IS the bezel
ram = None

def init():
    global ram
    ram = ab.region('system_ram')

def tick(frame):
    ab.clear(ab.rgb(14, 16, 26))
    ab.draw_game(0, 0, GAME_W, H, ab.SAMPLE['NEAREST'])

    # read the machine and show something the game never displays
    hp = ab.read_u8(ram, 0x0E)
    ab.fill_rect(GAME_W + 32, 80, 400 * (hp / 255), 24, ab.rgb(120, 200, 255))
    ab.text('HP %d' % hp, GAME_W + 32, 130, 32, ab.rgb(235, 238, 250))
```

## The `ab` module

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
| **host** | `logical_width` `logical_height` `physical_width` `physical_height` `elapsed_ms` `delta_ms` `input` `log` `asset` `config_bool` `config_number` `config_string` `rgb` |

Constant tables, so scripts never hard-code ABI numbers: `ab.EVENT` `ab.FIT`
`ab.SAMPLE` `ab.DEVICE` `ab.BTN`, all plain dicts keyed by string
(`ab.BTN['START']`). `ab.GAME` is the live-frame handle — pass it anywhere a
texture is wanted.

### Python shapes

Structured returns are **dicts**, so subscript them:

```python
img = ab.image('assets/logo.png')           # {'texture':, 'width':, 'height':}
ab.draw_texture(img['texture'], 0, 0, img['width'], img['height'])

ab.mesh([                                    # dicts, or 5-tuples (x, y, rgba, u, v)
    {'x': 0,  'y': 0,  'rgba': ab.rgb(255, 0, 0)},
    {'x': 64, 'y': 0,  'rgba': ab.rgb(0, 255, 0)},
    {'x': 0,  'y': 64, 'rgba': ab.rgb(0, 0, 255)},
])

# four arbitrary corners, perspective-correct: a receding plane reads as
# depth rather than a PS1-style warp
ab.quad([{'x':100,'y':0}, {'x':300,'y':20}, {'x':320,'y':200}, {'x':80,'y':180}],
        img['texture'])
```

It is **`ab.draw_text`**, not `ab.print` — `print` is Python's own builtin and
shadowing it would be hostile. `ab.text` is the built-in 3×5 bitmap font, fine
for debug and not for UI.

`ab.read(id, off, len)` returns `bytes`. `ab.region(name)` returns `None` when
the platform has no such region, so guard it.

## What this MicroPython has, and does not

Real Python semantics: classes, closures, comprehensions, exceptions, f-string
style `%` formatting, `math`.

**No filesystem, no `os`, no `pip`, no `import` of your own modules.** A bezel
reads the machine and draws; everything it needs arrives through `ab`. Keep it
to one `main.py`.

## Platform redraw profiles

`nes`, `gb`, `md`, `snes`, `msx` and `pce` are available as modules, already
injected as globals the way `ab` is. The API is documented once, in
[the Lua README](../lua/README.md#platform-redraw-profiles); the renderers
are the same shared C core, so only the spelling differs here:

- options are a dict: `msx.draw({'x': 240, 'y': 60, 'scale': 4})`
- configuration failures raise (`RuntimeError` for a missing binding or
  region set, `ValueError` for malformed arguments); transient conditions
  return `None` (`draw` on a frame-read failure, `snes.tick` / `snes.draw`
  before the core has a frame)
- multi-value returns are tuples: `sprite_bounds()` is `(x0, y0, x1, y1)`,
  `msx.mode()` is `(mode, description)` with `mode` set to `None` when the
  screen mode is unsupported, `snes.frame_size()` is `(w, h)`,
  `snes.set_hd_tiles(blob)` is `(indexed_count, rgba_count)`
- draw results are dicts with the same keys as the Lua tables

```python
def init():
    nes.bind()
    hero = ab.image('assets/hero.png')
    nes.replace_sprite({'tiles': [0x53, 0x54], 'image': hero,
                        'base_w': 15, 'base_h': 16, 'ring': 4})

def tick(frame):
    ab.clear(ab.rgb(0, 0, 0))
    r = nes.draw({'x': 0, 'y': 92, 'scale': 4})
```

## Shader presets

```python
tube = ab.surface_create(512, 448)
ab.surface_preset(ab.GAME, tube, 'crt-lottes.glslp')   # a whole multi-pass chain
ab.draw_texture(tube, 240, 0, 1440, 1080)
```

No shaders ship with this package. See
[the preset docs](../../docs/ACTIVE_BEZELS.md#shader-presets-glslp) for where to
get them and which ones work.

## Building the runtime

```sh
./build.sh   # needs emcc; builds MicroPython's embed port at a pinned version
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no WASI)
and exports the five ABI entry points.

Three settings are load-bearing, each learned the hard way:

- **`MICROPY_LONGINT_IMPL`** is mandatory. Without it the lexer rejects
  `0xff0000ff` — which is every colour literal a bezel writes.
- **`MICROPY_GCREGS_SETJMP`** — the GC needs to scan registers, and the wasm
  target has no architecture-specific path.
- **`port/mphalport.c` is excluded.** Its `printf` drags WASI imports into the
  wasm, and then the module will not instantiate.

The GC heap is 2 MB, raised from the embed port's default 512 KB: decoded
images and font atlases live there, and 512 KB runs out on a real bezel.

## Function reference

Every call, with its parameters. `[x]` is optional. Colours are packed
`0xRRGGBBAA`; geometry is in logical units on the 1920x1080 canvas.


### draw

| call | parameters | notes |
| --- | --- | --- |
| `ab.clear` | `rgba` | Fill the whole target. |
| `ab.fill_rect` | `x, y, w, h, rgba` | Axis-aligned rect. |
| `ab.triangle` | `x1, y1, x2, y2, x3, y3, rgba` |  |
| `ab.mesh` | `verts[, texture]` | verts: {x,y[,u,v][,rgba]}. texture may be ab.GAME. |
| `ab.text` | `str, x, y, size, rgba` | Built-in 3x5 debug font. Use draw_text for UI. |
| `ab.scissor` | `x, y, w, h` | Clip subsequent draws. |
| `ab.scissor_reset` | — |  |

### the game

| call | parameters | notes |
| --- | --- | --- |
| `ab.draw_game` | `x, y, w, h[, sampling]` | sampling: ab.SAMPLE.NEAREST|LINEAR. |
| `ab.draw_game_fit` | `[mode[, alignX[, alignY[, sampling]]]]` | mode: ab.FIT.* |
| `ab.game_width` | — | -> px |
| `ab.game_height` | — | -> px |
| `ab.game_pixel` | `x, y` | -> 0xRRGGBBAA of the live frame; 0 outside. |

### transform

| call | parameters | notes |
| --- | --- | --- |
| `ab.push_transform` | — |  |
| `ab.pop_transform` | — |  |
| `ab.reset_transform` | — |  |
| `ab.translate` | `x, y` |  |
| `ab.scale` | `x, y` |  |
| `ab.rotate` | `radians` |  |
| `ab.skew` | `x, y` |  |
| `ab.transform2d` | `a, b, c, d, e, f` | Full 2x3 matrix. |

### textures

| call | parameters | notes |
| --- | --- | --- |
| `ab.texture_create` | `rgba_bytes, w, h` | -> handle. Needs w*h*4 bytes. |
| `ab.texture_destroy` | `handle` |  |
| `ab.draw_texture` | `handle, x, y, w, h` | handle may be a SURFACE. |
| `ab.draw_texture_rect` | `handle, x, y, w, h, sx, sy, sw, sh` | Source sub-rect: atlases. |
| `ab.image` | `asset_name` | -> {texture, width, height} (decoded PNG/JPG/GIF/BMP). |
| `ab.image_data` | `asset_name` | -> raw decoded pixels + dimensions. |

### text

| call | parameters | notes |
| --- | --- | --- |
| `ab.font` | `asset_name` | -> font handle (TrueType). |
| `ab.draw_text` | `font, str, x, y, size, rgba` | Anti-aliased. NOT `print`. |
| `ab.measure` | `font, str, size` | -> width in logical units. |
| `ab.font_metrics` | `font, size` | -> ascent, descent, line height. |

### perspective

| call | parameters | notes |
| --- | --- | --- |
| `ab.quad` | `corners, texture` | corners: 4x {x,y} TL,TR,BR,BL. Perspective-correct. |

### surfaces

| call | parameters | notes |
| --- | --- | --- |
| `ab.surface_create` | `w, h` | -> handle. Shares the 1920x1080 LOGICAL canvas: geometry is projected against it whatever the pixel size. |
| `ab.surface_target` | `handle` | Redirect draws into it. Does NOT nest -- an end closes the innermost open surface. |
| `ab.surface_end` | — | Clears ONCE per surface per frame, on first target; re-entry accumulates. |
| `ab.surface_filter` | `source, destination, shader[, mask_texture]` | GLSL ES 3.00 fragment shader. source/destination may be equal (in-place). mask_texture arrives as `u_mask`, sampled NEAREST. |
| `ab.surface_preset` | `source, destination, preset_name` | Multi-pass .glslp chain. |

### shaders

| call | parameters | notes |
| --- | --- | --- |
| `ab.effect_set` | `shader_source` | -> false when there is no GPU backend. |
| `ab.effect_clear` | — |  |

### memory

| call | parameters | notes |
| --- | --- | --- |
| `ab.region` | `name` | -> index or nil. Re-resolve after a machine jump. |
| `ab.region_find_id` | `id` | -> index or nil. |
| `ab.region_size` | `index` | -> bytes |
| `ab.region_flags` | `index` | -> bit flags (read/write/snapshot). |
| `ab.region_offset` | `index` | -> host offset |
| `ab.region_count` | — |  |
| `ab.region_generation` | — | Bumps on reset/state load. |
| `ab.read_u8` | `index, offset` |  |
| `ab.read_u16` | `index, offset[, big_endian]` |  |
| `ab.read_u24` | `index, offset[, big_endian]` |  |
| `ab.read_u32` | `index, offset[, big_endian]` |  |
| `ab.read` | `index, offset, length` | -> bulk bytes. ONE crossing instead of N. |
| `ab.write_u8` | `index, offset, value` | -> 0 on a snapshot region (refuses). |

### host

| call | parameters | notes |
| --- | --- | --- |
| `ab.logical_width` | — | 1920 |
| `ab.logical_height` | — | 1080 |
| `ab.physical_width` | — |  |
| `ab.physical_height` | — |  |
| `ab.elapsed_ms` | — | Since the first tick. |
| `ab.delta_ms` | — | Clamped to 250. |
| `ab.input` | `port, device, index, id` | PHYSICAL pad, always. |
| `ab.input_override` | `port, device, index, id, value` | pre_frame ONLY. id 256 = whole joypad mask. |
| `ab.log` | `message` |  |
| `ab.asset` | `name` | -> bytes or nil. |
| `ab.config_bool` | `key, default` |  |
| `ab.config_number` | `key, default` |  |
| `ab.config_string` | `key, default` |  |
| `ab.rgb` | `r, g, b[, a]` | -> packed 0xRRGGBBAA. |

### redraw profiles

On `nes` and the other platform globals.

| call | parameters | notes |
| --- | --- | --- |
| `nes.bind` | — | -> true, or nil + reason. Call in init(). |
| `nes.draw` | `{x=, y=, scale=[, bg_surface=][, solid_surface=][, spr_surface=]}` | -> {bg_quads, spr_quads, hd_drawn, sprites_replaced}. The three surfaces route the emitted batches apart: backdrop, solid tiles, sprites. |
| `nes.hide_cell` | `cx, cy` | Do not draw this 8x8 BACKGROUND cell. Consumed by each draw. |
| `nes.hide_sprite` | `slot` | Do not draw this OAM slot. |
| `nes.isolate_sprite` | `slot` | Draw ONLY the marked slots this pass. Wins over hide_sprite. |
| `nes.replace_sprite` | `{tiles=, image=[, anchor_exclude=][, base_w=][, base_h=][, ring=]}` | -> rule id. |
| `nes.remove_replacement` | `id` |  |
| `nes.clear_replacements` | — |  |
| `nes.sprite_bounds` | — | -> x0,y0,x1,y1 of the matched metasprite, or nil. |

### Script errors

A script error is caught by the runtime, drawn on an on-screen panel, and
logged with an `AB-ERROR:` prefix. It is ALSO exported to the host through
`ab_last_error`, which is what makes it visible to tooling: the host's tick
call returns normally for this class of failure, so a host-side `error`
field stays null while the screen shows a stack trace. In romdev it surfaces
as `BEZEL_SCRIPT_ERROR` in the bezel status. A reload clears the latch, so a
fixed script recovers without restarting the host.
