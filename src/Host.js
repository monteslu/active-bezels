/*
 * Host.js — the one place that knows how to reach a libretro core's memory.
 *
 * Why this exists
 * ---------------
 * `Regions.js` needs three things from whatever emulator is embedding this
 * runtime: `_retro_get_memory_data(id)`, `_retro_get_memory_size(id)`, and a
 * byte view of the core's heap. Every libretro core exposes exactly those, but
 * the *embedders* disagree about where the Emscripten module hangs:
 *
 *   retroemu   host.core        (and host.core.wasmMemory)
 *   romdev     host.mod         (LibretroHost, same exports, different name)
 *
 * That is a one-word difference, and the tempting fix is a small shim in each
 * consumer. Two shims is how the contract quietly forks: the day a region needs
 * a bounds check or a generation bump, one side gets it and the other doesn't,
 * and the symptom is "the composite differs between retroemu and romdev" —
 * which is miserable to chase, because both look correct in isolation.
 *
 * So the adapter lives here, with the code that depends on it. A new embedder
 * adds a case to `resolveCoreModule` and everything downstream works unchanged.
 */

/**
 * Find the Emscripten module on an embedder's host object.
 *
 * Accepts, in order: an object that already looks like the module itself, then
 * the two known embedder shapes. Returns null rather than throwing so a caller
 * can decide whether a missing core is fatal (it is at init) or merely means
 * "no regions this frame" (it is during teardown).
 *
 * @param {object|null} host
 * @returns {object|null} the Emscripten module, or null
 */
export function resolveCoreModule(host) {
  if (!host) return null;
  // Already a module (someone passed the core directly).
  if (typeof host._retro_get_memory_data === 'function') return host;
  // retroemu.
  if (host.core && typeof host.core._retro_get_memory_data === 'function') return host.core;
  // romdev's LibretroHost.
  if (host.mod && typeof host.mod._retro_get_memory_data === 'function') return host.mod;
  return null;
}

/**
 * The heap bytes backing a core's memory.
 *
 * Prefers the `WebAssembly.Memory` object when the core exposes one, because
 * that is what lets an Active Bezel share the core's memory directly rather
 * than copying it every frame. Falls back to HEAPU8's buffer, which every
 * Emscripten build has.
 *
 * NOTE: read this fresh each time rather than caching. Emscripten heaps are
 * detached and replaced on growth, and a cached view silently reads a dead
 * buffer — zeros, not an error, which is the worst possible failure here
 * because it looks like "the game state is empty" rather than "the view is
 * stale".
 *
 * @param {object|null} mod
 * @returns {Uint8Array|null}
 */
export function coreHeap(mod) {
  if (!mod) return null;
  if (mod.wasmMemory?.buffer) return new Uint8Array(mod.wasmMemory.buffer);
  if (mod.HEAPU8) return mod.HEAPU8;
  return null;
}

/**
 * The raw `WebAssembly.Memory`, when the core has one.
 *
 * Only meaningful for the zero-copy path: a guest may import the *identical*
 * memory object rather than reading through host functions. Returns null when
 * the core doesn't expose it, in which case the region-oriented path is used
 * and nothing else changes.
 *
 * @param {object|null} mod
 * @returns {WebAssembly.Memory|null}
 */
export function coreMemoryObject(mod) {
  return mod?.wasmMemory ?? null;
}

/**
 * Normalize any supported embedder into the shape the runtime consumes.
 *
 * Consumers may pass their own host straight to `ActiveBezelRuntime`; this is
 * exported for embedders that would rather adapt explicitly, and for tests that
 * want to stand up a fake core without imitating an embedder.
 *
 * @param {object} host
 * @returns {{core: object|null}} a host whose `.core` is the resolved module
 */
export function asBezelHost(host) {
  const mod = resolveCoreModule(host);
  // Preserve the original object's other properties (input managers, status,
  // whatever the embedder attaches) and only normalize where the core lives.
  return new Proxy(host, {
    get(target, prop, receiver) {
      if (prop === 'core') return mod;
      return Reflect.get(target, prop, receiver);
    },
  });
}
