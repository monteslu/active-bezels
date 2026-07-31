/*
 * RetroArch `.glslp` shader presets.
 *
 * A `.glslp` is an INI-ish file describing a CHAIN of GLSL passes: each pass
 * renders into an offscreen buffer, the next pass samples it, and the last one
 * produces the picture. Single-pass shaders already work through
 * `surface_filter`; this is what unlocks the multi-pass ones -- crt-royale is
 * twelve passes -- which is most of the interesting shader library.
 *
 * This module is PARSING AND PLANNING ONLY. It turns a preset file into a
 * plain description: which sources to compile, how big each pass's target is,
 * what to bind where. It never touches GL. That split is deliberate -- the
 * plan is pure data, so it can be unit-tested against the 600-odd presets that
 * ship with RetroArch without a GPU anywhere in sight, and the executor
 * (GlslChain) stays a thin loop over it.
 *
 * Semantics follow libretro's own GLSL loader. Where real presets in the wild
 * disagree with a strict reading of it, the wild wins and the deviation is
 * commented at the point it matters.
 */

import { readFileSync, existsSync } from 'node:fs';
import { dirname, resolve, extname } from 'node:path';

/* Scale kinds, spelled as the preset spells them. */
export const SCALE_SOURCE = 'source';
export const SCALE_VIEWPORT = 'viewport';
export const SCALE_ABSOLUTE = 'absolute';

const WRAP_MODES = new Set(['repeat', 'clamp_to_edge', 'clamp', 'clamp_to_border']);

/*
 * Strip a value as written in a preset.
 *
 * Quotes are optional in this format -- `shaders = "12"` and `shaders = 5`
 * both occur in the shipped corpus -- and a `#` can start a trailing comment
 * on a value line, which crt-royale does:
 *
 *   mask_grille_texture_small_mipmap = "false"  # Mipmapping causes artifacts
 *
 * The comment strip has to happen OUTSIDE quotes, because a quoted path could
 * legitimately contain a '#'. Cheap state machine rather than a regex so that
 * case is actually handled rather than accidentally handled.
 */
function cleanValue(raw) {
  let out = '';
  let quote = null;
  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i];
    if (quote) {
      if (ch === quote) quote = null;
      else out += ch;
      continue;
    }
    if (ch === '"' || ch === "'") { quote = ch; continue; }
    if (ch === '#') break;                    /* trailing comment */
    out += ch;
  }
  return out.trim();
}

/* Parse the flat key/value layer. Later keys win, matching the reference loader. */
export function parseIni(text) {
  const map = new Map();
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#') || trimmed.startsWith('//')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 0) continue;
    const key = trimmed.slice(0, eq).trim();
    if (!key) continue;
    map.set(key, cleanValue(trimmed.slice(eq + 1)));
  }
  return map;
}

function getBool(map, key, fallback) {
  if (!map.has(key)) return fallback;
  const v = map.get(key).toLowerCase();
  if (v === 'true' || v === '1') return true;
  if (v === 'false' || v === '0') return false;
  return fallback;
}

function getFloat(map, key, fallback) {
  if (!map.has(key)) return fallback;
  const n = Number.parseFloat(map.get(key));
  return Number.isFinite(n) ? n : fallback;
}

function getScaleType(map, key) {
  if (!map.has(key)) return null;
  const v = map.get(key).toLowerCase();
  return (v === SCALE_SOURCE || v === SCALE_VIEWPORT || v === SCALE_ABSOLUTE) ? v : null;
}

function getWrap(map, keys, fallback) {
  for (const key of keys) {
    if (!map.has(key)) continue;
    const v = map.get(key).toLowerCase();
    if (WRAP_MODES.has(v)) return v;
  }
  return fallback;
}

/*
 * Split a list value.
 *
 * `textures` is reliably semicolon-separated, but `parameters` is not: most
 * presets use ';' and at least one shipped preset (dithering/bayer_4x4) uses
 * ', '. Accept both rather than silently reading one long bogus name.
 */
function splitList(value) {
  if (!value) return [];
  return value.split(/[;,]/).map((s) => s.trim()).filter(Boolean);
}

/*
 * `#pragma parameter <id> "<label>" <default> <min> <max> <step>`
 *
 * Parameters are GLOBAL across the preset, not per-pass: the first declaration
 * of an id wins, and the resolved value is pushed to every pass that declares
 * a uniform of that name.
 */
