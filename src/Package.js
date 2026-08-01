import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import yauzl from 'yauzl';

const ID_RE = /^[a-z0-9][a-z0-9._-]{2,127}$/;
const VERSION_RE = /^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$/;
const RENDERERS = new Set(['cpu-rgba-v1', 'gpu-command-v1']);
const SETTING_TYPES = new Set(['boolean', 'integer', 'float', 'number', 'choice', 'color', 'action']);

function safeName(name) {
  if (!name || name.includes('\0') || name.includes('\\') || path.posix.isAbsolute(name)) return false;
  const normalized = path.posix.normalize(name);
  return normalized !== '..' && !normalized.startsWith('../');
}

function readZip(file) {
  return new Promise((resolve, reject) => {
    yauzl.open(file, { lazyEntries: true, autoClose: true }, (err, zip) => {
      if (err) return reject(err);
      const entries = new Map();
      let total = 0;
      zip.readEntry();
      zip.on('entry', (entry) => {
        if (!safeName(entry.fileName)) {
          zip.close();
          return reject(new Error(`unsafe package entry: ${entry.fileName}`));
        }
        if (/\/$/.test(entry.fileName)) {
          zip.readEntry();
          return;
        }
        if (((entry.externalFileAttributes >>> 16) & 0o170000) === 0o120000) {
          zip.close();
          return reject(new Error(`symlinks are not allowed: ${entry.fileName}`));
        }
        if (entry.uncompressedSize > 64 * 1024 * 1024 || total + entry.uncompressedSize > 128 * 1024 * 1024) {
          zip.close();
          return reject(new Error('active bezel package exceeds the 128 MiB unpacked limit'));
        }
        zip.openReadStream(entry, (streamErr, stream) => {
          if (streamErr) return reject(streamErr);
          const chunks = [];
          stream.on('data', (chunk) => chunks.push(chunk));
          stream.on('error', reject);
          stream.on('end', () => {
            const data = Buffer.concat(chunks);
            total += data.length;
            entries.set(entry.fileName, data);
            zip.readEntry();
          });
        });
      });
      zip.on('end', () => resolve(entries));
      zip.on('error', reject);
    });
  });
}

function validateSettings(settings) {
  if (settings === undefined) return [];
  if (!Array.isArray(settings)) throw new Error('manifest.settings must be an array');
  const keys = new Set();
  return settings.map((setting, i) => {
    if (!setting || typeof setting !== 'object') throw new Error(`setting ${i} must be an object`);
    if (!/^[A-Za-z][A-Za-z0-9_.-]{0,63}$/.test(setting.key ?? '')) {
      throw new Error(`setting ${i} has an invalid key`);
    }
    if (keys.has(setting.key)) throw new Error(`duplicate setting key: ${setting.key}`);
    keys.add(setting.key);
    if (!SETTING_TYPES.has(setting.type)) throw new Error(`unsupported setting type: ${setting.type}`);
    if (setting.type === 'choice' && (!Array.isArray(setting.choices) || setting.choices.length === 0)) {
      throw new Error(`choice setting ${setting.key} needs choices`);
    }
    return { ...setting };
  });
}

