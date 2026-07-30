import { performance } from 'node:perf_hooks';
import { ActiveBezelPackage } from './Package.js';
import { matchActiveBezel } from './Matcher.js';
import { ActiveBezelConfig } from './Config.js';
import { ActiveBezelRegions } from './Regions.js';
import { ActiveBezelCompositor, LOGICAL_WIDTH, LOGICAL_HEIGHT } from './Compositor.js';
import { ActiveBezelGpuCompositor } from './GpuCompositor.js';

export const AB_EVENT = Object.freeze({
  RESET: 1,
  STATE_LOADED: 2,
  REWIND_JUMP: 3,
  CONFIG_CHANGED: 4,
  DISPLAY_CHANGED: 5,
  ASSETS_RELOADED: 6,
  REGIONS_CHANGED: 7,
});

function readGuestString(runtime, ptr, length) {
  const memory = runtime.instance?.exports?.memory;
  if (!memory || ptr < 0 || length < 0 || ptr + length > memory.buffer.byteLength) return '';
  return new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, length));
}

export class ActiveBezelRuntime {
  static async create(options) {
    const pkg = await ActiveBezelPackage.open(options.packagePath);
    const runtime = new ActiveBezelRuntime({ ...options, pkg });
    await runtime.init();
    return runtime;
  }

  constructor({
    pkg, host, romBytes, platform, config = {}, force = false,
    outputWidth = 1920, outputHeight = 1080, allowGpu = true, inputManager = null,
  }) {
    this.package = pkg;
    this.host = host;
    this.romBytes = Buffer.from(romBytes);
    this.platform = platform;
    this.force = force;
    this.inputManager = inputManager;
    this.config = new ActiveBezelConfig(pkg.manifest.settings, config);
    this.regions = new ActiveBezelRegions(host, this.romBytes);
    this.physicalWidth = outputWidth;
    this.physicalHeight = outputHeight;
    const internal = pkg.manifest.runtime.internalResolution ?? [outputWidth, outputHeight];
    const compositorOptions = {
      outputWidth: internal[0],
      outputHeight: internal[1],
    };
    this.compositor = allowGpu && pkg.manifest.runtime.renderer === 'gpu-command-v1'
      ? (ActiveBezelGpuCompositor.create(compositorOptions)
        ?? new ActiveBezelCompositor(compositorOptions))
      : new ActiveBezelCompositor(compositorOptions);
    this.match = matchActiveBezel(pkg.manifest, this.romBytes, platform, { force });
    this.enabled = false;
    this.error = null;
    this.stats = {
      ticks: 0,
      totalTickMs: 0, maxTickMs: 0, lastTickMs: 0,
      totalComposeMs: 0, maxComposeMs: 0, lastComposeMs: 0,
    };
  }