export function parsePragmaParameters(source) {
  const found = [];
  const re = /^\s*#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)/gm;
  let m;
  while ((m = re.exec(source)) !== null) {
    const step = Number.parseFloat(m[6]);
    found.push({
      id: m[1],
      label: m[2],
      value: Number.parseFloat(m[3]),
      minimum: Number.parseFloat(m[4]),
      maximum: Number.parseFloat(m[5]),
      /* A zero step is meaningless and the reference loader rewrites it to 1. */
      step: step === 0 ? 1 : step,
    });
  }
  return found;
}

/*
 * Compile-time prologue for one stage of a pass.
 *
 * A `.glsl` pass is ONE file compiled twice, selecting a stage with a #define
 * and branching on `#if defined(VERTEX)`. The defines must land AFTER any
 * `#version`, which must remain the first line -- getting this backwards is an
 * instant compile error on every real preset, since royale's files open with
 * `#version 130`.
 */
export function injectDefines(source, stage, extraDefines = []) {
  const defines = [`#define ${stage}`, '#define PARAMETER_UNIFORM', ...extraDefines];
  const lines = source.split('\n');
  let insertAt = 0;
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].trim().startsWith('#version')) { insertAt = i + 1; break; }
    if (lines[i].trim() !== '') break;         /* no #version: prepend */
  }
  lines.splice(insertAt, 0, ...defines);
  return lines.join('\n');
}

/*
 * Retarget a desktop-GL shader at GLES 3.
 *
 * These presets were written for desktop OpenGL and say so: `#version 130`,
 * `#version 330`, and so on. A GLES context accepts only `100 es` and
 * `300 es`, so ~100 of the shipped shader files fail to compile on sight --
 * including every pass of crt-royale.
 *
 * The dialects are close enough that a header swap plus a compatibility
 * prelude carries them across. What actually differs, and is handled here:
 *
 *   - the version line itself
 *   - GLES needs an explicit default float precision; desktop does not have one
 *   - `attribute`/`varying` are the 1.10 spelling of `in`/`out`. Files that
 *     declare 130+ already use in/out, but files with no #version at all are
 *     usually written in the old spelling, and those must be left alone
 *     because they are the ones that still compile as `100 es`.
 *   - `texture2D`/`texture2DLod` became `texture`/`textureLod`
 *   - `gl_FragColor` became a declared out variable
 *
 * Only applied to sources that name a desktop version. A source with no
 * `#version` is left exactly as it is: it compiles as GLES 1.00, which is what
 * its `COMPAT_` macros are written against, and rewriting it would break it.
 *
 * This is a header swap, NOT a dialect translator, and it does not pretend to
 * be one. Measured against the 609 presets RetroArch ships, 328 run. What
 * stops the rest is not the version line:
 *
 *   - shaders that need lookup textures (over 100 presets) -- see
 *     _loadPresetTextures; they need an image decoder, not a translation
 *   - `##` token pasting, which GLES forbids outright
 *   - `fwidth` and other derivative builtins that need an extension pragma
 *   - non-constant global initializers, which GLES rejects and desktop allows
 *
 * crt-royale is in that last group: ten of its twelve passes declare globals
 * initialized from other globals, which is a 5000-line Cg port's worth of
 * shader work to unpick, not a loader fix. The chain runs it correctly the
 * moment the sources compile.
 */
