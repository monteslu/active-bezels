// Host.js — the embedder adapter.
//
// This is the only module written for the extraction rather than moved from
// retroemu, so it is the only one without inherited coverage.
//
// The problem it solves: every libretro core exposes the same three things
// (_retro_get_memory_data, _retro_get_memory_size, a heap view), but embedders
// disagree about where the Emscripten module hangs — retroemu puts it on
// `host.core`, romdev's LibretroHost on `host.mod`. That one-word difference
// is the kind of thing that gets papered over with a shim in each consumer,
// and then the contract quietly forks: one side gains a bounds check, the
// other doesn't, and the symptom is "the composite differs between retroemu
// and romdev" — miserable to chase, because both look right in isolation.

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolveCoreModule, coreHeap, coreMemoryObject, asBezelHost } from '../src/Host.js';
import { ActiveBezelRegions } from '../src/Regions.js';

/** A minimal stand-in for an Emscripten libretro module. */
function fakeModule({ withWasmMemory = false, heapBytes = 256 } = {}) {
  const buffer = new ArrayBuffer(heapBytes);
  const mod = {
    HEAPU8: new Uint8Array(buffer),
    _retro_get_memory_data: (id) => (id === 2 ? 64 : 0),
    _retro_get_memory_size: (id) => (id === 2 ? 128 : 0),
  };
  if (withWasmMemory) mod.wasmMemory = { buffer };
  return mod;
}

test('resolves the module from both embedder shapes and from a bare module', () => {
  const mod = fakeModule();
  assert.equal(resolveCoreModule({ core: mod }), mod, 'retroemu shape');
  assert.equal(resolveCoreModule({ mod }), mod, 'romdev LibretroHost shape');
  assert.equal(resolveCoreModule(mod), mod, 'the module passed directly');
});

test('returns null rather than throwing when there is no core', () => {
  // Callers distinguish "fatal at init" from "no regions during teardown", so
  // this must not throw.
  assert.equal(resolveCoreModule(null), null);
  assert.equal(resolveCoreModule({}), null);
  assert.equal(resolveCoreModule({ core: {} }), null, 'an object without the exports is not a core');
});

test('prefers WebAssembly.Memory when the core exposes one', () => {
  // The zero-copy path: a guest may import the identical memory object rather
  // than reading through host functions.
  const withMem = fakeModule({ withWasmMemory: true });
  assert.ok(coreMemoryObject(withMem), 'memory object surfaced');
  assert.equal(coreMemoryObject(fakeModule()), null, 'absent when the core has none');
  assert.ok(coreHeap(withMem) instanceof Uint8Array);
  assert.ok(coreHeap(fakeModule()) instanceof Uint8Array, 'falls back to HEAPU8');
  assert.equal(coreHeap(null), null);
});

test('the heap view is re-read, so heap growth cannot leave a stale window', () => {
  // Emscripten detaches and replaces the heap on growth. A cached view then
  // reads ZEROS rather than throwing, which looks like "the game state is
  // empty" — the worst possible failure mode for a bezel reading RAM.
  const mod = fakeModule();
  mod.HEAPU8[64] = 0xAB;
  const host = { mod };
  const regions = new ActiveBezelRegions(host, new Uint8Array([1, 2, 3]));
  const systemRam = regions.regions.findIndex((r) => r.name === 'system_ram');
  assert.ok(systemRam >= 0, 'system_ram present');
  assert.equal(regions.read(systemRam, 0), 0xAB);

  // Simulate growth: a brand-new backing buffer, as Emscripten would install.
  const grown = new ArrayBuffer(1024);
  mod.HEAPU8 = new Uint8Array(grown);
  mod.HEAPU8[64] = 0xCD;
  assert.equal(regions.read(systemRam, 0), 0xCD, 'read followed the new heap');
});

test('Regions works through either embedder shape, identically', () => {
  const a = fakeModule();
  const b = fakeModule();
  a.HEAPU8[64] = 0x11;
  b.HEAPU8[64] = 0x11;

  const viaCore = new ActiveBezelRegions({ core: a }, new Uint8Array([0]));
  const viaMod = new ActiveBezelRegions({ mod: b }, new Uint8Array([0]));

  const i = viaCore.regions.findIndex((r) => r.name === 'system_ram');
  const j = viaMod.regions.findIndex((r) => r.name === 'system_ram');
  assert.equal(viaCore.read(i, 0), viaMod.read(j, 0), 'same value through both shapes');
  assert.deepEqual(
    viaCore.describe().map((r) => r.name),
    viaMod.describe().map((r) => r.name),
    'same region table through both shapes',
  );
});

test('writes land in the core heap, and read-only regions reject them', () => {
  const mod = fakeModule();
  const regions = new ActiveBezelRegions({ mod }, new Uint8Array([9, 9, 9]));
  const systemRam = regions.regions.findIndex((r) => r.name === 'system_ram');
  assert.equal(regions.write(systemRam, 1, 0x5A), 1);
  assert.equal(mod.HEAPU8[65], 0x5A, 'wrote through to the core');

  // cart_source is flagged immutable (1 | 8) — a guest must not rewrite the ROM.
  const cart = regions.regions.findIndex((r) => r.name === 'cart_source');
  assert.ok(cart >= 0);
  assert.equal(regions.write(cart, 0, 0xFF), 0, 'immutable region refuses the write');
  assert.equal(regions.read(cart, 0), 9, 'ROM bytes unchanged');
});

test('out-of-range access is clamped, not thrown', () => {
  // A guest is allowed to be wrong; it must not be able to crash the host or
  // read outside its region.
  const regions = new ActiveBezelRegions({ mod: fakeModule() }, new Uint8Array([1]));
  const i = regions.regions.findIndex((r) => r.name === 'system_ram');
  assert.equal(regions.read(i, -1), 0);
  assert.equal(regions.read(i, 999999), 0);
  assert.equal(regions.write(i, -1, 1), 0);
  assert.equal(regions.write(i, 999999, 1), 0);
  assert.equal(regions.read(4242, 0), 0, 'unknown region index');
});

test('asBezelHost normalizes to .core while preserving other host properties', () => {
  const mod = fakeModule();
  const host = asBezelHost({ mod, inputManager: 'keep-me', status: { platform: 'nes' } });
  assert.equal(host.core, mod, 'core resolved');
  assert.equal(host.inputManager, 'keep-me', 'unrelated properties survive');
  assert.equal(host.status.platform, 'nes');
});
