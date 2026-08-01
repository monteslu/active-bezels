import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { existsSync } from 'node:fs';
import { ActiveBezelPackage, validateManifest } from '../src/Package.js';
import { matchActiveBezel } from '../src/Matcher.js';
import { ActiveBezelRuntime, AB_EVENT, MAX_DELTA_MS } from '../src/Runtime.js';
import { ActiveBezelConfig } from '../src/Config.js';
import { ActiveBezelCompositor, LOGICAL_WIDTH, LOGICAL_HEIGHT } from '../src/Compositor.js';
import { ActiveBezelGpuCompositor } from '../src/GpuCompositor.js';
import { CORE_REGIONS } from '../src/Regions.js';
import { fileURLToPath } from 'node:url';

const u32 = (n) => {
  const out = [];
  do {
    let byte = n & 0x7f;
    n >>>= 7;
    if (n) byte |= 0x80;
    out.push(byte);
  } while (n);
  return out;
};
const str = (s) => {
  const b = [...Buffer.from(s)];
  return [...u32(b.length), ...b];
};
const section = (id, bytes) => [id, ...u32(bytes.length), ...bytes];

function storedZip(entries) {
  const locals = [];
  const central = [];
  let offset = 0;
  const word = (n) => { const b = Buffer.alloc(2); b.writeUInt16LE(n); return b; };
  const dword = (n) => { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; };
  for (const [entryName, value] of entries) {
    const name = Buffer.from(entryName);
    const data = Buffer.from(value);
    const local = Buffer.concat([
      dword(0x04034b50), word(20), word(0), word(0), word(0), word(0),
      dword(0), dword(data.length), dword(data.length), word(name.length), word(0), name, data,
    ]);
    locals.push(local);
    central.push(Buffer.concat([
      dword(0x02014b50), word(20), word(20), word(0), word(0), word(0), word(0),
      dword(0), dword(data.length), dword(data.length), word(name.length), word(0), word(0),
      word(0), word(0), dword(0), dword(offset), name,
    ]));
    offset += local.length;
  }
  const directory = Buffer.concat(central);
  return Buffer.concat([
    ...locals, directory, dword(0x06054b50), word(0), word(0),
    word(entries.length), word(entries.length), dword(directory.length), dword(offset), word(0),
  ]);
}

function minimalGuest() {
  const types = [
    3,
    0x60, 0, 1, 0x7f,
    0x60, 1, 0x7f, 1, 0x7f,
    0x60, 1, 0x7e, 0,
  ];
  const functions = [3, 0, 1, 2];
  const exports = [
    3,
    ...str('ab_abi_version'), 0, 0,
    ...str('ab_init'), 0, 1,
    ...str('ab_tick'), 0, 2,
  ];
  const bodies = [
    3,
    4, 0, 0x41, 1, 0x0b,
    4, 0, 0x41, 0, 0x0b,
    2, 0, 0x0b,
  ];
  return Buffer.from([
    0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
    ...section(1, types),
    ...section(3, functions),
    ...section(7, exports),
    ...section(10, bodies),
  ]);
}

function directMemoryReaderGuest() {
  const types = [1, 0x60, 1, 0x7f, 1, 0x7f];
  const imports = [1, ...str('ab_core'), ...str('memory'), 2, 0, 1];
  const functions = [1, 0];
  const exports = [1, ...str('read_u8'), 0, 0];
  const bodies = [1, 7, 0, 0x20, 0, 0x2d, 0, 0, 0x0b];
  return Buffer.from([
    0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
    ...section(1, types), ...section(2, imports), ...section(3, functions),
    ...section(7, exports), ...section(10, bodies),
  ]);
}

function manifestFor(rom, extra = {}) {
  return {
    format: 'active-bezel',
    formatVersion: 1,
    id: 'org.test.diagnostic',
    name: 'Diagnostic',
    version: '1.0.0',
    entry: 'main.wasm',
    runtime: { abi: 'active-bezel-1', renderer: 'cpu-rgba-v1', extensions: [] },
    games: [{ platform: 'nes', sha256: crypto.createHash('sha256').update(rom).digest('hex') }],
    settings: [{ key: 'map', type: 'boolean', default: true }],
    ...extra,
  };
}

test('manifest validation rejects malformed settings and accepts v1', () => {
  const rom = Buffer.from([1, 2, 3]);
  assert.equal(validateManifest(manifestFor(rom)).runtime.abi, 'active-bezel-1');
  assert.throws(
    () => validateManifest(manifestFor(rom, { settings: [{ key: '../bad', type: 'boolean' }] })),
    /invalid key/,
  );
});

test('package loader rejects malformed archives, traversal, missing entries and unsupported ABI', async (t) => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-invalid-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const malformed = path.join(dir, 'malformed.ab');
  await fs.writeFile(malformed, 'not a zip');
  await assert.rejects(() => ActiveBezelPackage.open(malformed));

  const traversal = path.join(dir, 'traversal.ab');
  await fs.writeFile(traversal, storedZip([['../escape', 'bad']]));
  await assert.rejects(() => ActiveBezelPackage.open(traversal), /unsafe package entry|invalid relative path/);

  const missing = path.join(dir, 'missing');
  await fs.mkdir(missing);
  const manifest = manifestFor(Buffer.from([1]));
  await fs.writeFile(path.join(missing, 'manifest.json'), JSON.stringify(manifest));
  await assert.rejects(() => ActiveBezelPackage.open(missing), /missing main.wasm/);

  assert.throws(
    () => validateManifest({ ...manifest, runtime: { ...manifest.runtime, abi: 'active-bezel-99' } }),
    /active-bezel-1/,
  );
});

test('configuration normalizes every v1 type, actions, migration and defaults', () => {
  const schema = [
    { key: 'enabled', type: 'boolean', default: true },
    { key: 'count', type: 'integer', default: 2, min: 0, max: 5 },
    { key: 'opacity', type: 'float', default: 0.5, min: 0, max: 1 },
    { key: 'scale', type: 'number', default: 1 },
    { key: 'side', type: 'choice', choices: ['left', 'right'], default: 'right' },
    { key: 'tint', type: 'color', default: '#102030' },
    { key: 'reveal', type: 'action' },
  ];
  const config = new ActiveBezelConfig(schema, {
    enabled: 0, count: 99, opacity: '0.75', side: 'removed-old-value', tint: 'bad',
  });
  assert.deepEqual(config.values, {
    enabled: false, count: 5, opacity: 0.75, scale: 1,
    side: 'right', tint: '#102030', reveal: 0,
  });
  assert.equal(config.set('reveal'), 1);
  assert.equal(config.set('reveal'), 2);
  assert.equal(config.set('count', -99), 0);
  assert.throws(() => config.set('unknown', true), /unknown Active Bezel setting/);
});

test('matching distinguishes exact, compatible, forced and none', () => {
  const rom = Buffer.from([1, 2, 3, 4]);
  const manifest = manifestFor(rom);
  assert.equal(matchActiveBezel(manifest, rom, 'nes').level, 'exact');
  const other = Buffer.from([1, 2, 9, 4]);
  manifest.games = [];
  manifest.compatible = [{ platform: 'nes', size: 4, signatures: [{ offset: 0, bytes: '0102' }] }];
  assert.equal(matchActiveBezel(manifest, other, 'nes').level, 'compatible');
  assert.equal(matchActiveBezel({ ...manifest, compatible: [] }, other, 'nes').level, 'none');
  assert.equal(matchActiveBezel({ ...manifest, compatible: [] }, other, 'nes', { force: true }).level, 'forced');
});

test('two WASM modules can share the exact core memory object without copying', async () => {
  const coreMemory = new WebAssembly.Memory({ initial: 1 });
  const coreView = new Uint8Array(coreMemory.buffer);
  coreView[0x234] = 0xa7;
  const { instance } = await WebAssembly.instantiate(directMemoryReaderGuest(), {
    ab_core: { memory: coreMemory },
  });
  assert.equal(instance.exports.read_u8(0x234), 0xa7);
  coreView[0x234] = 0x5c;
  assert.equal(instance.exports.read_u8(0x234), 0x5c);
});

