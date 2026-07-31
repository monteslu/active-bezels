/* abconf.h -- compile-time Lua configuration for the Active Bezel runtime.
 *
 * Injected with -include before every Lua translation unit, ahead of
 * lauxlib.h's `#if !defined(lua_writestring)` guards, so Lua's stdout and
 * stderr hooks resolve to the host's log import instead of stdio. Keeping
 * stdio out is what keeps WASI imports out of the wasm.
 */
#ifndef AB_LUA_CONF_H
#define AB_LUA_CONF_H

#include <stddef.h>

void ab_runtime_write(const char *data, size_t length);
void ab_runtime_writeline(void);

/* Deterministic interpreter seed. Stock Lua mixes time(NULL) and ASLR
 * addresses (lstate.c), which imports WASI clock_time_get and makes replays
 * diverge. A bezel has neither a wall clock nor ASLR worth harvesting. */
#define luai_makeseed(L) ((unsigned int)0x9E3779B9u)

#define lua_writestring(s, l)      ab_runtime_write((s), (l))
#define lua_writeline()            ab_runtime_writeline()
#define lua_writestringerror(s, p) ab_runtime_write((s), 0)

#endif
