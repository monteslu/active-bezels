# The prebuilt JavaScript runtime

`main.wasm` here is a complete Active Bezel guest that embeds QuickJS and
exposes the entire ab import surface to a script. A JS bezel ships this wasm
as its `entry` plus its own `main.js` (or `assets/main.js`); iterating is
**edit + repack**, with no compiler in the loop. The runtime re-reads the
script on ASSETS_RELOADED and paints load/runtime errors on screen instead of
dying, so a broken script is a visible, fixable state.

## Script contract

```js
function init() {}          // optional; once, after the script loads
function tick(frame) {}     // required; draw the whole scene every frame
function event(kind) {}     // optional; AB_EVENT numbers
```

Scripts are evaluated as classic global scripts, not modules: `init`/`tick`/
`event` have to be reachable as globals, and there is no filesystem for
`import` to resolve against.

The global `ab` object carries the API: `clear, draw_game, draw_game_fit,
fill_rect, triangle, text, scissor, scissor_reset, push_transform,
pop_transform, reset_transform, translate, scale, rotate, mesh,
texture_create, texture_destroy, draw_texture, draw_texture_rect, effect_set,
effect_clear, game_width, game_height, game_pixel, logical_width,
logical_height, physical_width, physical_height, elapsed_ms, delta_ms, input,
log, region, region_find_id, region_size, region_flags, region_offset,
region_generation, region_count, read_u8, write_u8, read, asset, asset_text,
image, image_data, font, print, measure, font_metrics, read_u16, read_u24,
read_u32, config_bool, config_number, config_string, rgb`, plus the constant
tables `ab.EVENT, ab.FIT, ab.SAMPLE, ab.DEVICE, ab.BTN`.

Colors are packed `0xRRGGBBAA`; `ab.rgb(r, g, b[, a])` builds one. Both
spellings work -- a literal like `0xff0000ff` and a computed `(r << 24) | ...`
(which JS makes a *signed* int32) land on the same color.

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

## Building the runtime

```sh
./build.sh   # needs emcc; seeds QuickJS from a known-good checkout
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no WASI)
and exports the five ABI entry points, and it fails on any other import
module rather than shipping a wasm the host cannot instantiate.

QuickJS is trimmed to what a drawing script uses. `Date`, `Promise`, `Proxy`,
`WeakRef` and `BigInt` are left out on purpose: there is no wall clock (time
comes from `ab.elapsed_ms`), and nothing pumps a job queue, so a promise would
never settle. `RegExp`, `JSON`, `Map`/`Set` and typed arrays are in.