test('directory package loads and runtime composes a complete 16:9 frame', async (t) => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-test-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const rom = Buffer.from([1, 2, 3, 4]);
  await fs.writeFile(path.join(dir, 'manifest.json'), JSON.stringify(manifestFor(rom)));
  await fs.writeFile(path.join(dir, 'main.wasm'), minimalGuest());
  const pkg = await ActiveBezelPackage.open(dir);
  assert.equal(pkg.manifest.name, 'Diagnostic');

  const heap = new Uint8Array(65536);
  const host = {
    core: {
      HEAPU8: heap,
      _retro_get_memory_data: (id) => id === 2 ? 1024 : 0,
      _retro_get_memory_size: (id) => id === 2 ? 2048 : 0,
    },
  };
  const runtime = await ActiveBezelRuntime.create({
    packagePath: dir, host, romBytes: rom, platform: 'nes',
    outputWidth: 320, outputHeight: 180,
  });
  assert.equal(runtime.status().match.level, 'exact');
  assert.equal(runtime.setConfig('map', false), false);
  runtime.event(AB_EVENT.RESET);
  const game = new Uint8Array(4 * 4 * 4).fill(255);
  const frame = runtime.processFrame(game, 4, 4, 1);
  assert.deepEqual([frame.width, frame.height, frame.rgba.length], [320, 180, 320 * 180 * 4]);
  assert.equal(runtime.status().stats.ticks, 1);
});

test('command compositor draws alpha rectangles, triangles, text and clipping', () => {
  const compositor = new ActiveBezelCompositor({ outputWidth: 160, outputHeight: 90 });
  compositor.clear(0x000000ff);
  compositor.fillRect(0, 0, 960, 1080, 0xff0000ff);
  compositor.scissor(960, 0, 960, 540);
  compositor.fillRect(960, 0, 960, 1080, 0x00ff00ff);
  compositor.resetScissor();
  compositor.triangle(960, 540, 1920, 540, 1920, 1080, 0x0000ffff);
  compositor.text('AB1', 1000, 100, 50, 0xffffffff);
  const frame = compositor.compose(new Uint8Array(4), 1, 1);
  const at = (x, y) => [...frame.rgba.subarray((y * 160 + x) * 4, (y * 160 + x) * 4 + 4)];
  assert.deepEqual(at(10, 80), [255, 0, 0, 255]);
  assert.deepEqual(at(100, 10), [0, 255, 0, 255]);
  assert.deepEqual(at(100, 80), [0, 0, 0, 255]);
  assert.deepEqual(at(150, 80), [0, 0, 255, 255]);
  assert.ok(frame.rgba.some((value, i) => value === 255 && i % 4 < 3));
});

test('command compositor rejects an unbounded guest command stream', () => {
  const compositor = new ActiveBezelCompositor({
    outputWidth: 16, outputHeight: 9, maxCommands: 2,
  });
  compositor.fillRect(0, 0, 1, 1, 0xffffffff);
  compositor.fillRect(1, 0, 1, 1, 0xffffffff);
  assert.throws(
    () => compositor.fillRect(2, 0, 1, 1, 0xffffffff),
    /command limit exceeded/,
  );
});

test('draw_texture_rect blits a SUB-RECTANGLE of an atlas', () => {
  // The reason this import exists: a guest with a tilesheet must be able to
  // draw one entry per command. Without it the only way to draw real pixels is
  // a command per pixel, which blows the 16k command limit on any busy scene.
  const c = new ActiveBezelCompositor({ outputWidth: 64, outputHeight: 64 });
  // 2x1 atlas: left pixel red, right pixel blue.
  const atlas = new Uint8ClampedArray([255, 0, 0, 255, 0, 0, 255, 255]);
  const tex = c.createTexture(atlas, 2, 1);

  // Draw ONLY the right half (blue) across the whole output.
  c.reset();
  c.drawTexture(tex, 0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT, 1, 0, 1, 1);
  const out = c.compose(new Uint8ClampedArray(4), 1, 1).rgba;
  assert.deepEqual([out[0], out[1], out[2]], [0, 0, 255], 'sub-rect selected the blue texel');

  // The 5-arg form must still draw the WHOLE texture -- existing packages
  // depend on it, so the new parameters have to be strictly additive.
  c.reset();
  c.drawTexture(tex, 0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT);
  const whole = c.compose(new Uint8ClampedArray(4), 1, 1).rgba;
  assert.deepEqual([whole[0], whole[1], whole[2]], [255, 0, 0], 'no source rect still starts at the left texel');
});

test('a guest can READ the game frame it is compositing', async (t) => {
  // Why this exists: a package that reconstructs world graphics has to match
  // the emulator's own colours. Without a read path it can only convert
  // palette indices through its own table, and cores disagree on that decode --
  // which put a visibly different shade of sky on each side of the seam in a
  // real package before this was added.
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-gamepx-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const rom = Buffer.from([9, 9, 9, 9]);
  await fs.writeFile(path.join(dir, 'manifest.json'), JSON.stringify(manifestFor(rom)));
  await fs.writeFile(path.join(dir, 'main.wasm'), minimalGuest());

  const runtime = await ActiveBezelRuntime.create({
    packagePath: dir, host: {}, romBytes: rom, platform: 'nes',
  });

  // A 2x1 game frame: left pixel opaque red, right pixel opaque green.
  const frame = new Uint8ClampedArray([255, 0, 0, 255, 0, 255, 0, 255]);
  runtime.processFrame(frame, 2, 1, 0);

  // Call the host imports exactly as the guest would.
  const host = runtime._hostImports;
  assert.equal(host.game_width(), 2);
  assert.equal(host.game_height(), 1);
  assert.equal(host.game_pixel(0, 0) >>> 0, 0xff0000ff, 'left pixel is opaque red');
  assert.equal(host.game_pixel(1, 0) >>> 0, 0x00ff00ff, 'right pixel is opaque green');
  // Out of bounds reads 0 rather than trapping or leaking adjacent memory.
  assert.equal(host.game_pixel(2, 0), 0, 'x past the edge');
  assert.equal(host.game_pixel(0, 5), 0, 'y past the edge');
  assert.equal(host.game_pixel(-1, 0), 0, 'negative coordinates');
});

test('OpenGL command compositor matches CPU reference and releases resources', (t) => {
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 160, outputHeight: 90 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const cpu = new ActiveBezelCompositor({ outputWidth: 160, outputHeight: 90 });
  const game = new Uint8Array(16 * 9 * 4);
  for (let i = 0; i < game.length; i += 4) {
    game[i] = (i / 4) & 255;
    game[i + 1] = 100;
    game[i + 2] = 200;
    game[i + 3] = 255;
  }
  for (const compositor of [cpu, gpu]) {
    compositor.clear(0x101020ff);
    compositor.drawGame(240, 0, 1440, 1080);
    compositor.fillRect(0, 0, 240, 1080, 0xff000080);
    compositor.triangle(1600, 0, 1920, 540, 1600, 1080, 0x00ff00ff);
    compositor.scissor(1400, 0, 200, 200);
    compositor.fillRect(1300, 0, 400, 400, 0xffff00ff);
    compositor.resetScissor();
    const texture = compositor.createTexture(new Uint8Array([
      255, 255, 255, 255, 0, 0, 0, 255,
      0, 0, 0, 255, 255, 255, 255, 255,
    ]), 2, 2);
    compositor.drawTexture(texture, 1700, 800, 120, 120);
  }
  const expected = cpu.compose(game, 16, 9).rgba;
  const actual = gpu.compose(game, 16, 9).rgba;
  let error = 0;
  let samples = 0;
  for (let i = 0; i < expected.length; i++) {
    if (i % 4 === 3) continue;
    error += Math.abs(expected[i] - actual[i]);
    samples++;
  }
  // Sub-byte mean tolerance permits rasterizer edge ownership differences.
  assert.ok(error / samples < 0.3, `mean channel error ${error / samples}`);
  gpu.destroy();
  assert.equal(gpu.gpuReady, false);
});

