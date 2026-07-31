#!/usr/bin/env node
/*
 * Verify every prebuilt runtime wasm against the shipped ABI contract.
 *
 * The prebuilt wasms are the whole point of runtimes/: a bezel author copies
 * main.wasm plus a script and never installs emcc. That makes these binaries
 * the artifact consumers actually run, so they need a gate that does not
 * depend on anyone having a toolchain. Each build.sh already self-checks at
 * BUILD time; this checks the COMMITTED bytes, which is a different claim --
 * a wasm can be committed stale, or from a tree that was mid-edit.
 *
 * Checks per runtime:
 *   1. imports come only from the ab_host module -- the host provides exactly
 *      one import object, so an `env` or wasi_snapshot_preview1 import means
 *      the module cannot instantiate at all in a real session.
 *   2. every imported name exists in sdk/abi.json hostImports. Importing a
 *      name the host does not supply is a LinkError at load, and the module
 *      name check alone would not catch a typo'd or removed binding.
 *   3. every required guest export from sdk/abi.json is present, with the
 *      declared signature. Signatures matter because a wrong ab_tick arity
 *      traps on the first frame rather than failing at load.
 *   4. size is under the runtime's budget.
 *
 * The ABI comes from sdk/abi.json rather than a hardcoded list so that this
 * script cannot drift from the spec it is supposed to be enforcing.
 *
 * Usage: node scripts/check-runtimes.mjs [--json]
 * Exits 1 if any runtime fails any check.
 */
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const RUNTIMES_DIR = path.join(ROOT, 'runtimes');

/*
 * Size budgets, in bytes. These are ceilings that catch a REGRESSION (a
 * debug build, a dropped -Oz, an accidentally linked stdlib), not targets --
 * so each sits roughly 1.5-2x over the current artifact rather than hugging
 * it. A budget set to the current size would fail on every legitimate
 * feature addition, which trains people to raise it without looking.
 *
 * Current sizes at the time of writing: lua 337K, js 531K, python 236K,
 * ruby 573K.
 */
const BUDGETS = {
  lua: 512 * 1024,
  js: 1024 * 1024,
  python: 512 * 1024,
  ruby: 1024 * 1024,
};

// Runtimes with no budget entry are still checked for ABI conformance; they
// just have no size ceiling. Better than refusing to run when a fifth
// language lands before someone picks a number for it.
const DEFAULT_BUDGET = null;

const abi = JSON.parse(readFileSync(path.join(ROOT, 'sdk', 'abi.json'), 'utf8'));
const HOST_MODULE = 'ab_host';
const KNOWN_IMPORTS = new Set(Object.keys(abi.hostImports));
const REQUIRED_EXPORTS = Object.entries(abi.guestExports)
  .filter(([, spec]) => spec.required)
  .map(([name]) => name);

/*
 * The five ABI functions the task contract names. ab_event and ab_shutdown
 * are optional in abi.json (a guest may omit them), but every runtime we
 * SHIP implements all five, because a runtime that cannot receive an event
 * or clean up is not a complete host for a scripting language. So the bar
 * for runtimes/ is stricter than the bar for an arbitrary guest.
 */
const RUNTIME_EXPORTS = ['ab_abi_version', 'ab_init', 'ab_tick', 'ab_event', 'ab_shutdown'];

