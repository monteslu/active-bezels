# Lua Native Starter

An Active Bezel written in Lua on the **prebuilt runtime** (`runtimes/lua/main.wasm`).
`main.wasm` here is a verbatim copy of that runtime; the bezel itself is
`assets/main.lua`. There is no compile step:

    edit assets/main.lua
    abtool pack . my-bezel.ab

The runtime exposes the complete ab import surface as the global `ab` table
(drawing, transforms, textures, mesh, shader effects, live memory regions,
config, input, time) plus batteries: `ab.image` (PNG/JPG/GIF/BMP via
stb_image), `ab.font`/`ab.print`/`ab.measure` (anti-aliased TrueType via
stb_truetype, one white atlas tinted per call), `ab.read_u16/u24/u32`,
`ab.loadasset`, and constant tables (`ab.EVENT`, `ab.FIT`, `ab.SAMPLE`,
`ab.DEVICE`, `ab.BTN`). It reloads the script on ASSETS_RELOADED and renders
load or runtime errors on screen instead of dying.

`assets/roboto-medium.ttf` is Roboto Medium (Apache-2.0, Google).