test('GPU renders text as geometry, pixel-identical to the CPU reference', (t) => {
  // Text used to be handled by re-composing the WHOLE scene on a second CPU
  // compositor and blending it over the GL readback: +4.4 ms/frame measured at
  // 1080p, a 2.2x penalty for a single string. It is now batched geometry.
  // This asserts the two backends still agree EXACTLY -- the glyph cells must
  // land on the same device-pixel grid, which GL does not do by default
  // (it fills only pixels whose centre is covered).
  const opts = { outputWidth: 320, outputHeight: 180 };
  const gpu = ActiveBezelGpuCompositor.create(opts);
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const cpu = new ActiveBezelCompositor(opts);
  const game = new Uint8Array(4);
  const draw = (c) => {
    c.reset();
    c.clear(0x000000ff);
    c.text('ABC 019 XYZ', 100, 200, 60, 0xff8800ff);
    return c.compose(game, 1, 1).rgba;
  };
  const a = draw(cpu);
  const b = draw(gpu);
  let lit = 0;
  for (let i = 0; i < a.length; i += 4) if (a[i] > 100) lit++;
  assert.ok(lit > 100, 'control: the text actually drew something on the CPU reference');
  let diff = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) diff++;
  assert.equal(diff, 0, 'CPU and GPU text must be pixel-identical');
  gpu.destroy();
});

test('transforms and mesh batches match between CPU and GPU', (t) => {
  // Transforms are applied at the single _push chokepoint, so BOTH backends
  // receive already-transformed commands and cannot disagree about what a
  // rotation means. A rotated rect is no longer a rect, so it is emitted as
  // two triangles rather than silently losing the rotation.
  const opts = { outputWidth: 200, outputHeight: 120 };
  const gpu = ActiveBezelGpuCompositor.create(opts);
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const cpu = new ActiveBezelCompositor(opts);
  const game = new Uint8Array(4);
  const scene = (c) => {
    c.reset();
    c.clear(0x000000ff);
    c.fillRect(100, 100, 400, 300, 0xff0000ff);
    c.pushTransform(); c.translate(600, 100); c.scale(2, 1);
    c.fillRect(0, 0, 200, 200, 0x00ff00ff);
    c.popTransform();
    c.pushTransform(); c.translate(1200, 500); c.rotate(Math.PI / 6);
    c.fillRect(-100, -100, 200, 200, 0x0000ffff);
    c.popTransform();
    c.mesh([
      { x: 200, y: 600, rgba: 0xff0000ff },
      { x: 600, y: 600, rgba: 0x00ff00ff },
      { x: 400, y: 950, rgba: 0x0000ffff },
    ]);
    // textured mesh with a vertex tint: one white texture, coloured per draw.
    // Both backends must modulate identically.
    const white = c.createTexture(new Uint8Array(16).fill(255), 2, 2);
    c.mesh([
      { x: 1000, y: 600, u: 0, v: 0, rgba: 0xff8000ff },
      { x: 1400, y: 600, u: 1, v: 0, rgba: 0xff8000ff },
      { x: 1200, y: 950, u: 0.5, v: 1, rgba: 0xff8000ff },
    ], white);
    // a ROTATED texture must land at its transformed position on both
    // backends (it becomes a textured mesh; it used to render axis-aligned
    // at the untransformed origin).
    const mark = c.createTexture(new Uint8Array([
      255, 0, 255, 255, 255, 0, 255, 255,
      255, 0, 255, 255, 255, 0, 255, 255,
    ]), 2, 2);
    c.pushTransform(); c.translate(1650, 200); c.rotate(Math.PI / 4);
    c.drawTexture(mark, -80, -80, 160, 160);
    c.popTransform();
    return c.compose(game, 1, 1).rgba;
  };
  const a = scene(cpu);
  const b = scene(gpu);
  let lit = 0;
  for (let i = 0; i < a.length; i += 4) if (a[i] || a[i + 1] || a[i + 2]) lit++;
  assert.ok(lit > 1000, 'control: the scene actually drew something');
  // Gouraud interpolation differs by at most 1/255 (GPU interpolates in float,
  // the CPU rasteriser rounds). Anything larger is a real geometry mismatch.
  let worst = 0;
  for (let i = 0; i < a.length; i++) worst = Math.max(worst, Math.abs(a[i] - b[i]));
  assert.ok(worst <= 1, `CPU/GPU differ by ${worst}, expected <= 1 (rounding only)`);
  // the rotated texture's magenta must appear AT the transformed position
  const cx = Math.round(1650 / 1920 * 200), cy = Math.round(200 / 1080 * 120);
  const o = (cy * 200 + cx) * 4;
  assert.deepEqual([a[o], a[o + 1], a[o + 2]], [255, 0, 255], 'rotated texture centre (CPU)');
  assert.deepEqual([b[o], b[o + 1], b[o + 2]], [255, 0, 255], 'rotated texture centre (GPU)');
  gpu.destroy();
});

test('GPU nearest sampling picks the same texels as the CPU blitter', (t) => {
  // The real SMB/Zanac geometry: a 256x224 core frame scaled to 1411x1080.
  // Fragment-center sampling picked a later texel than the CPU\'s left-edge
  // floor() wherever a texel boundary fell inside an output pixel -- 276k
  // differing pixels on this checkerboard before the UV half-pixel shift.
  // Zero tolerance: any regression here is a real sampling divergence.
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 1920, outputHeight: 1080 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const cpu = new ActiveBezelCompositor({ outputWidth: 1920, outputHeight: 1080 });
  const GW = 256, GH = 224;
  const game = new Uint8Array(GW * GH * 4);
  for (let y = 0; y < GH; y++) for (let x = 0; x < GW; x++) {
    const o = (y * GW + x) * 4;
    game[o] = ((x ^ y) & 1) ? 255 : 0;
    game[o + 1] = (y & 1) ? 200 : 40;
    game[o + 2] = (x & 1) ? 180 : 60;
    game[o + 3] = 255;
  }
  const scene = (c) => {
    c.reset(); c.clear(0x101020ff);
    c.drawGame(0, 0, 1411, 1080);
    return c.compose(game, GW, GH).rgba;
  };
  const a = scene(cpu);
  const b = scene(gpu);
  let lit = 0;
  for (let i = 0; i < a.length; i += 4) if (a[i] || a[i + 1] || a[i + 2]) lit++;
  assert.ok(lit > 100_000, 'control: the game frame actually drew');
  let diff = 0;
  for (let i = 0; i < a.length; i += 4) {
    if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) diff++;
  }
  assert.equal(diff, 0, `CPU and GPU picked different texels for ${diff} pixels`);
  gpu.destroy();
});

test('the transform stack restores exactly', () => {
  const c = new ActiveBezelCompositor({ outputWidth: 32, outputHeight: 18 });
  c.reset();
  c.pushTransform();
  c.translate(100, 50);
  c.rotate(1.1);
  c.scale(3, 4);
  c.popTransform();
  c.fillRect(10, 20, 30, 40, 0xffffffff);
  const cmd = c.commands[c.commands.length - 1];
  assert.deepEqual([cmd.x, cmd.y, cmd.w, cmd.h], [10, 20, 30, 40],
    'after pop, geometry must be untransformed');
});

