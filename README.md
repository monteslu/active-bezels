# Active Bezels

[![npm](https://img.shields.io/npm/v/active-bezel)](https://www.npmjs.com/package/active-bezel)
[![tests](https://img.shields.io/github/actions/workflow/status/monteslu/active-bezels/test.yml?label=tests)](https://github.com/monteslu/active-bezels/actions/workflows/test.yml)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

An **Active Bezel** is an optional executable companion to a specific ROM.

It ships as a `.ab` file — an ordinary ZIP holding a manifest, a `main.wasm`
guest, and optional assets. It starts with the ROM, runs **once per emulated
frame**, reads the emulator's live memory regions, and renders the complete
final scene.

Despite the name, it is not limited to decorating empty side panels. The package
owns the whole output picture: it can centre the original game, push it to one
side, draw a live map of the world around it, overlay a HUD, or replace the
picture entirely.

```text
poll input → run one core frame → run the bezel tick for that frame
                                       │
                    reads the state the core just produced,
                    may write memory back, emits its scene
                                       │
                              composite and present
```

The core tick and the bezel tick are ordered on the same host frame. Nothing is
asynchronous, so the visible frame and the state used to enhance it are
deterministic.

## What this repository is

The format specification, the reference runtime, and the packaging tool.

It deliberately does not belong to any one emulator. Two hosts consume it today:

- **[retroemu](https://github.com/monteslu/retroemu)** renders Active Bezels to
  a window.
- **[romdev](https://github.com/monteslu/romdev)** composites them headlessly,
  so an agent can capture the composite, the raw core framebuffer, and the
  guest's own command stream for the same frame — which is what makes a package
  verifiable rather than merely plausible.

| | |
|---|---|
| [`docs/ACTIVE_BEZELS.md`](docs/ACTIVE_BEZELS.md) | the format and ABI |
| [`sdk/active_bezel.h`](sdk/active_bezel.h) | C header for guests |
| [`sdk/abi.json`](sdk/abi.json) | machine-readable ABI |
| [`bin/abtool.js`](bin/abtool.js) | `scaffold` / `verify` / `inspect` / `pack` |
| [`examples/`](examples/) | reference packages |

## Install

```sh
npm install active-bezel
```

```js
import { ActiveBezelRuntime } from 'active-bezel';

const runtime = await ActiveBezelRuntime.create({
  packagePath: 'Game.ab',
  host,                 // your emulator host
  romBytes,             // for exact hash matching
  platform: 'nes',
});

// after each core frame:
const composite = runtime.processFrame(gameRgba, gameW, gameH, frameNumber);
```

The runtime accepts either embedder shape — a host exposing the Emscripten
module as `.core` or as `.mod` — so you should not need an adapter. See
[`src/Host.js`](src/Host.js) if you are wiring a third.

## Authoring a package

```sh
npx abtool scaffold my-bezel c     # or: lua
npx abtool verify my-bezel
npx abtool pack my-bezel my-bezel.ab
npx abtool inspect my-bezel.ab
```

`pack` emits a deterministic stored ZIP. `.ab` is deliberately ordinary — rename
it to `.zip` and look inside.

Guests can be written in any language that emits suitable WebAssembly:
freestanding C, Rust, Zig, AssemblyScript. A Lua option bundles the wasmcart Lua
runtime and needs no compiler at all.

## Dependencies

`native-gles` is required — the hosts that consume this package need GL to run.
It is still bound *lazily*, because a native addon can be installed but
unloadable (a missing prebuild, a failed install script, a Node ABI mismatch),
and that must not take down package loading, manifest validation or the CPU
compositor, none of which need GL. When GL is unavailable for any reason the
runtime falls back to the CPU compositor, which is fully featured; the GL path
is a performance option, not a capability one.

`wasmcart` is genuinely optional and loads on demand: only a guest declaring
`runtime.language: "lua54-wasmcart"` needs it. A wasm guest never touches it.

## A caution worth repeating

A `.ab` can load cleanly, tick without trapping, and emit perfectly valid draw
commands while being **completely wrong about the game**.

That is not hypothetical. An early package for a maze game declared in its
`profile.json` that it read the player's room, X and Y; its readable `main.c`
contained room-aware logic; and the compiled `main.wasm` shipped alongside them
ignored the room byte entirely and plotted raw coordinates across a fake map,
with two diagnostic bars that looked convincingly like progress meters. Unit
tests proved the package loaded and drew. They proved nothing about whether the
picture meant anything.

So: validate that the guest reads the regions it claims to, correlate its output
against live state over a real play trace, and treat `profile.json` labels as
research leads until evidence says otherwise. `romdev`'s command and
region-access tracing exists specifically to make this class of error visible.

## Licence

MIT — see [LICENSE](LICENSE).

The bundled example packages (`diagnostic`, `lua-starter`) are generic: they
contain no game content and target no specific ROM.