  async init() {
    if (this.match.level === 'none') throw new Error('Active Bezel does not match this ROM (use force to override)');
    const missing = this.regions.validateRequirements(this.package.manifest.requires);
    if (missing.length) throw new Error(`Active Bezel is missing required regions: ${missing.map((x) => x.region).join(', ')}`);

    if (this.package.manifest.runtime.language === 'lua54-wasmcart') {
      // Loaded on demand: this pulls in the wasmcart Lua runtime, which a
      // wasm-guest package (the common case) has no reason to install.
      let ActiveBezelLuaAdapter;
      try {
        ({ ActiveBezelLuaAdapter } = await import('./LuaAdapter.js'));
      } catch (cause) {
        throw new Error(
          "This Active Bezel declares runtime.language 'lua54-wasmcart', which needs the optional `wasmcart` package. "
          + 'Install it (npm i wasmcart) or repackage the guest as wasm.',
          { cause },
        );
      }
      this.lua = new ActiveBezelLuaAdapter(this.package);
      await this.lua.init();
      this.enabled = true;
      return;
    }

    const imports = {
      ab_host: {
        abi_version: () => 1,
        logical_width: () => LOGICAL_WIDTH,
        logical_height: () => LOGICAL_HEIGHT,
        physical_width: () => this.physicalWidth,
        physical_height: () => this.physicalHeight,
        input_state: (port, device, index, id) =>
          this.inputManager?.getState(port, device, index, id) ?? 0,
        region_generation: () => this.regions.generation,
        region_count: () => this.regions.regions.length,
        region_name_length: (index) => this.regions.regions[index]?.name.length ?? 0,
        region_name_read: (index, dst, capacity) => {
          const name = this.regions.regions[index]?.name;
          const memory = this.instance?.exports?.memory;
          if (!name || !memory) return 0;
          const bytes = new TextEncoder().encode(name);
          const n = Math.min(bytes.length, capacity, Math.max(0, memory.buffer.byteLength - dst));
          new Uint8Array(memory.buffer, dst, n).set(bytes.subarray(0, n));
          return n;
        },
        region_find: (ptr, length) => {
          const name = readGuestString(this, ptr, length);
          return this.regions.regions.findIndex((region) => region.name === name);
        },
        region_find_id: (id) => this.regions.regions.findIndex((region) => region.id === id),
        region_size: (index) => this.regions.regions[index]?.size ?? 0,
        region_flags: (index) => this.regions.regions[index]?.flags ?? 0,
        region_offset: (index) => this.regions.regions[index]?.ptr ?? 0,
        region_read_u8: (index, offset) => this.regions.read(index, offset),
        region_write_u8: (index, offset, value) => this.regions.write(index, offset, value),
        config_bool: (ptr, length) => this.config.get(readGuestString(this, ptr, length)) ? 1 : 0,
        config_number: (ptr, length) => Number(this.config.get(readGuestString(this, ptr, length))) || 0,
        config_string_length: (ptr, length) =>
          new TextEncoder().encode(String(this.config.get(readGuestString(this, ptr, length)) ?? '')).length,
        config_string_read: (ptr, length, dst, capacity) => {
          const memory = this.instance?.exports?.memory;
          if (!memory || dst < 0 || capacity < 0) return -1;
          const value = new TextEncoder().encode(String(this.config.get(readGuestString(this, ptr, length)) ?? ''));
          const n = Math.min(value.length, capacity, Math.max(0, memory.buffer.byteLength - dst));
          new Uint8Array(memory.buffer, dst, n).set(value.subarray(0, n));
          return n;
        },
        asset_size: (ptr, length) => {
          const name = readGuestString(this, ptr, length);
          return this.package.has(name) ? this.package.read(name).length : -1;
        },
        asset_read: (ptr, length, dst, capacity) => {
          const name = readGuestString(this, ptr, length);
          const memory = this.instance?.exports?.memory;
          if (!this.package.has(name) || !memory || dst < 0 || capacity < 0) return -1;
          const bytes = this.package.read(name);
          const n = Math.min(bytes.length, capacity, Math.max(0, memory.buffer.byteLength - dst));
          new Uint8Array(memory.buffer, dst, n).set(bytes.subarray(0, n));
          return n;
        },
        command_clear: (rgba) => this.compositor.clear(rgba),
        command_draw_game: (x, y, width, height, sampling) =>
          this.compositor.drawGame(x, y, width, height, sampling),
        command_draw_game_fit: (mode, alignX, alignY, sampling) =>
          this.compositor.drawGameFit(mode, alignX, alignY, sampling),
        command_fill_rect: (x, y, width, height, rgba) =>
          this.compositor.fillRect(x, y, width, height, rgba),
        command_triangle: (x1, y1, x2, y2, x3, y3, rgba) =>
          this.compositor.triangle(x1, y1, x2, y2, x3, y3, rgba),
        command_text: (ptr, length, x, y, size, rgba) =>
          this.compositor.text(readGuestString(this, ptr, length), x, y, size, rgba),
        command_scissor: (x, y, width, height) =>
          this.compositor.scissor(x, y, width, height),
        command_scissor_reset: () => this.compositor.resetScissor(),
        texture_create_rgba: (ptr, width, height) => {
          const memory = this.instance?.exports?.memory;
          const length = width * height * 4;
          if (!memory || ptr < 0 || length < 0 || ptr + length > memory.buffer.byteLength) return 0;
          return this.compositor.createTexture(new Uint8Array(memory.buffer, ptr, length), width, height);
        },
        texture_destroy: (handle) => this.compositor.destroyTexture(handle),
        command_draw_texture: (handle, x, y, width, height) =>
          this.compositor.drawTexture(handle, x, y, width, height),
        log: (ptr, length) => {
          if (process.env.RETROEMU_DEBUG) console.error(`[active-bezel] ${readGuestString(this, ptr, length)}`);
        },
      },
      ab_core: {},
    };

    const coreMemory = this.host.core?.wasmMemory
      ?? this.host.core?.asm?.memory
      ?? (this.host.core?.HEAPU8?.buffer instanceof SharedArrayBuffer ? null : this.host.core?.HEAPU8?.buffer);
    if (coreMemory instanceof WebAssembly.Memory) imports.ab_core.memory = coreMemory;

    const bytes = this.package.read(this.package.manifest.entry);
    const module = await WebAssembly.compile(bytes);
    const requiredImports = WebAssembly.Module.imports(module);
    for (const spec of requiredImports) {
      if (spec.kind === 'memory' && spec.module === 'ab_core' && !imports.ab_core.memory) {
        throw new Error('this core does not expose an importable WebAssembly.Memory');
      }
    }
    this.instance = await WebAssembly.instantiate(module, imports);
    const exports = this.instance.exports;
    for (const name of ['ab_abi_version', 'ab_init', 'ab_tick']) {
      if (typeof exports[name] !== 'function') throw new Error(`Active Bezel is missing export ${name}`);
    }
    if (Number(exports.ab_abi_version()) !== 1) throw new Error('Active Bezel guest ABI version is not 1');
    const initResult = Number(exports.ab_init(0));
    if (initResult !== 0) throw new Error(`Active Bezel initialization failed (${initResult})`);
    this.enabled = true;
  }