test('a long stall is clamped, and elapsed stays consistent with the deltas', async (t) => {
  // The scenario: a bezel is ticked from a playtest window at ~60fps, but a
  // host may also step hundreds of emulator frames at once, or pause while a
  // window is covered. Real gaps between ticks can be seconds. Reporting them
  // raw makes any delta-driven animation lurch, and leaves elapsed_ms
  // disagreeing with the sum of the deltas.
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-time-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const rom = Buffer.from([3, 1, 4, 1]);
  await fs.writeFile(path.join(dir, 'manifest.json'), JSON.stringify(manifestFor(rom)));
  await fs.writeFile(path.join(dir, 'main.wasm'), minimalGuest());
  const runtime = await ActiveBezelRuntime.create({
    packagePath: dir, host: {}, romBytes: rom, platform: 'nes',
  });
  const frame = new Uint8ClampedArray(4);
  const host = runtime._hostImports;

  runtime.processFrame(frame, 1, 1, 0);
  const t0 = host.time_elapsed_ms();

  // Simulate a long stall by rewinding the runtime's own clock references,
  // which is what a real multi-second pause looks like from inside.
  runtime._lastTickMs -= 4000;
  runtime._tickStartMs -= 4000;
  runtime.processFrame(frame, 1, 1, 1);

  const delta = host.time_delta_ms();
  const elapsed = host.time_elapsed_ms();
  assert.ok(delta <= MAX_DELTA_MS, `delta ${delta} must be clamped to ${MAX_DELTA_MS}`);
  // The epoch is advanced by whatever was clamped away, so elapsed must NOT
  // have jumped the full 4 seconds either.
  assert.ok(elapsed - t0 <= MAX_DELTA_MS + 50,
    `elapsed jumped ${elapsed - t0}ms; it must track the clamped delta, not wall clock`);
});

test('canonical region catalog uses stable unique ids and names', () => {
  const ids = new Set(CORE_REGIONS.map((region) => region.id));
  const names = new Set(CORE_REGIONS.map((region) => region.name));
  assert.equal(ids.size, CORE_REGIONS.length);
  assert.equal(names.size, CORE_REGIONS.length);
  assert.ok(names.has('nes_palette'));
  assert.ok(names.has('gb_vram'));
  assert.ok(names.has('gba_oam'));
});

test('machine-readable ABI schema covers the runtime contract', async () => {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const abi = JSON.parse(await fs.readFile(
    path.resolve(here, '../sdk/abi.json'), 'utf8',
  ));
  assert.equal(abi.version, 1);
  for (const name of ['ab_abi_version', 'ab_init', 'ab_tick']) {
    assert.equal(abi.guestExports[name].required, true);
  }
  for (const name of [
    'input_state', 'region_generation', 'region_read_u8', 'region_write_u8',
    'command_draw_game', 'command_draw_texture',
  ]) assert.ok(abi.hostImports[name], `ABI import ${name}`);
});

test('every scaffold abtool ships is a valid, compilable package', async () => {
  /* These are what `abtool scaffold` hands a new author, so a broken one is
   * the worst possible first impression. Checked as PACKAGES -- manifest
   * parses, declared entry exists, and the wasm actually compiles. */
  const here = path.dirname(fileURLToPath(import.meta.url));
  for (const lang of ['lua', 'js', 'python', 'ruby', 'c']) {
    const dir = path.resolve(here, `../runtimes/${lang}/start`);
    const pkg = await ActiveBezelPackage.open(dir);
    assert.ok(await WebAssembly.compile(pkg.read(pkg.manifest.entry)),
      `${lang} scaffold entry must be valid wasm`);
  }
});

test('a host may transfer the composed frame away between frames', (t) => {
  // retroemu ships the composite to its video worker with a transfer list,
  // which DETACHES the backing ArrayBuffer. The next compose must reallocate
  // rather than trap with "set on a detached ArrayBuffer".
  const check = (c) => {
    c.reset(); c.clear(0x336699ff);
    const first = c.compose(new Uint8Array(4), 1, 1);
    assert.ok(first.rgba.length > 0);
    structuredClone(first.rgba.buffer, { transfer: [first.rgba.buffer] }); // detach
    assert.equal(first.rgba.buffer.byteLength, 0, 'control: buffer must be detached');
    c.reset(); c.clear(0x996633ff);
    const second = c.compose(new Uint8Array(4), 1, 1);
    assert.deepEqual([...second.rgba.subarray(0, 3)], [0x99, 0x66, 0x33]);
  };
  check(new ActiveBezelCompositor({ outputWidth: 32, outputHeight: 18 }));
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 32, outputHeight: 18 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  check(gpu);
  gpu.destroy();
});

test('skew and perspective quads agree between CPU and GPU', (t) => {
  // Two things a bezel needs for a surface that is not axis-aligned:
  //   skew()  -- a shear in the transform stack, so a rect leans
  //   quad()  -- four arbitrary corners with PERSPECTIVE-CORRECT texturing,
  //              which is the difference between a tilt that reads as a
  //              receding plane and one that warps like a PS1 polygon
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 320, outputHeight: 180 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const cpu = new ActiveBezelCompositor({ outputWidth: 320, outputHeight: 180 });

  const N = 16;
  const tex = new Uint8Array(N * N * 4);
  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      const o = (y * N + x) * 4;
      const on = ((x ^ y) & 1) === 1;
      tex[o] = on ? 255 : 20; tex[o + 1] = on ? 180 : 20;
      tex[o + 2] = on ? 60 : 20; tex[o + 3] = 255;
    }
  }
  // a trapezoid: narrow far edge on top, wide near edge at the bottom
  const corners = [{ x: 760, y: 200 }, { x: 1160, y: 200 },
    { x: 1720, y: 900 }, { x: 200, y: 900 }];

  const scene = (c) => {
    c.reset(); c.clear(0x000000ff);
    const handle = c.createTexture(tex, N, N);
    c.quad(corners, handle);
    c.pushTransform(); c.translate(300, 950); c.skew(Math.PI / 6, 0);
    c.fillRect(0, 0, 400, 100, 0x40a0ffff);
    c.popTransform();
    return c.compose(new Uint8Array(4), 1, 1).rgba;
  };
  const a = scene(cpu);
  const b = scene(gpu);

  let lit = 0;
  for (let i = 0; i < a.length; i += 4) if (a[i] + a[i + 1] + a[i + 2] > 90) lit++;
  assert.ok(lit > 5000, `control: the shapes must actually draw (lit ${lit})`);

  let big = 0;
  for (let i = 0; i < a.length; i += 4) {
    const d = Math.max(Math.abs(a[i] - b[i]), Math.abs(a[i + 1] - b[i + 1]),
      Math.abs(a[i + 2] - b[i + 2]));
    if (d > 40) big++;
  }
  // only edge-coverage pixels may disagree; a wrong divisor would repaint
  // the whole quad and blow past this by orders of magnitude
  assert.ok(big < 60, `CPU/GPU differ materially on ${big} px`);
  gpu.destroy();
});

