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
  "requires": [{ "region": "system_ram", "minSize": 2048 }],
  "settings": []
}
```

Exact SHA-256 matches are authoritative. `compatible` rules may additionally
identify known revisions by platform, total size, and multiple byte signatures.
They are weaker and reported as such. Mismatches never auto-attach; the player
may explicitly force one.

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

## ABI

The machine-readable source of truth is `sdk/active-bezel/abi.json`; the C binding is
`sdk/active-bezel/active_bezel.h`.

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

## Lua

Set `runtime.language` to `lua54-wasmcart`, place Lua under `app/`, and reuse
the checked-in wasmcart-lua `main.wasm`. This is genuine Lua 5.4 with the
LÖVE-shaped wasmcart graphics API, not a subset or transpiler. Its framebuffer
is the background composition and the host places the current game over it.
Use the raw C ABI for a package that needs live machine-region reads or writes.

See `examples/lua-starter`.

The reproducible CPU/GPU numbers and methodology are in
[ACTIVE_BEZEL_BENCHMARK.md](ACTIVE_BEZEL_BENCHMARK.md).

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

## The prebuilt Lua runtime

`runtimes/lua/main.wasm` is a complete guest embedding Lua 5.4. A Lua bezel
ships that wasm as its `entry` plus its own `main.lua` (or `assets/main.lua`);
iteration is edit + repack, with no compiler in the loop. The global `ab`
table exposes the full import surface -- drawing, transforms, textures, mesh,
shader effects, live memory regions, config, input, time. The runtime
re-reads the script on ASSETS_RELOADED and renders script errors on screen
instead of dying, so a broken bezel is a visible, fixable state. The build
asserts the artifact imports only `ab_host` (no env, no WASI) and exports the
five ABI entry points. `examples/lua-native/` is a packageable starter.