  processFrame(gameRgba, gameWidth, gameHeight, frameNumber) {
    if (!this.enabled) return { rgba: gameRgba, width: gameWidth, height: gameHeight };
    this.compositor.reset();
    const started = performance.now();
    try {
      let luaFrame = null;
      if (this.lua) luaFrame = this.lua.tick();
      else this.instance.exports.ab_tick(BigInt(frameNumber));
      const exports = this.instance?.exports ?? {};
      if (luaFrame) {
        this.compositor.drawSurface(luaFrame.rgba, luaFrame.width, luaFrame.height);
        const gameAspect = gameWidth / gameHeight;
        const h = LOGICAL_HEIGHT;
        const w = h * gameAspect;
        this.compositor.drawGame((LOGICAL_WIDTH - w) / 2, 0, w, h, 0);
      } else if (typeof exports.ab_framebuffer_ptr === 'function' && exports.memory) {
        const ptr = Number(exports.ab_framebuffer_ptr());
        const width = Number(exports.ab_framebuffer_width?.() ?? 0);
        const height = Number(exports.ab_framebuffer_height?.() ?? 0);
        const length = width * height * 4;
        if (width > 0 && height > 0 && ptr >= 0 && ptr + length <= exports.memory.buffer.byteLength) {
          this.compositor.drawSurface(new Uint8Array(exports.memory.buffer, ptr, length), width, height);
        }
      }
      const elapsed = performance.now() - started;
      const composeStarted = performance.now();
      const composed = this.compositor.compose(gameRgba, gameWidth, gameHeight);
      const composeElapsed = performance.now() - composeStarted;
      this.stats.ticks++;
      this.stats.lastTickMs = elapsed;
      this.stats.totalTickMs += elapsed;
      this.stats.maxTickMs = Math.max(this.stats.maxTickMs, elapsed);
      this.stats.lastComposeMs = composeElapsed;
      this.stats.totalComposeMs += composeElapsed;
      this.stats.maxComposeMs = Math.max(this.stats.maxComposeMs, composeElapsed);
      return composed;
    } catch (err) {
      this.error = err;
      this.enabled = false;
      console.error(`[active-bezel] disabled after trap: ${err?.stack ?? err}`);
      return { rgba: gameRgba, width: gameWidth, height: gameHeight };
    }
  }

  event(type) {
    if (type === AB_EVENT.RESET || type === AB_EVENT.STATE_LOADED || type === AB_EVENT.REWIND_JUMP) {
      this.regions.refresh();
    }
    if (!this.enabled || typeof this.instance?.exports?.ab_event !== 'function') return;
    this.instance.exports.ab_event(type, 0);
  }

  setConfig(key, value) {
    const normalized = this.config.set(key, value);
    this.event(AB_EVENT.CONFIG_CHANGED);
    return normalized;
  }

  setDisplay(width, height) {
    if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0) return;
    this.physicalWidth = width;
    this.physicalHeight = height;
    this.event(AB_EVENT.DISPLAY_CHANGED);
  }

  status() {
    return {
      enabled: this.enabled,
      package: this.package.describe(),
      match: this.match,
      config: { ...this.config.values },
      display: {
        logicalWidth: LOGICAL_WIDTH,
        logicalHeight: LOGICAL_HEIGHT,
        internalWidth: this.compositor.outputWidth,
        internalHeight: this.compositor.outputHeight,
        physicalWidth: this.physicalWidth,
        physicalHeight: this.physicalHeight,
        pictureEffect: this.package.manifest.pictureEffect,
        rendererBackend: this.compositor.gpuReady ? 'opengl-es-3' : 'cpu',
      },
      regions: this.regions.describe(),
      stats: {
        ...this.stats,
        averageTickMs: this.stats.ticks ? this.stats.totalTickMs / this.stats.ticks : 0,
        averageComposeMs: this.stats.ticks ? this.stats.totalComposeMs / this.stats.ticks : 0,
      },
      error: this.error?.message ?? null,
    };
  }

  shutdown() {
    try {
      this.instance?.exports?.ab_shutdown?.();
      void this.lua?.shutdown();
    } finally {
      this.compositor.destroy();
      this.enabled = false;
    }
  }
}