test('quad() foreshortens the texture, mesh() does not', (t) => {
  // The proof that the perspective divisor is real: with the SAME four
  // corners, quad() must compress the texture toward the far edge while a
  // hand-built affine mesh spaces it evenly.
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 320, outputHeight: 180 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const N = 16;
  const tex = new Uint8Array(N * N * 4);
  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      const o = (y * N + x) * 4;
      const on = (y % 2) === 0;
      tex[o] = on ? 255 : 20; tex[o + 1] = on ? 255 : 20;
      tex[o + 2] = on ? 255 : 20; tex[o + 3] = 255;
    }
  }
  const TL = { x: 760, y: 200 }, TR = { x: 1160, y: 200 };
  const BR = { x: 1720, y: 900 }, BL = { x: 200, y: 900 };

  // measure stripe band heights down the centre column, far -> near
  const bands = (rgba) => {
    const cx = Math.round(960 / 1920 * 320);
    const out = [];
    let prev = null, start = 0;
    for (let y = Math.round(200 / 1080 * 180); y < Math.round(900 / 1080 * 180); y++) {
      const on = rgba[(y * 320 + cx) * 4] > 128;
      if (prev === null) { prev = on; start = y; continue; }
      if (on !== prev) { out.push(y - start); start = y; prev = on; }
    }
    return out;
  };
  const split = (b) => {
    const half = Math.floor(b.length / 2);
    const far = b.slice(0, half).reduce((x, y) => x + y, 0);
    const near = b.slice(half).reduce((x, y) => x + y, 0);
    return near > 0 ? far / near : 0;
  };

  gpu.reset(); gpu.clear(0x000000ff);
  const h1 = gpu.createTexture(tex, N, N);
  gpu.quad([TL, TR, BR, BL], h1);
  const perspective = split(bands(gpu.compose(new Uint8Array(4), 1, 1).rgba));

  gpu.reset(); gpu.clear(0x000000ff);
  const h2 = gpu.createTexture(tex, N, N);
  const w = 0xffffffff;
  gpu.mesh([
    { ...TL, u: 0, v: 0, rgba: w }, { ...TR, u: 1, v: 0, rgba: w }, { ...BL, u: 0, v: 1, rgba: w },
    { ...BL, u: 0, v: 1, rgba: w }, { ...TR, u: 1, v: 0, rgba: w }, { ...BR, u: 1, v: 1, rgba: w },
  ], h2);
  const affine = split(bands(gpu.compose(new Uint8Array(4), 1, 1).rgba));

  assert.ok(perspective > 1.8,
    `quad() must foreshorten (far/near band ratio ${perspective.toFixed(2)})`);
  assert.ok(affine < 1.4,
    `mesh() must stay affine (far/near band ratio ${affine.toFixed(2)})`);
  gpu.destroy();
});

test('offscreen surfaces: render, filter, reuse as a texture', (t) => {
  // A surface is a guest-allocated render target that survives across
  // frames. The point is ORDER: filter FIRST, flat, at the source's own
  // scale, then map the result through whatever geometry you like. The
  // scene-wide effect pass cannot do that -- it runs last, over everything.
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 160, outputHeight: 90 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');

  const surface = gpu.surfaceCreate(64, 64);
  assert.ok(surface > 0, 'surfaceCreate must return a handle');

  // a red game frame, inverted by the filter into cyan
  const game = new Uint8Array(8 * 8 * 4);
  for (let i = 0; i < game.length; i += 4) {
    game[i] = 200; game[i + 1] = 40; game[i + 2] = 40; game[i + 3] = 255;
  }
  const invert = `#version 300 es
    precision mediump float;
    in vec2 v_uv; out vec4 out_color;
    uniform sampler2D u_texture;
    void main() { vec4 c = texture(u_texture, v_uv); out_color = vec4(1.0 - c.rgb, 1.0); }`;

  gpu.reset(); gpu.clear(0x000000ff);
  assert.equal(
    gpu.surfaceFilter(ActiveBezelCompositor.GAME_TEXTURE, surface, invert, game, 8, 8), 1,
    'surfaceFilter must succeed on the GPU path');
  gpu.drawTexture(surface, 0, 0, 960, 1080);
  const out = gpu.compose(game, 8, 8).rgba;

  const at = (x, y) => [...out.subarray((y * 160 + x) * 4, (y * 160 + x) * 4 + 3)];
  const [r, g, b] = at(40, 45);
  assert.ok(r < 90 && g > 180 && b > 180,
    `the filtered surface must draw INVERTED (got ${r},${g},${b})`);

  // a broken shader must fail, not silently pass the picture through
  assert.equal(gpu.surfaceFilter(ActiveBezelCompositor.GAME_TEXTURE, surface,
    'void main() { this is not glsl', game, 8, 8), 0,
    'a bad shader must report failure');
  gpu.destroy();
});

test('a surface is not vertically flipped when drawn', (t) => {
  // Surfaces render into an FBO (rows bottom-up) but every consumer samples
  // top-down. The first version of this came back upside down AND mirrored
  // on screen, so the orientation gets an asymmetric test rather than a
  // symmetric shader that would hide it.
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 160, outputHeight: 90 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  const surface = gpu.surfaceCreate(32, 32);

  // source: TOP half red, BOTTOM half blue
  const src = new Uint8Array(8 * 8 * 4);
  for (let y = 0; y < 8; y++) {
    for (let x = 0; x < 8; x++) {
      const o = (y * 8 + x) * 4;
      const top = y < 4;
      src[o] = top ? 220 : 20; src[o + 1] = 20;
      src[o + 2] = top ? 20 : 220; src[o + 3] = 255;
    }
  }
  const passthrough = `#version 300 es
    precision mediump float;
    in vec2 v_uv; out vec4 out_color;
    uniform sampler2D u_texture;
    void main() { out_color = texture(u_texture, v_uv); }`;

  gpu.reset(); gpu.clear(0x000000ff);
  assert.equal(gpu.surfaceFilter(ActiveBezelCompositor.GAME_TEXTURE, surface,
    passthrough, src, 8, 8), 1);
  gpu.drawTexture(surface, 0, 0, 1920, 1080);
  const out = gpu.compose(src, 8, 8).rgba;
  const at = (x, y) => [...out.subarray((y * 160 + x) * 4, (y * 160 + x) * 4 + 3)];
  const top = at(80, 15), bottom = at(80, 75);
  assert.ok(top[0] > top[2], `top must stay RED (got ${top})`);
  assert.ok(bottom[2] > bottom[0], `bottom must stay BLUE (got ${bottom})`);
  gpu.destroy();
});

test('a picture effect filters the scene without flipping it', (t) => {
  const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 192, outputHeight: 108 });
  if (!gpu) return t.skip('OpenGL ES context unavailable');
  // Asymmetric scene: red band TOP, blue band BOTTOM. A pass-through shader
  // must keep them where they are -- the scene texture is rendered bottom-up,
  // and sampling it top-down showed the whole frame upside down while every
  // symmetric shader (invert, vignette) hid the flip completely.
  const scene = () => {
    gpu.reset(); gpu.clear(0x000000ff);
    gpu.fillRect(0, 0, 1920, 200, 0xff0000ff);
    gpu.fillRect(0, 880, 1920, 200, 0x0000ffff);
  };
  scene();
  assert.equal(gpu.setEffect(`#version 300 es
    precision mediump float;
    in vec2 v_uv; out vec4 out_color;
    uniform sampler2D u_texture;
    void main() { out_color = texture(u_texture, v_uv); }`), 1, 'shader must compile');
  scene();
  const out = gpu.compose(new Uint8Array(4), 1, 1).rgba;
  const px = (x, y) => [...out.subarray((y * 192 + x) * 4, (y * 192 + x) * 4 + 3)];
  assert.deepEqual(px(96, 5), [255, 0, 0], 'top band must stay red');
  assert.deepEqual(px(96, 102), [0, 0, 255], 'bottom band must stay blue');
  // and the effect must actually run: invert turns the red band cyan
  assert.equal(gpu.setEffect(`#version 300 es
    precision mediump float;
    in vec2 v_uv; out vec4 out_color;
    uniform sampler2D u_texture;
    void main() { vec4 c = texture(u_texture, v_uv); out_color = vec4(1.0 - c.rgb, c.a); }`), 1);
  scene();
  const inverted = gpu.compose(new Uint8Array(4), 1, 1).rgba;
  const ipx = (x, y) => [...inverted.subarray((y * 192 + x) * 4, (y * 192 + x) * 4 + 3)];
  assert.deepEqual(ipx(96, 5), [0, 255, 255], 'invert must reach the pixels');
  // a broken shader is refused and the unfiltered picture survives
  assert.equal(gpu.setEffect('void main() { this is not glsl'), 0, 'bad shader must be refused');
  gpu.destroy();
});

