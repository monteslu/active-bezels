#!/usr/bin/env node
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ActiveBezelPackage, validateManifest } from '../src/Package.js';

const CRC_TABLE = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  CRC_TABLE[n] = c >>> 0;
}
function crc32(bytes) {
  let c = 0xffffffff;
  for (const byte of bytes) c = CRC_TABLE[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function u16(n) { const b = Buffer.alloc(2); b.writeUInt16LE(n); return b; }
function u32(n) { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; }

/* Files that never belong inside a package:
 *   *.ab            — a previous build sitting in the source dir. Packing the
 *                     dir it lives in nests archive inside archive (found as
 *                     a 10MB "0.1.1" holding a 0.1.0 holding nothing wrong).
 *   screenshot.png  — repo/gallery preview, not runtime data the guest reads.
 *   dotfiles        — .git, .gitignore, .DS_Store and friends. */
function excluded(name) {
  const base = name.split('/').pop();
  return name.endsWith('.ab') || base === 'screenshot.png' || base.startsWith('.');
}

async function collect(root, dir = root, out = []) {
  for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
    if (entry.isSymbolicLink()) throw new Error(`symlink not allowed: ${entry.name}`);
    const full = path.join(dir, entry.name);
    const rel = path.relative(root, full).split(path.sep).join('/');
    if (excluded(rel)) continue;
    if (entry.isDirectory()) await collect(root, full, out);
    else out.push({ name: rel, data: await fs.readFile(full) });
  }
  return out;
}

async function pack(source, destination) {
  const files = await collect(source);
  const manifestFile = files.find((file) => file.name === 'manifest.json');
  if (!manifestFile) throw new Error('source is missing manifest.json');
  const manifest = validateManifest(JSON.parse(manifestFile.data.toString('utf8')));
  if (!files.some((file) => file.name === manifest.entry)) throw new Error(`source is missing ${manifest.entry}`);

  const locals = [];
  const central = [];
  let offset = 0;
  for (const file of files.sort((a, b) => a.name.localeCompare(b.name))) {
    const name = Buffer.from(file.name);
    const crc = crc32(file.data);
    const local = Buffer.concat([
      u32(0x04034b50), u16(20), u16(0), u16(0), u16(0), u16(0),
      u32(crc), u32(file.data.length), u32(file.data.length), u16(name.length), u16(0), name, file.data,
    ]);
    locals.push(local);
    central.push(Buffer.concat([
      u32(0x02014b50), u16(20), u16(20), u16(0), u16(0), u16(0), u16(0),
      u32(crc), u32(file.data.length), u32(file.data.length), u16(name.length), u16(0), u16(0),
      u16(0), u16(0), u32(0), u32(offset), name,
    ]));
    offset += local.length;
  }
  const centralBytes = Buffer.concat(central);
  const zip = Buffer.concat([
    ...locals,
    centralBytes,
    u32(0x06054b50), u16(0), u16(0), u16(files.length), u16(files.length),
    u32(centralBytes.length), u32(offset), u16(0),
  ]);
  await fs.writeFile(destination, zip);
  return ActiveBezelPackage.open(destination);
}

/*
 * Scaffold a new bezel.
 *
 * Every target copies `runtimes/<lang>/start/`: a runtime (or a prebuilt
 * wasm), a commented source file that is already a working bezel, and a ready
 * manifest. The four scripting targets need no toolchain at all -- edit the
 * script, reload, done. `c` additionally ships its header and a build.sh.
 */
const SCAFFOLD_LANGUAGES = ['lua', 'js', 'python', 'ruby', 'c'];

async function scaffold(destination, language = 'lua') {
  if (!SCAFFOLD_LANGUAGES.includes(language)) {
    throw new Error(
      `unknown language '${language}'. Try one of: ${SCAFFOLD_LANGUAGES.join(', ')}`);
  }
  const here = path.dirname(fileURLToPath(import.meta.url));
  const source = path.resolve(here, `../runtimes/${language}/start`);
  const target = path.resolve(destination);
  try {
    await fs.stat(target);
    throw new Error(`destination already exists: ${target}`);
  } catch (err) {
    if (err.code !== 'ENOENT') throw err;
  }
  await fs.cp(source, target, { recursive: true, errorOnExist: true });
  if (language === 'c') {
    /* the machine-readable ABI, beside the header, for anyone generating
     * bindings or checking a signature */
    await fs.copyFile(path.resolve(here, '../sdk/abi.json'), path.join(target, 'abi.json'));
  }
  {
    /* Name the package after the directory, so two scaffolds are not both
     * "My Bezel" with the same id. */
    const manifestPath = path.join(target, 'manifest.json');
    const manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
    const slug = path.basename(target).replace(/[^a-zA-Z0-9._-]/g, '-').toLowerCase();
    manifest.id = `local.${slug}`;
    manifest.name = path.basename(target);
    await fs.writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
  }
  return target;
}

function usage() {
  console.log(`abtool — Active Bezel v1 package tools

Usage:
  abtool inspect <package.ab|directory>
  abtool pack <source-directory> <package.ab>
  abtool verify <package.ab|directory>
  abtool scaffold <destination> [lua|js|python|ruby|c]   (default: lua)

The packer writes a deterministic, uncompressed ZIP. WASM and PNG assets are
already compact, and stored entries make packages fast and universally
inspectable.`);
}

const [command, ...args] = process.argv.slice(2);
try {
  if (command === 'inspect' && args[0]) {
    const pkg = await ActiveBezelPackage.open(args[0]);
    console.log(JSON.stringify({
      ...pkg.describe(),
      games: pkg.manifest.games.length,
      compatible: pkg.manifest.compatible.length,
      settings: pkg.manifest.settings,
      entries: [...pkg.entries.keys()],
    }, null, 2));
  } else if (command === 'pack' && args[0] && args[1]) {
    const pkg = await pack(path.resolve(args[0]), path.resolve(args[1]));
    console.log(`${pkg.manifest.name} ${pkg.manifest.version}`);
    console.log(pkg.path);
    console.log(`sha256 ${pkg.archiveSha256}`);
  } else if (command === 'verify' && args[0]) {
    const pkg = await ActiveBezelPackage.open(args[0]);
    const module = await WebAssembly.compile(pkg.read(pkg.manifest.entry));
    const imports = WebAssembly.Module.imports(module);
    const exports = WebAssembly.Module.exports(module);
    if (pkg.manifest.runtime.language === 'wasm') {
      for (const required of ['ab_abi_version', 'ab_init', 'ab_tick']) {
        if (!exports.some((item) => item.kind === 'function' && item.name === required)) {
          throw new Error(`entry is missing ${required}`);
        }
      }
    }
    const allowedModules = pkg.manifest.runtime.language === 'lua54-wasmcart'
      ? ['ab_host', 'ab_core', 'env', 'wasi_snapshot_preview1']
      : ['ab_host', 'ab_core'];
    const forbidden = imports.filter((item) => !allowedModules.includes(item.module));
    if (forbidden.length) throw new Error(`entry imports unsupported module ${forbidden[0].module}`);
    console.log(JSON.stringify({
      ok: true,
      package: pkg.describe(),
      imports,
      exports,
    }, null, 2));
  } else if (command === 'scaffold' && args[0]) {
    const language = args[1] ?? 'lua';
    console.log(await scaffold(args[0], language));
  } else {
    usage();
    process.exitCode = command ? 1 : 0;
  }
} catch (err) {
  console.error(`abtool: ${err.message}`);
  process.exitCode = 1;
}