const WASM_TYPE = { 0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64' };

/**
 * Read the declared signature of an exported function.
 *
 * WebAssembly.Module.exports() gives names and kinds but not types, so the
 * type section is parsed directly. Node's `type` reflection is behind a flag
 * and not available on every version in the support matrix (engines says
 * node >= 20), and this is a few dozen lines of LEB128.
 *
 * @param {Uint8Array} bytes whole wasm module
 * @returns {Map<string, {params: string[], result: string|null}>}
 */
function exportSignatures(bytes) {
  let p = 8; // skip magic + version
  const u32 = () => {
    let result = 0;
    let shift = 0;
    for (;;) {
      const b = bytes[p++];
      result |= (b & 0x7f) << shift;
      if ((b & 0x80) === 0) return result >>> 0;
      shift += 7;
    }
  };

  const types = []; // type-section signatures, indexed by type index
  const funcTypes = []; // function index -> type index
  const exportRefs = []; // [name, funcIndex], resolved after all sections
  let sawFuncSection = false;

  while (p < bytes.length) {
    const id = bytes[p++];
    const size = u32();
    const end = p + size;

    if (id === 1) { // type
      const count = u32();
      for (let i = 0; i < count; i++) {
        p++; // 0x60 func form
        const nParams = u32();
        const params = [];
        for (let j = 0; j < nParams; j++) params.push(WASM_TYPE[bytes[p++]] ?? '?');
        const nResults = u32();
        const results = [];
        for (let j = 0; j < nResults; j++) results.push(WASM_TYPE[bytes[p++]] ?? '?');
        types.push({ params, result: results[0] ?? null });
      }
    } else if (id === 2) { // import -- imported functions occupy the LOW indices
      const count = u32();
      for (let i = 0; i < count; i++) {
        // NOT `p += u32()`: += snapshots p BEFORE u32() advances the cursor
        // past the LEB128 length itself, so the length bytes get counted
        // twice-over-wrong and every later import lands mid-record.
        const modLen = u32(); p += modLen; // module name bytes
        const fieldLen = u32(); p += fieldLen; // field name bytes
        const kind = bytes[p++];
        if (kind === 0x00) { // func: typeidx, and it consumes a function index
          funcTypes.push(u32());
        } else if (kind === 0x01) { // table: reftype, limits
          p++;
          const flags = bytes[p++];
          u32();
          if (flags & 0x01) u32();
        } else if (kind === 0x02) { // memory: limits
          const flags = bytes[p++];
          u32();
          if (flags & 0x01) u32();
        } else if (kind === 0x03) { // global: valtype, mutability
          p += 2;
        }
      }
    } else if (id === 3) { // function -- defined funcs continue the index space
      sawFuncSection = true;
      const count = u32();
      for (let i = 0; i < count; i++) funcTypes.push(u32());
    } else if (id === 7) { // export
      const count = u32();
      for (let i = 0; i < count; i++) {
        const nameLen = u32();
        const name = Buffer.from(bytes.subarray(p, p + nameLen)).toString('utf8');
        p += nameLen;
        const kind = bytes[p++];
        const index = u32();
        if (kind === 0x00) exportRefs.push([name, index]);
      }
    }
    p = end; // sections we do not care about are skipped wholesale
  }

  // Resolved only now: the export section can legally precede the function
  // section, so funcTypes is not complete while exports are being read.
  const exports = new Map();
  if (!sawFuncSection && exportRefs.length) return exports; // nothing to resolve against
  for (const [name, index] of exportRefs) {
    const sig = types[funcTypes[index]];
    if (sig) exports.set(name, sig);
  }
  return exports;
}

/**
 * @param {string} name runtime directory name
 * @param {string} wasmPath absolute path to main.wasm
 */
function checkRuntime(name, wasmPath) {
  const failures = [];
  const warnings = [];
  const bytes = readFileSync(wasmPath);
  const size = statSync(wasmPath).size;

  let module;
  try {
    module = new WebAssembly.Module(bytes);
  } catch (err) {
    return { name, size, failures: [`not a valid wasm module: ${err.message}`], warnings, modules: [], exports: [] };
  }

  const byModule = new Map();
  for (const imp of WebAssembly.Module.imports(module)) {
    if (!byModule.has(imp.module)) byModule.set(imp.module, []);
    byModule.get(imp.module).push(imp.name);
  }

  // 1. only ab_host
  const stray = [...byModule.keys()].filter((m) => m !== HOST_MODULE);
  for (const m of stray) {
    failures.push(`imports from '${m}' (only '${HOST_MODULE}' is provided): ${byModule.get(m).sort().join(' ')}`);
  }

  // 2. every ab_host name is one the host actually exposes
  const unknown = (byModule.get(HOST_MODULE) ?? []).filter((n) => !KNOWN_IMPORTS.has(n));
  if (unknown.length) {
    failures.push(`imports names absent from sdk/abi.json hostImports: ${unknown.sort().join(' ')}`);
  }

  // 3. exports: presence (hard) + signature (advisory, see below)
  const sigs = exportSignatures(bytes);
  const exportNames = new Set(WebAssembly.Module.exports(module).map((e) => e.name));
  for (const fn of RUNTIME_EXPORTS) {
    if (!exportNames.has(fn)) { failures.push(`missing export ${fn}`); continue; }
    const want = abi.guestExports[fn];
    const got = sigs.get(fn);
    if (!want || !got) continue;
    const wantSig = `(${want.params.join(',')})->${want.result ?? 'void'}`;
    const gotSig = `(${got.params.join(',')})->${got.result ?? 'void'}`;
    /*
     * Signature mismatches are FAILURES.
     *
     * They were warnings while a real three-way drift existed: runtime.c
     * declared ab_init(void)/ab_event(int32_t), sdk/abi.json declared
     * ab_init(i32)/ab_event(i32,i32), and src/Runtime.js called them with
     * the extra argument. Nothing broke, because a JS -> wasm call silently
     * DROPS surplus arguments -- which is exactly what made it invisible.
     *
     * sdk/abi.json won (examples/diagnostic/main.c, the original C guest,
     * always took ab_init(descriptor) and ab_event(type, data)), and all
     * four runtimes were rebuilt to match. With the spec and the artifacts
     * agreeing there is no judgement left to defer, and a host that does
     * NOT drop surplus arguments would trap on a regression here.
     */
    if (wantSig !== gotSig) failures.push(`${fn} is ${gotSig}, sdk/abi.json declares ${wantSig}`);
  }
  for (const fn of REQUIRED_EXPORTS) {
    if (!exportNames.has(fn)) failures.push(`missing REQUIRED export ${fn}`);
  }

  // 4. size budget
  const budget = BUDGETS[name] ?? DEFAULT_BUDGET;
  if (budget !== null && size > budget) {
    failures.push(`${kb(size)} exceeds budget ${kb(budget)}`);
  }

  return {
    name,
    size,
    budget,
    failures,
    warnings,
    modules: [...byModule.keys()].sort(),
    importCount: (byModule.get(HOST_MODULE) ?? []).length,
    exports: RUNTIME_EXPORTS.filter((fn) => exportNames.has(fn)),
  };
}

const kb = (n) => `${(n / 1024).toFixed(0)}KB`;

function main() {
  const json = process.argv.includes('--json');

  const names = readdirSync(RUNTIMES_DIR, { withFileTypes: true })
    .filter((d) => d.isDirectory() && d.name !== 'common')
    .map((d) => d.name)
    .sort();

  const results = [];
  for (const name of names) {
    const wasmPath = path.join(RUNTIMES_DIR, name, 'main.wasm');
    let exists = true;
    try { statSync(wasmPath); } catch { exists = false; }
    if (!exists) {
      // A missing wasm is a hard failure here even though the test suite
      // SKIPS it. That difference is deliberate: the suite has to stay green
      // on a working tree where someone is mid-rebuild, but the committed
      // repo must always carry all four, and this script is what CI uses to
      // say so before it trusts a green test run.
      results.push({ name, size: 0, failures: ['main.wasm missing'], warnings: [], modules: [], exports: [] });
      continue;
    }
    results.push(checkRuntime(name, wasmPath));
  }

  if (json) {
    console.log(JSON.stringify(results, null, 2));
  } else {
    const w = Math.max(7, ...results.map((r) => r.name.length));
    console.log(`${'runtime'.padEnd(w)}  ${'size'.padStart(8)}  ${'budget'.padStart(8)}  ${'imports'.padStart(7)}  exports  status`);
    console.log('-'.repeat(w + 46));
    for (const r of results) {
      const status = r.failures.length ? 'FAIL' : 'ok';
      const imports = r.modules.length ? r.modules.join(',') : '(none)';
      console.log(
        `${r.name.padEnd(w)}  ${kb(r.size).padStart(8)}  ${(r.budget ? kb(r.budget) : '-').padStart(8)}  `
        + `${String(r.importCount ?? 0).padStart(3)} ${imports.padEnd(10)}  ${String(r.exports.length)}/5      ${status}`,
      );
    }
    const warned = results.filter((r) => r.warnings?.length);
    if (warned.length) {
      console.log('\nwarnings (not fatal):');
      for (const r of warned) for (const w of r.warnings) console.log(`  ${r.name}: ${w}`);
    }
    for (const r of results) {
      for (const f of r.failures) console.error(`${r.name}: ${f}`);
    }
  }

  const failed = results.filter((r) => r.failures.length);
  if (failed.length) {
    if (!json) console.error(`\n${failed.length} of ${results.length} runtimes failed`);
    process.exit(1);
  }
  if (!json) console.log(`\nall ${results.length} runtimes ok`);
}

main();