/*
 * The prebuilt runtimes: one table, four languages.
 *
 * These wasms are the zero-toolchain authoring path -- a bezel ships the
 * runtime plus a script, and romdev can mint one by copying two files. What
 * matters is that EVERY language reaches the SAME command stream a C guest
 * would, so the tests are written once and parameterised by language.
 *
 * A runtime that has not been built yet is skipped, not failed: the repo
 * ships prebuilt wasms, but a fresh clone may not have run every build.sh.
 */
const RUNTIMES = [
  {
    lang: 'lua', script: 'main.lua', wasm: '../runtimes/lua/main.wasm',
    // exercises: constants, live memory, config, stdlib, PNG, TTF, mesh
    api: `
      local booted = false
      function init() booted = true end
      function tick(frame)
        assert(booted, 'init() must have run first')
        ab.clear(ab.rgb(1, 2, 3))
        ab.fill_rect(10, 20, 100, 50, 0xff0000ff)
        ab.text('lua ' .. frame, 40, 40, 30, 0xffffffff)
        ab.push_transform(); ab.translate(5, 5)
        ab.triangle(0, 0, 50, 0, 0, 50, 0x00ff00ff)
        ab.pop_transform()
        assert(ab.EVENT.ASSETS_RELOADED == 6 and ab.FIT.INTEGER == 3
               and ab.SAMPLE.LINEAR == 1 and ab.BTN.START == 3 and ab.BTN.MASK == 256)
        assert(ab.config_bool('map') == true)
        assert(('%03d'):format(7) == '007')
        local ram = ab.region('system_ram')
        assert(ram ~= nil and ab.read_u8(ram, 0) == 65)
        assert(#ab.read(ram, 0, 4) == 4)
        assert(ab.read_u16(ram, 0) == 0x4141 and ab.read_u32(ram, 0, true) == 0x41414141)
        local badge = ab.image('assets/badge.png')
        assert(badge.texture > 0 and badge.width == 48 and badge.height == 48)
        ab.draw_texture(badge.texture, 0, 0, 96, 96)
        if ab.asset('assets/roboto-medium.ttf') then
          local font = ab.font('assets/roboto-medium.ttf')
          assert(ab.measure(font, 'MMMM', 40) > ab.measure(font, 'iiii', 40))
          ab.print(font, 'Hello', 100, 100, 40, ab.rgb(255, 0, 0))
        end
        ab.mesh({ { x = 0, y = 0, rgba = 0xff0000ff },
                  { x = 9, y = 0, rgba = 0x00ff00ff },
                  { x = 0, y = 9, rgba = 0x0000ffff } })
      end`,
    broken: 'this is not lua at all (',
    throws: 'function tick(frame) error("boom in tick") end',
    panel: 'lua bezel error',
  },
  {
    lang: 'python', script: 'main.py', wasm: '../runtimes/python/main.wasm',
    api: `
booted = False
def init():
    global booted
    booted = True
def tick(frame):
    assert booted, 'init() must have run first'
    ab.clear(ab.rgb(1, 2, 3))
    ab.fill_rect(10, 20, 100, 50, 0xff0000ff)
    ab.text('py ' + str(frame), 40, 40, 30, 0xffffffff)
    ab.push_transform(); ab.translate(5, 5)
    ab.triangle(0, 0, 50, 0, 0, 50, 0x00ff00ff)
    ab.pop_transform()
    assert ab.EVENT['ASSETS_RELOADED'] == 6 and ab.FIT['INTEGER'] == 3
    assert ab.SAMPLE['LINEAR'] == 1 and ab.BTN['START'] == 3 and ab.BTN['MASK'] == 256
    assert ab.config_bool('map') == True
    assert '%03d' % 7 == '007'
    ram = ab.region('system_ram')
    assert ram is not None and ab.read_u8(ram, 0) == 65
    assert len(ab.read(ram, 0, 4)) == 4
    assert ab.read_u16(ram, 0) == 0x4141 and ab.read_u32(ram, 0, True) == 0x41414141
    badge = ab.image('assets/badge.png')
    assert badge['texture'] > 0 and badge['width'] == 48 and badge['height'] == 48
    ab.draw_texture(badge['texture'], 0, 0, 96, 96)
    if ab.asset('assets/roboto-medium.ttf'):
        font = ab.font('assets/roboto-medium.ttf')
        assert ab.measure(font, 'MMMM', 40) > ab.measure(font, 'iiii', 40)
        ab.draw_text(font, 'Hello', 100, 100, 40, ab.rgb(255, 0, 0))
    ab.mesh([{'x': 0, 'y': 0, 'rgba': 0xff0000ff},
             {'x': 9, 'y': 0, 'rgba': 0x00ff00ff},
             {'x': 0, 'y': 9, 'rgba': 0x0000ffff}])
`,
    broken: 'this is not python at all (',
    throws: 'def tick(frame):\n    raise ValueError("boom in tick")\n',
    panel: 'python bezel error',
  },
  {
    lang: 'js', script: 'main.js', wasm: '../runtimes/js/main.wasm',
    api: `
      let booted = false;
      function init() { booted = true; }
      function tick(frame) {
        if (!booted) throw new Error('init() must have run first');
        ab.clear(ab.rgb(1, 2, 3));
        ab.fill_rect(10, 20, 100, 50, 0xff0000ff);
        ab.text('js ' + frame, 40, 40, 30, 0xffffffff);
        ab.push_transform(); ab.translate(5, 5);
        ab.triangle(0, 0, 50, 0, 0, 50, 0x00ff00ff);
        ab.pop_transform();
        if (ab.EVENT.ASSETS_RELOADED !== 6 || ab.FIT.INTEGER !== 3
            || ab.SAMPLE.LINEAR !== 1 || ab.BTN.START !== 3 || ab.BTN.MASK !== 256)
          throw new Error('constants');
        if (ab.config_bool('map') !== true) throw new Error('config');
        const ram = ab.region('system_ram');
        if (ram === null || ab.read_u8(ram, 0) !== 65) throw new Error('ram');
        if (ab.read(ram, 0, 4).length !== 4) throw new Error('bulk read');
        if (ab.read_u16(ram, 0) !== 0x4141 || ab.read_u32(ram, 0, true) !== 0x41414141)
          throw new Error('multibyte');
        const badge = ab.image('assets/badge.png');
        if (!(badge.texture > 0 && badge.width === 48 && badge.height === 48))
          throw new Error('png');
        ab.draw_texture(badge.texture, 0, 0, 96, 96);
        if (ab.asset('assets/roboto-medium.ttf')) {
        const font = ab.font('assets/roboto-medium.ttf');
        if (!(ab.measure(font, 'MMMM', 40) > ab.measure(font, 'iiii', 40)))
          throw new Error('measure');
        ab.print(font, 'Hello', 100, 100, 40, ab.rgb(255, 0, 0));
        }
        ab.mesh([{ x: 0, y: 0, rgba: 0xff0000ff },
                 { x: 9, y: 0, rgba: 0x00ff00ff },
                 { x: 0, y: 9, rgba: 0x0000ffff }]);
      }`,
    broken: 'function tick( {{{ this is not js',
    throws: 'function tick(frame) { throw new Error("boom in tick"); }',
    panel: 'js runtime',
  },
  {
    lang: 'ruby', script: 'main.rb', wasm: '../runtimes/ruby/main.wasm',
    api: `
$booted = false
def init
  $booted = true
end
def tick(frame)
  raise 'init() must have run first' unless $booted
  AB.clear(AB.rgb(1, 2, 3))
  AB.fill_rect(10, 20, 100, 50, 0xff0000ff)
  AB.text('rb ' + frame.to_s, 40, 40, 30, 0xffffffff)
  AB.push_transform; AB.translate(5, 5)
  AB.triangle(0, 0, 50, 0, 0, 50, 0x00ff00ff)
  AB.pop_transform
  raise 'constants' unless AB::EVENT[:ASSETS_RELOADED] == 6 && AB::FIT[:INTEGER] == 3
  raise 'buttons' unless AB::SAMPLE[:LINEAR] == 1 && AB::BTN[:START] == 3 && AB::BTN[:MASK] == 256
  raise 'config' unless AB.config_bool('map') == true
  raise 'format' unless format('%03d', 7) == '007'
  ram = AB.region('system_ram')
  raise 'ram' unless ram && AB.read_u8(ram, 0) == 65
  raise 'bulk' unless AB.read(ram, 0, 4).bytesize == 4
  raise 'multibyte' unless AB.read_u16(ram, 0) == 0x4141 && AB.read_u32(ram, 0, true) == 0x41414141
  badge = AB.image('assets/badge.png')
  raise 'png' unless badge[:texture] > 0 && badge[:width] == 48 && badge[:height] == 48
  AB.draw_texture(badge[:texture], 0, 0, 96, 96)
  if AB.asset('assets/roboto-medium.ttf')
  font = AB.font('assets/roboto-medium.ttf')
  raise 'measure' unless AB.measure(font, 'MMMM', 40) > AB.measure(font, 'iiii', 40)
  AB.draw_text(font, 'Hello', 100, 100, 40, AB.rgb(255, 0, 0))
  end
  AB.mesh([{ x: 0, y: 0, rgba: 0xff0000ff },
           { x: 9, y: 0, rgba: 0x00ff00ff },
           { x: 0, y: 9, rgba: 0x0000ffff }])
end
`,
    broken: 'def tick(frame) this is not ruby (((',
    throws: 'def tick(frame)\n  raise "boom in tick"\nend\n',
    panel: 'ruby bezel error',
  },
];

