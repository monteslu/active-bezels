import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { CartHost } from 'wasmcart';

export class ActiveBezelLuaAdapter {
  constructor(pkg) {
    this.package = pkg;
    this.host = new CartHost();
    this.tempDir = null;
  }

  async init() {
    this.tempDir = await fs.mkdtemp(path.join(os.tmpdir(), 'active-bezel-lua-'));
    const root = this.package.manifest.runtime.luaAssets ?? 'app/';
    const prefix = root.endsWith('/') ? root : `${root}/`;
    await fs.writeFile(path.join(this.tempDir, 'main.wasm'), this.package.read(this.package.manifest.entry));
    const [width, height] = this.package.manifest.runtime.internalResolution ?? [1280, 720];
    await fs.writeFile(path.join(this.tempDir, 'manifest.json'), JSON.stringify({
      name: this.package.manifest.name,
      version: this.package.manifest.version,
      abi: 3,
      entry: 'main.wasm',
      width,
      height,
      assets: 'app/',
      players: 0,
      debug: true,
    }));
    for (const [name, bytes] of this.package.entries) {
      if (!name.startsWith(prefix) || name.endsWith('/')) continue;
      const relative = name.slice(prefix.length);
      const destination = path.join(this.tempDir, 'app', ...relative.split('/'));
      await fs.mkdir(path.dirname(destination), { recursive: true });
      await fs.writeFile(destination, bytes);
    }
    await this.host.load(this.tempDir, { deterministic: { seed: 1 } });
  }

  tick() {
    const frame = this.host.runFrame([]);
    return {
      rgba: frame.framebuffer,
      width: frame.width,
      height: frame.height,
    };
  }

  async shutdown() {
    this.host.destroy?.();
    if (this.tempDir) {
      await fs.rm(this.tempDir, { recursive: true, force: true });
      this.tempDir = null;
    }
  }
}
