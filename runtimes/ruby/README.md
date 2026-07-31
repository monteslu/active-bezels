# The prebuilt Ruby runtime

`main.wasm` here is a complete Active Bezel guest that embeds mruby 3.4 and
exposes the entire ab import surface to a script. A Ruby bezel ships this wasm
as its `entry` plus its own `main.rb` (or `assets/main.rb`); iterating is
**edit + repack**, with no compiler in the loop. The runtime re-reads the
script on ASSETS_RELOADED and paints load/runtime errors on screen instead of
dying, so a broken script is a visible, fixable state.

## Script contract

```ruby
def init; end            # optional; once, after the script loads
def tick(frame); end     # required; draw the whole scene every frame
def event(kind); end     # optional; AB_EVENT numbers
```

The `AB` module carries the API as module functions: `clear, draw_game,
draw_game_fit, fill_rect, triangle, text, scissor, scissor_reset,
push_transform, pop_transform, reset_transform, translate, scale, rotate,
mesh, texture_create, texture_destroy, draw_texture, draw_texture_rect,
effect_set, effect_clear, game_width, game_height, game_pixel, logical_width,
logical_height, physical_width, physical_height, elapsed_ms, delta_ms, input,
log, region, region_find_id, region_size, region_flags, region_offset,
region_generation, region_count, read_u8, write_u8, read, asset, image,
image_data, font, draw_text, measure, font_metrics, read_u16, read_u24,
read_u32, config_bool, config_number, config_string, rgb`.

Colors are packed `0xRRGGBBAA`; `AB.rgb(r, g, b, a = 255)` builds one.

Constants are frozen Hashes: `AB::EVENT`, `AB::FIT`, `AB::SAMPLE`,
`AB::DEVICE`, `AB::BTN`, keyed by Symbol (`AB::BTN[:START]`).

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
reach Kernel's rather than the bezel's. `AB.print` still exists as an alias for
parity with the Lua runtime's `ab.print`, but `AB.draw_text` is the name that
cannot collide.

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