async function runtimeAvailable(spec) {
  try {
    await fs.access(new URL(spec.wasm, import.meta.url));
    return true;
  } catch {
    return false;
  }
}

/* A package for one runtime: the wasm as entry, the script, and the two
 * assets the API test decodes. */
async function makeRuntimePackage(spec, script, rom, withAssets = true) {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), `active-bezel-${spec.lang}-`));
  await fs.writeFile(path.join(dir, 'manifest.json'), JSON.stringify(manifestFor(rom)));
  await fs.copyFile(new URL(spec.wasm, import.meta.url), path.join(dir, 'main.wasm'));
  await fs.writeFile(path.join(dir, spec.script), script);
  if (withAssets) {
    await fs.mkdir(path.join(dir, 'assets'), { recursive: true });
    /*
     * Copy the example's assets when they are present, and SYNTHESISE a PNG
     * when they are not.
     *
     * The examples asset directories are gitignored on purpose -- binary art must not be
     * redistributed here without clearance -- so a clean checkout (CI, or any
     * contributor) has no badge.png, and a hard copyFile made every runtime
     * test fail there while passing on the author's machine. A generated
     * image exercises the same decode path and is identical everywhere.
     */
    for (const asset of ['badge.png', 'roboto-medium.ttf']) {
      const from = new URL(`../examples/lua-native/assets/${asset}`, import.meta.url);
      try {
        await fs.copyFile(from, path.join(dir, 'assets', asset));
      } catch {
        /* Only the image has a synthetic stand-in: a font cannot be faked, and
         * every scaffold already guards ab.font behind ab.asset(). */
        if (asset.endsWith('.png')) await fs.copyFile(BADGE_PNG, path.join(dir, 'assets', asset));
      }
    }
  }
  return dir;
}

/* No TrueType font is redistributed from this repo, so the TTF-specific
 * assertions are skipped unless one is dropped in by hand. */
const hasExampleFont = existsSync(
  new URL('./fixtures/font.ttf', import.meta.url));

/*
 * A real 48x48 PNG, committed under test/fixtures because the example asset
 * directories are gitignored (binary art is not redistributed from here) and
 * the guest scripts assert width == 48 and height == 48. Decoded by the same
 * decoder the product uses -- no encoder is hand-written for the tests.
 */
const BADGE_PNG = new URL('./fixtures/badge.png', import.meta.url);

function memoryHost(fill = 65) {
  return { core: {
    HEAPU8: new Uint8Array(65536).fill(fill),
    _retro_get_memory_data: (id) => id === 2 ? 1024 : 0,
    _retro_get_memory_size: (id) => id === 2 ? 2048 : 0,
  } };
}

