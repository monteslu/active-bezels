# The prebuilt Lua runtime

`main.wasm` here is a complete Active Bezel guest that embeds Lua 5.4 and
exposes the entire ab import surface to a script. A Lua bezel ships this wasm
as its `entry` plus its own `main.lua` (or `assets/main.lua`); iterating is
**edit + repack**, with no compiler in the loop. The runtime re-reads the
script on ASSETS_RELOADED and paints load/runtime errors on screen instead of
dying, so a broken script is a visible, fixable state.

## Script contract

```lua
function init() end          -- optional; once, after the script loads
function tick(frame) end     -- required; draw the whole scene every frame
function event(kind) end     -- optional; AB_EVENT numbers
```

The global `ab` table carries the API: `clear, draw_game, draw_game_fit,
fill_rect, triangle, text, scissor, scissor_reset, push_transform,
pop_transform, reset_transform, translate, scale, rotate, mesh,
texture_create, texture_destroy, draw_texture, draw_texture_rect, effect_set,
effect_clear, game_width, game_height, game_pixel, logical_width,
logical_height, physical_width, physical_height, elapsed_ms, delta_ms, input,
log, region, region_find_id, region_size, region_flags, region_offset,
region_generation, region_count, read_u8, write_u8, read, asset,
config_bool, config_number, config_string, rgb`.

Colors are packed `0xRRGGBBAA`; `ab.rgb(r, g, b[, a])` builds one.

## Building the runtime

```sh
./build.sh   # needs emcc; fetches Lua 5.4.7 pinned, patches idempotently
```

The build asserts the artifact imports **only `ab_host`** (no `env`, no
WASI) and exports the five ABI entry points. Lua's file loading and its
wall-clock RNG seed are compiled out -- there is no filesystem and no wall
clock in a bezel, and either would drag WASI imports into the wasm.

`examples/lua-native/` is a packageable starter that uses a copy of this
runtime.