export function validateManifest(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('manifest must be an object');
  const m = structuredClone(input);
  if (m.format !== 'active-bezel' || m.formatVersion !== 1) {
    throw new Error('unsupported Active Bezel package format');
  }
  if (!ID_RE.test(m.id ?? '')) throw new Error('manifest.id is invalid');
  if (!VERSION_RE.test(m.version ?? '')) throw new Error('manifest.version must be semantic versioning');
  if (typeof m.name !== 'string' || !m.name.trim()) throw new Error('manifest.name is required');
  if (!safeName(m.entry ?? '')) throw new Error('manifest.entry is invalid');
  if (m.runtime?.abi !== 'active-bezel-1') throw new Error('runtime.abi must be active-bezel-1');
  if (!RENDERERS.has(m.runtime?.renderer)) throw new Error(`unsupported renderer: ${m.runtime?.renderer}`);
  if (m.runtime.language !== undefined && !['wasm', 'lua54-wasmcart'].includes(m.runtime.language)) {
    throw new Error(`unsupported runtime language: ${m.runtime.language}`);
  }
  m.runtime.language ??= 'wasm';
  if (m.runtime.language === 'lua54-wasmcart') {
    const root = m.runtime.luaAssets ?? 'app/';
    if (!safeName(root.replace(/\/$/, '') || 'app')) throw new Error('runtime.luaAssets is invalid');
    m.runtime.luaAssets = root.endsWith('/') ? root : `${root}/`;
  }
  m.runtime.extensions ??= [];
  if (!Array.isArray(m.runtime.extensions)) throw new Error('runtime.extensions must be an array');
  if (m.runtime.internalResolution !== undefined) {
    const [width, height, ...extra] = Array.isArray(m.runtime.internalResolution)
      ? m.runtime.internalResolution : [];
    if (extra.length || !Number.isSafeInteger(width) || !Number.isSafeInteger(height)
      || width < 16 || height < 9 || width > 3840 || height > 2160
      || Math.abs(width / height - 16 / 9) > 0.001) {
      throw new Error('runtime.internalResolution must be a 16:9 [width,height] no larger than 3840x2160');
    }
  }
  m.games ??= [];
  m.compatible ??= [];
  m.requires ??= [];
  if (!Array.isArray(m.games) || !Array.isArray(m.compatible) || !Array.isArray(m.requires)) {
    throw new Error('games, compatible and requires must be arrays');
  }
  /*
   * `universal` says "this package works with ANY ROM" -- a CRT-in-a-room
   * bezel, a scanline filter, a border. It is a deliberate claim, not the
   * absence of one: an empty `games` list means "matches nothing", which is
   * indistinguishable from a package whose author simply forgot to list its
   * ROMs. Without this flag a game-agnostic bezel could only ever be loaded
   * with force, and the host had to tell every user that a package built to
   * work everywhere "does not match this ROM".
   *
   * A universal package must not ALSO claim specific ROMs: the two are
   * different promises, and a manifest making both is a mistake worth
   * catching at load rather than at match time.
   */
  m.universal ??= false;
  if (typeof m.universal !== 'boolean') throw new Error('universal must be a boolean');
  if (m.universal && (m.games.length || m.compatible.length)) {
    throw new Error(
      'a universal package must not also list games or compatible rules — '
      + 'it either works with any ROM or it works with specific ones');
  }
  for (const game of m.games) {
    if (!game.platform || !/^[0-9a-f]{64}$/i.test(game.sha256 ?? '')) {
      throw new Error('each exact game needs platform and sha256');
    }
  }
  for (const rule of m.compatible) {
    if (!rule.platform || !Number.isSafeInteger(rule.size) || !Array.isArray(rule.signatures)) {
      throw new Error('each compatible rule needs platform, size and signatures');
    }
    for (const sig of rule.signatures) {
      if (!Number.isSafeInteger(sig.offset) || sig.offset < 0 || !/^(?:[0-9a-f]{2})+$/i.test(sig.bytes ?? '')) {
        throw new Error('compatibility signature has invalid offset or bytes');
      }
    }
  }
  for (const requirement of m.requires) {
    if (typeof requirement.region !== 'string' || !requirement.region) throw new Error('region requirement needs a name');
    requirement.minSize ??= 1;
  }
  m.settings = validateSettings(m.settings);
  m.pictureEffect ??= 'scene';
  if (!['none', 'game', 'scene', 'composite'].includes(m.pictureEffect)) {
    throw new Error('pictureEffect must be "none", "game", "scene", or "composite"');
  }
  return m;
}

export class ActiveBezelPackage {
  static async open(packagePath) {
    const absolute = path.resolve(packagePath);
    const stat = await fs.stat(absolute);
    let entries;
    let archiveSha256;
    if (stat.isDirectory()) {
      entries = new Map();
      let total = 0;
      const visit = async (dir, prefix = '') => {
        for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
          if (entry.isSymbolicLink()) throw new Error(`symlinks are not allowed: ${entry.name}`);
          const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
          if (!safeName(rel)) throw new Error(`unsafe package entry: ${rel}`);
          const full = path.join(dir, entry.name);
          if (entry.isDirectory()) await visit(full, rel);
          else {
            const fileStat = await fs.stat(full);
            if (fileStat.size > 64 * 1024 * 1024 || total + fileStat.size > 128 * 1024 * 1024) {
              throw new Error('active bezel package exceeds the 128 MiB unpacked limit');
            }
            const bytes = await fs.readFile(full);
            total += bytes.length;
            entries.set(rel, bytes);
          }
        }
      };
      await visit(absolute);
      const hash = crypto.createHash('sha256');
      for (const name of [...entries.keys()].sort()) hash.update(name).update('\0').update(entries.get(name));
      archiveSha256 = hash.digest('hex');
    } else {
      const archive = await fs.readFile(absolute);
      archiveSha256 = crypto.createHash('sha256').update(archive).digest('hex');
      entries = await readZip(absolute);
    }
    const manifestBytes = entries.get('manifest.json');
    if (!manifestBytes) throw new Error('Active Bezel package is missing manifest.json');
    let parsed;
    try {
      parsed = JSON.parse(manifestBytes.toString('utf8'));
    } catch (err) {
      throw new Error(`invalid manifest.json: ${err.message}`);
    }
    const manifest = validateManifest(parsed);
    if (!entries.has(manifest.entry)) throw new Error(`Active Bezel package is missing ${manifest.entry}`);
    return new ActiveBezelPackage({ packagePath: absolute, archiveSha256, entries, manifest });
  }

  constructor({ packagePath, archiveSha256, entries, manifest }) {
    this.path = packagePath;
    this.archiveSha256 = archiveSha256;
    this.entries = entries;
    this.manifest = manifest;
  }

  read(name) {
    if (!safeName(name)) throw new Error(`unsafe asset path: ${name}`);
    const bytes = this.entries.get(name);
    if (!bytes) throw new Error(`package asset not found: ${name}`);
    return bytes;
  }

  has(name) {
    return safeName(name) && this.entries.has(name);
  }

  describe() {
    const m = this.manifest;
    return {
      id: m.id, name: m.name, version: m.version, author: m.author ?? null,
      renderer: m.runtime.renderer, archiveSha256: this.archiveSha256, path: this.path,
    };
  }
}