for (const spec of RUNTIMES) {
  test(`the ${spec.lang} runtime runs a script against the full ab API`, async (t) => {
    if (!await runtimeAvailable(spec)) return t.skip(`${spec.lang} runtime not built`);
    const rom = Buffer.from([9, 9, 9]);
    const dir = await makeRuntimePackage(spec, spec.api, rom);
    t.after(() => fs.rm(dir, { recursive: true, force: true }));

    const runtime = await ActiveBezelRuntime.create({
      packagePath: dir, host: memoryHost(), romBytes: rom, platform: 'nes',
      outputWidth: 320, outputHeight: 180,
    });
    const frame = runtime.processFrame(new Uint8Array(4 * 4 * 4).fill(255), 4, 4, 1);
    assert.deepEqual([frame.width, frame.height], [320, 180]);

    const kinds = runtime.compositor.commands.map((c) => c.kind);
    /*
     * `mesh` is emitted by TrueType text, which needs a font -- and fonts are
     * not redistributed from this repo, so a clean checkout has none. The
     * other four kinds are unconditional. See hasExampleFont below for the
     * matching skip on the tint assertion.
     */
    const required = ['rect', 'text', 'triangle', 'texture'];
    if (hasExampleFont) required.push('mesh');
    for (const kind of required) {
      assert.ok(kinds.includes(kind), `${spec.lang}: ${kind} must reach the compositor`);
    }
    const meshes = runtime.compositor.commands.filter((c) => c.kind === 'mesh');
    /*
     * The TTF assertion needs a real font, and fonts are not redistributed
     * here (examples/*_/assets is gitignored), so a clean checkout has none.
     * Assert it when the font is present and say plainly when it is not --
     * silently dropping the check would let a real TTF regression through.
     */
    if (hasExampleFont) {
      assert.ok(meshes.some((m) => m.handle > 0 && m.vertices.length >= 30
        && (m.vertices[0].rgba >>> 0) === 0xff0000ff),
        `${spec.lang}: TTF text must emit a mesh tinted by vertex colour`);
    } else {
      t.diagnostic(`${spec.lang}: no example font on this checkout; TTF mesh check skipped`);
    }
    assert.ok(meshes.some((m) => !m.handle && m.vertices.length === 3),
      `${spec.lang}: an untextured per-vertex mesh must reach the compositor`);

    // a failed assertion inside the script would surface as the error panel
    runtime.processFrame(new Uint8Array(4 * 4 * 4).fill(255), 4, 4, 2);
    const texts = runtime.compositor.commands
      .filter((c) => c.kind === 'text').map((c) => String(c.text));
    assert.ok(!texts.some((s) => s.toLowerCase().includes('error')),
      `${spec.lang}: unexpected error text: ${texts.join(' | ')}`);
  });

  test(`the ${spec.lang} runtime survives a broken script and says so on screen`, async (t) => {
    if (!await runtimeAvailable(spec)) return t.skip(`${spec.lang} runtime not built`);
    const rom = Buffer.from([9, 9, 9]);
    const dir = await makeRuntimePackage(spec, spec.broken, rom, false);
    t.after(() => fs.rm(dir, { recursive: true, force: true }));

    const runtime = await ActiveBezelRuntime.create({
      packagePath: dir, host: memoryHost(0), romBytes: rom, platform: 'nes',
      outputWidth: 320, outputHeight: 180,
    });
    const frame = runtime.processFrame(new Uint8Array(4).fill(128), 1, 1, 1);
    assert.ok(frame.rgba.length > 0, 'a frame still composes');
    const texts = runtime.compositor.commands
      .filter((c) => c.kind === 'text').map((c) => String(c.text));
    assert.ok(texts.some((s) => s.includes(spec.panel)),
      `${spec.lang}: error panel expected, got: ${texts.join(' | ')}`);
  });

  test(`the ${spec.lang} runtime reports a runtime error from tick`, async (t) => {
    if (!await runtimeAvailable(spec)) return t.skip(`${spec.lang} runtime not built`);
    // A script that LOADS but throws mid-frame is the common authoring
    // mistake; the message has to name the failure, not just say "error".
    const rom = Buffer.from([9, 9, 9]);
    const dir = await makeRuntimePackage(spec, spec.throws, rom, false);
    t.after(() => fs.rm(dir, { recursive: true, force: true }));

    const runtime = await ActiveBezelRuntime.create({
      packagePath: dir, host: memoryHost(0), romBytes: rom, platform: 'nes',
      outputWidth: 320, outputHeight: 180,
    });
    runtime.processFrame(new Uint8Array(4).fill(128), 1, 1, 1);
    runtime.processFrame(new Uint8Array(4).fill(128), 1, 1, 2);
    const texts = runtime.compositor.commands
      .filter((c) => c.kind === 'text').map((c) => String(c.text));
    assert.ok(texts.some((s) => s.includes('boom in tick')),
      `${spec.lang}: the panel must carry the real message, got: ${texts.join(' | ')}`);
  });

  test(`the ${spec.lang} scaffold runs as shipped`, async (t) => {
    if (!await runtimeAvailable(spec)) return t.skip(`${spec.lang} runtime not built`);
    // The scaffold is what romdev copies to mint a bezel. If it does not run
    // clean -- both before and after assets exist -- the zero-toolchain path
    // is broken for every new author.
    let scaffold;
    try {
      scaffold = await fs.readFile(
        new URL(`../runtimes/${spec.lang}/${spec.script}`, import.meta.url), 'utf8');
    } catch {
      return t.skip(`${spec.lang} scaffold missing`);
    }
    const rom = Buffer.from([9, 9, 9]);
    const dir = await makeRuntimePackage(spec, scaffold, rom, false);
    t.after(() => fs.rm(dir, { recursive: true, force: true }));

    const runtime = await ActiveBezelRuntime.create({
      packagePath: dir, host: memoryHost(0x5a), romBytes: rom, platform: 'nes',
      outputWidth: 320, outputHeight: 180,
    });
    runtime.processFrame(new Uint8Array(4 * 4 * 4).fill(200), 4, 4, 1);
    let texts = runtime.compositor.commands
      .filter((c) => c.kind === 'text').map((c) => String(c.text));
    assert.ok(!texts.some((s) => s.toLowerCase().includes('error')),
      `${spec.lang} scaffold (no assets): ${texts.join(' | ')}`);
    let kinds = runtime.compositor.commands.map((c) => c.kind);
    for (const kind of ['game', 'rect', 'mesh']) {
      assert.ok(kinds.includes(kind), `${spec.lang} scaffold must emit ${kind}`);
    }

    // now with the assets its guarded branches want
    await fs.mkdir(path.join(dir, 'assets'), { recursive: true });
    /* Same story as makeRuntimePackage: the example assets are gitignored, so
     * a clean checkout has neither. The image is synthesised; the font is not,
     * and every scaffold guards ab.font behind ab.asset() for exactly that. */
    if (hasExampleFont) {
      await fs.copyFile(new URL('./fixtures/font.ttf', import.meta.url),
        path.join(dir, 'assets/font.ttf'));
    }
    await fs.copyFile(BADGE_PNG, path.join(dir, 'assets/logo.png'));
    assert.equal(await runtime.reloadAssets(), true, `${spec.lang}: reloadAssets`);

    runtime.processFrame(new Uint8Array(4 * 4 * 4).fill(200), 4, 4, 2);
    texts = runtime.compositor.commands
      .filter((c) => c.kind === 'text').map((c) => String(c.text));
    assert.ok(!texts.some((s) => s.toLowerCase().includes('error')),
      `${spec.lang} scaffold (with assets): ${texts.join(' | ')}`);
    kinds = runtime.compositor.commands.map((c) => c.kind);
    assert.ok(kinds.includes('texture'), `${spec.lang} scaffold must draw its image`);
    assert.ok(runtime.compositor.commands.some((c) => c.kind === 'mesh' && c.handle > 0),
      `${spec.lang} scaffold must draw TTF text`);
  });

  test(`the ${spec.lang} runtime hot reloads an edited script`, async (t) => {
    if (!await runtimeAvailable(spec)) return t.skip(`${spec.lang} runtime not built`);
    // The whole authoring loop: edit the script on disk, reload, see it.
    // reloadAssets() must RE-OPEN the package -- firing the event alone made
    // the guest re-read bytes the package had already cached.
    const red = { lua: 'function tick(f) ab.fill_rect(0,0,10,10, 0xff0000ff) end',
      python: 'def tick(frame):\n    ab.fill_rect(0,0,10,10, 0xff0000ff)\n',
      js: 'function tick(f){ ab.fill_rect(0,0,10,10, 0xff0000ff); }',
      ruby: 'def tick(frame)\n  AB.fill_rect(0,0,10,10, 0xff0000ff)\nend\n' }[spec.lang];
    const green = red.replace('0xff0000ff', '0x00ff00ff');

    const rom = Buffer.from([9, 9, 9]);
    const dir = await makeRuntimePackage(spec, red, rom, false);
    t.after(() => fs.rm(dir, { recursive: true, force: true }));

    const runtime = await ActiveBezelRuntime.create({
      packagePath: dir, host: memoryHost(0), romBytes: rom, platform: 'nes',
      outputWidth: 64, outputHeight: 36,
    });
    runtime.processFrame(new Uint8Array(4), 1, 1, 1);
    const before = runtime.compositor.commands.find((c) => c.kind === 'rect');
    assert.equal(before?.rgba >>> 0, 0xff0000ff, `${spec.lang}: starts red`);

    await fs.writeFile(path.join(dir, spec.script), green);
    assert.equal(await runtime.reloadAssets(), true, `${spec.lang}: reloadAssets`);

    runtime.processFrame(new Uint8Array(4), 1, 1, 2);
    const after = runtime.compositor.commands.find((c) => c.kind === 'rect');
    assert.equal(after?.rgba >>> 0, 0x00ff00ff,
      `${spec.lang}: must be green after reloading the edited script`);
  });
}


test('runtime remains stable for 10,000 lifecycle ticks', async (t) => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-soak-'));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  const rom = Buffer.from([5, 6, 7, 8]);
  await fs.writeFile(path.join(dir, 'manifest.json'), JSON.stringify(manifestFor(rom, {
    runtime: {
      abi: 'active-bezel-1', renderer: 'cpu-rgba-v1',
      internalResolution: [16, 9], extensions: [],
    },
  })));
  await fs.writeFile(path.join(dir, 'main.wasm'), minimalGuest());
  const heap = new Uint8Array(65536);
  const host = {
    core: {
      HEAPU8: heap,
      _retro_get_memory_data: (id) => id === 2 ? 1024 : 0,
      _retro_get_memory_size: (id) => id === 2 ? 2048 : 0,
    },
  };
  const runtime = await ActiveBezelRuntime.create({
    packagePath: dir, host, romBytes: rom, platform: 'nes',
    outputWidth: 16, outputHeight: 9,
  });
  const game = new Uint8Array(4);
  const outputIdentity = runtime.compositor.output;
  for (let i = 0; i < 10_000; i++) runtime.processFrame(game, 1, 1, i);
  assert.equal(runtime.status().stats.ticks, 10_000);
  assert.equal(runtime.compositor.output, outputIdentity);
  runtime.event(AB_EVENT.STATE_LOADED);
  runtime.event(AB_EVENT.REWIND_JUMP);
  runtime.shutdown();
  assert.equal(runtime.enabled, false);
});
