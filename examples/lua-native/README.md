# Lua Native Starter

An Active Bezel written in Lua on the **prebuilt runtime** (`runtimes/lua/main.wasm`).
`main.wasm` here is a verbatim copy of that runtime; the bezel itself is
`assets/main.lua`. There is no compile step:

    edit assets/main.lua
    abtool pack . my-bezel.ab

The runtime exposes the complete ab import surface as the global `ab` table
(drawing, transforms, textures, mesh, shader effects, live memory regions,
config, input, time), reloads the script on ASSETS_RELOADED, and renders load
or runtime errors on screen instead of dying.