export function toGlesShader(source, stage) {
  const versionMatch = source.match(/^\s*#version\s+(\d+)(\s+es)?/m);
  if (!versionMatch) return source;              /* no version: already fine as 100 es */
  if (versionMatch[2]) return source;            /* already an es profile */

  const body = source.replace(/^\s*#version\s+\d+.*$/m, '');

  /*
   * `out vec4 FragColor` is declared here rather than left to the shader,
   * because a 130-era fragment shader writes to gl_FragColor, which does not
   * exist in 300 es. Aliasing the name is enough for the overwhelming
   * majority; a shader that declares its own out variable would collide, so
   * only alias when it does not.
   */
  const declaresOut = /^\s*out\s+vec4\s+\w+\s*;/m.test(body);
  const prelude = [
    '#version 300 es',
    'precision highp float;',
    'precision highp int;',
    'precision highp sampler2D;',
    /*
     * Declare what this context can do, rather than editing shaders that ask.
     *
     * Shaders written for the Cg era gate features behind DRIVERS_ALLOW_*
     * switches, shipped commented out because fp30/fp40 profiles could not be
     * assumed. GLES 3 has all of them in core: textureLod, texture() with a
     * bias, and dFdx/dFdy. Leaving them undefined makes a shader #undef its
     * own fast paths and fall back to code paths that were never the intent.
     *
     * This is the loader's job -- the same job RetroArch's own GLES path does.
     * It sets a preprocessor switch the shader author provided for exactly
     * this purpose; it does not touch the shader's code.
     */
    '#define DRIVERS_ALLOW_TEX2DLOD',
    '#define DRIVERS_ALLOW_TEX2DBIAS',
    '#define DRIVERS_ALLOW_DERIVATIVES',
  ];
  if (stage === 'FRAGMENT' && !declaresOut) {
    prelude.push('out vec4 _ab_FragColor;', '#define gl_FragColor _ab_FragColor');
  }

  let out = body
    /* 1.10 spellings, only meaningful in files that declared a desktop version */
    .replace(/\battribute\b/g, 'in')
    .replace(/\bvarying\b/g, stage === 'VERTEX' ? 'out' : 'in')
    /* removed builtins */
    .replace(/\btexture2DLod\b/g, 'textureLod')
    .replace(/\btexture2DProj\b/g, 'textureProj')
    .replace(/\btexture2D\b/g, 'texture')
    .replace(/\btexture3D\b/g, 'texture')
    .replace(/\btextureCube\b/g, 'texture');

  return `${prelude.join('\n')}\n${out}`;
}

/*
 * Read a preset into a plan.
 *
 * `readFile` is injectable so the whole thing can be exercised on synthetic
 * presets with no disk involved.
 */
export function loadPreset(presetPath, { readFile } = {}) {
  const read = readFile ?? ((p) => readFileSync(p, 'utf8'));
  const baseDir = dirname(presetPath);
  const ini = parseIni(read(presetPath));

  /*
   * `#reference "other.glslp"` presets are an indirection-plus-overrides form.
   * Refusing loudly beats half-loading one and rendering something that is not
   * what the file describes.
   */
  if (!ini.has('shaders')) {
    const err = new Error(
      `${presetPath}: no 'shaders' key. ` +
      `(A '#reference' preset that inherits from another file is not supported.)`);
    err.code = 'ABGLSLP_NO_SHADERS';
    throw err;
  }

  const count = Number.parseInt(ini.get('shaders'), 10);
  if (!Number.isFinite(count) || count < 1) {
    throw new Error(`${presetPath}: 'shaders' is '${ini.get('shaders')}', expected a positive integer`);
  }

  const passes = [];
  const parameters = new Map();

  for (let i = 0; i < count; i++) {
    const file = ini.get(`shader${i}`);
    if (!file) throw new Error(`${presetPath}: 'shaders' is ${count} but 'shader${i}' is missing`);
    const path = resolve(baseDir, file);
    if (!existsSync(path)) throw new Error(`${presetPath}: shader${i} not found: ${path}`);
    const source = read(path);

    /*
     * scale_typeN overrides BOTH axes; the per-axis keys are only consulted
     * when it is absent. Getting this precedence wrong silently mis-sizes
     * royale's pass 5, which sets an absolute x and a viewport y.
     */
    const both = getScaleType(ini, `scale_type${i}`);
    const typeX = both ?? getScaleType(ini, `scale_type_x${i}`);
    const typeY = both ?? getScaleType(ini, `scale_type_y${i}`);
    const scale = getFloat(ini, `scale${i}`, 1);

    for (const p of parsePragmaParameters(source)) {
      if (!parameters.has(p.id)) parameters.set(p.id, p);
    }

    passes.push({
      index: i,
      path,
      source,
      alias: ini.get(`alias${i}`) || null,
      scaleTypeX: typeX,
      scaleTypeY: typeY,
      scaleX: getFloat(ini, `scale_x${i}`, scale),
      scaleY: getFloat(ini, `scale_y${i}`, scale),
      /*
       * Absent filter_linear means "use the frontend default" upstream. There
       * is no frontend here, and nearest is the safe answer for pixel art;
       * every pass in a serious preset states it explicitly anyway.
       */
      filterLinear: getBool(ini, `filter_linear${i}`, false),
      /*
       * Both spellings occur: passes 0-9 of royale use `wrap_modeN` and its
       * passes 11-12 use `texture_wrap_modeN`. Reading only one silently drops
       * the other's wrapping.
       */
      wrapMode: getWrap(ini, [`wrap_mode${i}`, `texture_wrap_mode${i}`], 'clamp_to_border'),
      /* Documented default is false; a true default corrupts non-mipmapped passes. */
      mipmapInput: getBool(ini, `mipmap_input${i}`, false),
      floatFramebuffer: getBool(ini, `float_framebuffer${i}`, false),
      srgbFramebuffer: getBool(ini, `srgb_framebuffer${i}`, false),
      frameCountMod: Math.max(0, Number.parseInt(ini.get(`frame_count_mod${i}`) ?? '0', 10) || 0),
    });
  }

  /* LUTs. Each name in `textures` is itself a key holding the path. */
  const textures = [];
  for (const name of splitList(ini.get('textures'))) {
    const file = ini.get(name);
    if (!file) continue;
    const path = resolve(baseDir, file);
    textures.push({
      name,
      path,
      exists: existsSync(path),
      format: extname(path).toLowerCase().replace('.', ''),
      linear: getBool(ini, `${name}_linear`, false),
      wrapMode: getWrap(ini, [`${name}_wrap_mode`], 'clamp_to_border'),
      mipmap: getBool(ini, `${name}_mipmap`, false),
    });
  }

  /*
   * Preset overrides for declared parameters, clamped to the declared range.
   * The `parameters = "..."` list itself is UI hinting and is not consulted:
   * an override works off the id key whether or not the id is listed.
   */
  for (const [id, p] of parameters) {
    if (!ini.has(id)) continue;
    const v = Number.parseFloat(ini.get(id));
    if (Number.isFinite(v)) p.value = Math.min(p.maximum, Math.max(p.minimum, v));
  }

  return { path: presetPath, baseDir, passes, textures, parameters, ini };
}

/*
 * Resolve every pass's target size.
 *
 * The one genuinely surprising rule: a pass with NO scale_type does not
 * default to `source`. Intermediate passes do, but the LAST pass defaults to
 * the viewport at 1.0, ignoring any scale. crt-royale's final pass depends on
 * exactly that, and a loader that defaults everything to `source` renders its
 * last pass at input resolution and stretches it.
 */
export function resolveSizes(passes, { inputWidth, inputHeight, viewportWidth, viewportHeight }) {
  const sizes = [];
  let prevW = inputWidth, prevH = inputHeight;

  for (let i = 0; i < passes.length; i++) {
    const p = passes[i];
    const isLast = i === passes.length - 1;

    const axis = (type, scale, prev, viewport) => {
      switch (type) {
        case SCALE_ABSOLUTE: return Math.max(1, Math.round(scale));
        case SCALE_VIEWPORT: return Math.max(1, Math.round(viewport * scale));
        case SCALE_SOURCE: return Math.max(1, Math.round(prev * scale));
        default: return isLast ? Math.max(1, Math.round(viewport))
                               : Math.max(1, Math.round(prev * scale));
      }
    };

    const width = axis(p.scaleTypeX, p.scaleX, prevW, viewportWidth);
    const height = axis(p.scaleTypeY, p.scaleY, prevH, viewportHeight);
    sizes.push({ width, height });
    prevW = width; prevH = height;
  }
  return sizes;
}

/*
 * Alias -> PassPrev define injection.
 *
 * `aliasN` names a pass so later passes can sample it by name
 * (`<ALIAS>texture`, `<ALIAS>texture_size`, `<ALIAS>video_size`) instead of by
 * index. Rather than binding extra uniforms, map the alias names onto the
 * positional PassPrev uniforms that already exist.
 *
 * Skips any alias the source already defines: crt-royale hand-writes these
 * same defines, and redefining a macro is a compile error.
 */
export function aliasDefines(passes, forIndex) {
  const defines = [];
  const source = passes[forIndex].source;
  for (let i = 0; i < forIndex; i++) {
    const alias = passes[i].alias;
    if (!alias) continue;
    const back = forIndex - i;                 /* PassPrev1 is the previous pass */
    for (const [suffix, uniform] of [
      ['texture', `PassPrev${back}Texture`],
      ['texture_size', `PassPrev${back}TextureSize`],
      ['video_size', `PassPrev${back}InputSize`],
    ]) {
      const name = `${alias}${suffix}`;
      if (source.includes(`#define ${name}`)) continue;
      defines.push(`#define ${name} ${uniform}`);
    }
  }
  return defines;
}
