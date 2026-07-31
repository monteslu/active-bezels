# Prebuilt runtimes

Four complete guest runtimes, each a wasm that embeds a scripting language and
exposes the entire Active Bezel ABI to it. A bezel written against one of these
needs **no toolchain**: copy the wasm and a script, edit, reload.

| Directory | Language | Size | Script | API entry |
|---|---|---|---|---|
| [`lua/`](lua/) | Lua 5.4.7 | 345 KB | `main.lua` | global `ab` table |
| [`python/`](python/) | MicroPython 1.24.1 | 242 KB | `main.py` | global `ab` module |
| [`js/`](js/) | QuickJS | 544 KB | `main.js` | global `ab` object |
| [`ruby/`](ruby/) | mruby 3.4.0 | 587 KB | `main.rb` | `AB` module |

Each directory holds:

- `main.wasm` — the shipped artifact, committed so consumers never need emcc
- `main.<ext>` — a commented scaffold that is a working bezel on its own
- `build.sh` — rebuilds the wasm from a pinned engine version
- `README.md` — language-specific notes and gotchas

## Shared

[`common/`](common/) is the C that every runtime links, so a PNG decodes
identically and text rasterises identically no matter the language:

| File | What |
|---|---|
| `ab_batteries.{h,c}` | image decode, TrueType atlas + tinted mesh text, multi-byte region reads, colour packing, asset slurping |
| `ab_wasi_stubs.c` | the stubs that keep an interpreter importing **only** `ab_host` |
| `stb_image.h`, `stb_truetype.h` | upstream single-headers, compiled once in `ab_batteries.c` |

Adding a fifth language means writing a binding file and a build script. It does
not mean reimplementing images or fonts.

## The contract every runtime holds

- **Imports only `ab_host`.** No `env`, no `wasi_snapshot_preview1`. The host
  provides one module and nothing else, so any other import means the wasm will
  not instantiate. Each `build.sh` asserts this and fails the build rather than
  letting it surface in someone's emulator.
- **Exports the five ABI functions.** `ab_abi_version`, `ab_init`, `ab_tick`,
  `ab_event`, `ab_shutdown`.
- **A script error is not a crash.** Load and runtime errors are logged, drawn
  on an on-screen panel with the failing line, and survivable: the runtime keeps
  ticking so the next reload can fix it. `ab_init` returns success even when the
  script failed, because the error state still needs frames to display itself.
- **Hot reload.** On `ASSETS_RELOADED` the runtime reboots its interpreter and
  re-reads the script. The host side of that is
  `ActiveBezelRuntime.reloadAssets()`, which re-opens the package from disk
  first — firing the event alone would have the guest re-read bytes the package
  had already cached.

## Why these four

Each was chosen because there is working precedent for embedding it in this
codebase, and the hard lessons transferred:

- **Lua** — smallest, fastest to start, the reference implementation.
- **MicroPython** — real Python semantics at a third of QuickJS's size. Note
  `MICROPY_LONGINT_IMPL` is mandatory: without it the lexer rejects
  `0xff0000ff`, which is every colour literal a bezel writes.
- **QuickJS** — modern JS, the language most authors already know.
  `JS_AddIntrinsicEval` is required even though scripts never call `eval`.
- **mruby** — Ruby's expressiveness at a reasonable size. `MRB_INT64` is
  mandatory (word boxing otherwise caps fixnums below `0xff0000ff`), and
  `capture_errors` prevents a syntax error from hanging the host through the
  stderr path.

## Building

```sh
cd runtimes/<lang> && ./build.sh
```

Needs emsdk on PATH. Each build fetches its engine at a pinned version, applies
whatever patches that engine needs to live without a filesystem, links the
shared batteries, and then self-verifies imports, exports and size.

`node scripts/check-runtimes.mjs` verifies every committed wasm at once.
