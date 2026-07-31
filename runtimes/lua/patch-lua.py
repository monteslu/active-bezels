#!/usr/bin/env python3
"""Guard Lua's file-loading functions behind AB_LUA_NOFILES.

An Active Bezel has no filesystem: the runtime reads main.lua from the
package's asset bundle through ab_asset_read, never from a path. So
luaL_loadfilex/luaL_dofile are dead code -- and they are the last things in
the VM that touch stdio (fopen/getc/freopen). Linking them drags WASI imports
into the wasm, and the Active Bezel host provides only ab_host/ab_core.

Same approach as wasmcart-lua's patch, independently applied: upstream Lua is
unmodified on disk apart from this guard, added idempotently at build time.
"""
import sys

MARKER = "AB_LUA_NOFILES"
START = "static int skipcomment"
END = "typedef struct LoadS"


def patch_lauxlib(path):
    src = open(path, encoding="utf-8").read()
    if MARKER in src:
        print(f"patch-lua: {path} already patched")
        return 0
    a, b = src.find(START), src.find(END)
    if a < 0 or b < 0 or b <= a:
        print(f"patch-lua: FAILED to locate the file-loading block in {path}", file=sys.stderr)
        return 1
    out = (src[:a]
           + f"#if !defined({MARKER})\n" + src[a:b] + f"#endif /* {MARKER} */\n\n"
           + src[b:])
    open(path, "w", encoding="utf-8").write(out)
    print(f"patch-lua: guarded file loading in {path}")
    return 0


def patch_mathlib(path):
    """Fixed default RNG seed: bezels have no wall clock, and the default
    time(NULL) seed both imports WASI clock_time_get and breaks replay
    determinism."""
    src = open(path, encoding="utf-8").read()
    if MARKER in src:
        print(f"patch-lua: {path} already patched")
        return 0
    old = "lua_Unsigned seed1 = (lua_Unsigned)time(NULL);"
    if old not in src:
        print(f"patch-lua: FAILED to locate the time() seed in {path}", file=sys.stderr)
        return 1
    new = ("/* " + MARKER + ": no wall clock in a bezel; a fixed seed keeps\n"
           "     the wasm free of WASI imports and replays reproducible. */\n"
           "  lua_Unsigned seed1 = (lua_Unsigned)0x2545F4914F6CDD1DULL;")
    open(path, "w", encoding="utf-8").write(src.replace(old, new, 1))
    print(f"patch-lua: fixed the RNG seed in {path}")
    return 0


if __name__ == "__main__":
    path = sys.argv[1]
    sys.exit(patch_mathlib(path) if path.endswith("lmathlib.c") else patch_lauxlib(path))
