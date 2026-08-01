# JavaScript bezels

`main.wasm` embeds **QuickJS 0.15.1** and hands a script the entire Active
Bezel API. Ship this wasm as your package's `entry`, put a `main.js` beside it,
and you have a bezel — no compiler, no build step, no bundler.

```sh
cp node_modules/active-bezel/runtimes/js/main.wasm my-bezel/
cp node_modules/active-bezel/runtimes/js/main.js   my-bezel/   # the scaffold
$EDITOR my-bezel/main.js
```

The scaffold is a **working bezel**, not a stub: a commented example of every
capability, which you edit down to what you want. Point `romdev` at the
directory and the loop is edit, reload, look.

## The contract

```js
function init() {}          // optional; once, after the script loads
function tick(frame) {}     // required; draw the whole scene, every frame
function event(kind) {}     // optional; the machine jumped (see ab.EVENT)
```

A bezel owns the whole **1920×1080** picture, including where the game goes.
Coordinates are on that grid whatever the real output resolution is.

A broken script is a visible, fixable state, never a dead session: a syntax
error or a throw from `tick` paints an on-screen panel naming the line, and the
runtime re-reads `main.js` on ASSETS_RELOADED.

## A first bezel

```js
const W = 1920, H = 1080;
const GAME_W = Math.floor(H * 4 / 3);   // 1440; the leftover IS the bezel
let ram = null;

function init() {
  ram = ab.region('system_ram');
}

function tick(frame) {
  ab.clear(ab.rgb(14, 16, 26));
  ab.draw_game(0, 0, GAME_W, H, ab.SAMPLE.NEAREST);

  // read the machine and show something the game never displays
  const hp = ab.read_u8(ram, 0x0E);
  ab.fill_rect(GAME_W + 32, 80, 400 * (hp / 255), 24, ab.rgb(120, 200, 255));
  ab.text(`HP ${hp}`, GAME_W + 32, 130, 32, ab.rgb(235, 238, 250));
}
```

Scripts are evaluated as classic global scripts, not modules: `init`/`tick`/
`event` have to be reachable as globals, and there is no filesystem for
`import` to resolve against.

## The `ab` object

Colours are packed `0xRRGGBBAA`; `ab.rgb(r, g, b[, a])` builds one. Both
spellings work — a literal like `0xff0000ff` and a computed `(r << 24) | ...`
(which JS makes a *signed* int32) land on the same colour.

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
| **host** | `logical_width` `logical_height` `physical_width` `physical_height` `elapsed_ms` `delta_ms` `input` `log` `asset` `asset_text` `config_bool` `config_number` `config_string` `rgb` |

Constant tables: `ab.EVENT` `ab.FIT` `ab.SAMPLE` `ab.DEVICE` `ab.BTN`.
`ab.GAME` is the live-frame handle — pass it anywhere a texture is wanted.

Names and semantics match the Lua runtime exactly, so a bezel ports between
the two by changing syntax only. Where JS has a better shape, it wins:

| call | returns |
| --- | --- |
| `ab.image(name)`, `ab.image_data(bytes)` | `{ texture, width, height }` |
| `ab.font_metrics(font, px)` | `{ ascent, descent, lineHeight }` |
| `ab.read(id, off, len)`, `ab.asset(name)` | `Uint8Array` |
| `ab.mesh(verts[, texture])` | takes `[{ x, y, rgba, u, v }, ...]` |
| `ab.region(name)`, `ab.config_string(key)` | value, or `null` when absent |

`ab.texture_create(pixels, w, h)` accepts any typed array or `ArrayBuffer` of
RGBA bytes. `console.log/warn/error/info` are wired to `ab.log`.

## Shader presets

```js
const tube = ab.surface_create(512, 448);
ab.surface_preset(ab.GAME, tube, 'crt-lottes.glslp');   // a whole multi-pass chain
ab.draw_texture(tube, 240, 0, 1440, 1080);
```

No shaders ship with this package. See
[the preset docs](../../docs/ACTIVE_BEZELS.md#shader-presets-glslp) for where to
get them and which ones work.

## Building the runtime

```sh
./build.sh   # needs emcc; seeds QuickJS v0.15.1 from a known-good checkout
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no WASI)
and exports the five ABI entry points, and it fails on any other import
module rather than shipping a wasm the host cannot instantiate.

QuickJS is trimmed to what a drawing script uses. `Date`, `Promise`, `Proxy`,
`WeakRef` and `BigInt` are left out on purpose: there is no wall clock (time
comes from `ab.elapsed_ms`), and nothing pumps a job queue, so a promise would
never settle. `RegExp`, `JSON`, `Map`/`Set` and typed arrays are in.
