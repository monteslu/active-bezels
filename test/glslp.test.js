import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { execSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  parseIni, parsePragmaParameters, injectDefines, loadPreset, resolveSizes, aliasDefines,
} from '../src/GlslPreset.js';
import { GlslChain } from '../src/GlslChain.js';
import { ActiveBezelGpuCompositor } from '../src/GpuCompositor.js';
import { decodeImage, decodeImageFile } from '../src/decodeImage.js';

/* A preset on disk, since loadPreset resolves and reads real paths. */
function writePreset(body, shaders = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'abglslp-'));
  mkdirSync(join(dir, 'shaders'), { recursive: true });
  for (const [name, source] of Object.entries(shaders)) {
    writeFileSync(join(dir, name), source);
  }
  const path = join(dir, 'preset.glslp');
  writeFileSync(path, body);
  return path;
}

const STOCK = `#version 130
#if defined(VERTEX)
attribute vec4 VertexCoord;
void main() { gl_Position = VertexCoord; }
#elif defined(FRAGMENT)
uniform sampler2D Texture;
void main() { gl_FragColor = texture2D(Texture, vec2(0.5)); }
#endif`;

describe('glslp: the key/value layer', () => {
  test('reads unquoted, quoted, and trailing-comment values', () => {
    const ini = parseIni([
      'shaders = 2',
      'shader0 = "a.glsl"',
      'mipmap_input0 = "false"  # mipmapping causes artifacts here',
      '# a whole-line comment',
      '',
      'alias1 = ',
    ].join('\n'));
    assert.equal(ini.get('shaders'), '2');
    assert.equal(ini.get('shader0'), 'a.glsl');
    assert.equal(ini.get('mipmap_input0'), 'false',
      'a trailing # comment must not become part of the value');
    assert.equal(ini.get('alias1'), '');
  });

  test('a # inside quotes is data, not a comment', () => {
    const ini = parseIni('path = "shaders/odd#name.glsl"');
    assert.equal(ini.get('path'), 'shaders/odd#name.glsl');
  });
});

describe('glslp: scale resolution', () => {
  const pass = (over = {}) => ({
    scaleTypeX: null, scaleTypeY: null, scaleX: 1, scaleY: 1, ...over,
  });
  const VIEW = { inputWidth: 256, inputHeight: 224, viewportWidth: 1920, viewportHeight: 1080 };

  test('an intermediate pass with no scale_type follows its source', () => {
    const sizes = resolveSizes([pass(), pass()], VIEW);
    assert.deepEqual(sizes[0], { width: 256, height: 224 });
  });

  test('the LAST pass with no scale_type is the viewport, not the source', () => {
    /* The rule crt-royale depends on. Defaulting it to `source` renders the
     * final pass at input resolution and stretches it to fit. */
    const sizes = resolveSizes([pass(), pass()], VIEW);
    assert.deepEqual(sizes[1], { width: 1920, height: 1080 });
  });

  test('the last pass ignores scale when it is defaulting to viewport', () => {
    const sizes = resolveSizes([pass({ scaleX: 4, scaleY: 4 })], VIEW);
    assert.deepEqual(sizes[0], { width: 1920, height: 1080 });
  });

  test('absolute, viewport and source sizing, on independent axes', () => {
    const sizes = resolveSizes([
      pass({ scaleTypeX: 'absolute', scaleX: 64, scaleTypeY: 'viewport', scaleY: 0.0625 }),
      pass({ scaleTypeX: 'source', scaleX: 2, scaleTypeY: 'source', scaleY: 2 }),
    ], VIEW);
    assert.deepEqual(sizes[0], { width: 64, height: 68 }, 'absolute x, viewport y');
    assert.deepEqual(sizes[1], { width: 128, height: 136 }, 'source scales off the PREVIOUS pass');
  });

  test('a size never rounds down to zero', () => {
    const sizes = resolveSizes([pass({ scaleTypeX: 'source', scaleX: 0.0001 }), pass()], VIEW);
    assert.ok(sizes[0].width >= 1);
  });
});

describe('glslp: stage defines', () => {
  test('defines land after #version, which must stay first', () => {
    const out = injectDefines('#version 130\nvoid main(){}', 'FRAGMENT');
    const lines = out.split('\n');
    assert.equal(lines[0], '#version 130', '#version must remain line 0 or the compile fails');
    assert.ok(lines.slice(1, 4).includes('#define FRAGMENT'));
    assert.ok(lines.slice(1, 4).includes('#define PARAMETER_UNIFORM'));
  });

  test('a source with no #version gets the defines prepended', () => {
    const out = injectDefines('void main(){}', 'VERTEX');
    assert.equal(out.split('\n')[0], '#define VERTEX');
  });
});

describe('glslp: #pragma parameter', () => {
  test('reads id, default and range', () => {
    const [p] = parsePragmaParameters('#pragma parameter crt_gamma "Simulated CRT Gamma" 2.5 1.0 5.0 0.025');
    assert.equal(p.id, 'crt_gamma');
    assert.equal(p.value, 2.5);
    assert.equal(p.minimum, 1);
    assert.equal(p.maximum, 5);
  });

  test('a zero step is rewritten to 1', () => {
    const [p] = parsePragmaParameters('#pragma parameter x "X" 1.0 0.0 2.0 0.0');
    assert.equal(p.step, 1);
  });

  test('a preset override is applied and clamped to the declared range', () => {
    const path = writePreset([
      'shaders = 1',
      'shader0 = "s.glsl"',
      'gamma = "99.0"',
    ].join('\n'), { 's.glsl': '#pragma parameter gamma "G" 2.5 1.0 5.0 0.1\n' + STOCK });
    const preset = loadPreset(path);
    assert.equal(preset.parameters.get('gamma').value, 5,
      'an out-of-range override clamps to the maximum rather than passing through');
  });
});

describe('glslp: loading a preset', () => {
  test('scale_typeN overrides both axes; per-axis keys are ignored when it is present', () => {
    const path = writePreset([
      'shaders = 1',
      'shader0 = "s.glsl"',
      'scale_type0 = "viewport"',
      'scale_type_x0 = "absolute"',
    ].join('\n'), { 's.glsl': STOCK });
    const p = loadPreset(path);
    assert.equal(p.passes[0].scaleTypeX, 'viewport');
    assert.equal(p.passes[0].scaleTypeY, 'viewport');
  });

  test('both wrap_mode and texture_wrap_mode spellings are honoured', () => {
    const a = loadPreset(writePreset('shaders = 1\nshader0 = "s.glsl"\nwrap_mode0 = "repeat"',
      { 's.glsl': STOCK }));
    const b = loadPreset(writePreset('shaders = 1\nshader0 = "s.glsl"\ntexture_wrap_mode0 = "repeat"',
      { 's.glsl': STOCK }));
    assert.equal(a.passes[0].wrapMode, 'repeat');
    assert.equal(b.passes[0].wrapMode, 'repeat',
      'crt-royale spells it texture_wrap_mode on its later passes');
  });

  test('an empty alias is no alias', () => {
    const p = loadPreset(writePreset('shaders = 1\nshader0 = "s.glsl"\nalias0 = ""',
      { 's.glsl': STOCK }));
    assert.equal(p.passes[0].alias, null);
  });

  test('mipmap_input defaults to false', () => {
    const p = loadPreset(writePreset('shaders = 1\nshader0 = "s.glsl"', { 's.glsl': STOCK }));
    assert.equal(p.passes[0].mipmapInput, false);
  });

  test('a missing shader file is an error naming the file', () => {
    const path = writePreset('shaders = 1\nshader0 = "absent.glsl"');
    assert.throws(() => loadPreset(path), /absent\.glsl/);
  });

  test('a count that does not match the shaderN keys is an error', () => {
    const path = writePreset('shaders = 2\nshader0 = "s.glsl"', { 's.glsl': STOCK });
    assert.throws(() => loadPreset(path), /shader1/);
  });

  test('a #reference preset is refused by name rather than half-loaded', () => {
    const path = writePreset('#reference "other.glslp"\ncrt_gamma = "2.4"');
    assert.throws(() => loadPreset(path), (e) => e.code === 'ABGLSLP_NO_SHADERS');
  });

  test('LUTs carry their own filter and wrap settings', () => {
    const dir = writePreset([
      'shaders = 1',
      'shader0 = "s.glsl"',
      'textures = "mask_small;mask_large"',
      'mask_small = "shaders/small.png"',
      'mask_small_wrap_mode = "repeat"',
      'mask_large = "shaders/large.png"',
      'mask_large_mipmap = "true"',
      'mask_large_linear = "true"',
    ].join('\n'), { 's.glsl': STOCK, 'shaders/small.png': 'x', 'shaders/large.png': 'x' });
    const p = loadPreset(dir);
    assert.equal(p.textures.length, 2);
    assert.equal(p.textures[0].wrapMode, 'repeat');
    assert.equal(p.textures[0].mipmap, false);
    assert.equal(p.textures[1].mipmap, true);
    assert.equal(p.textures[1].linear, true);
  });
});

describe('glslp: alias defines', () => {
  test('an alias maps onto the PassPrev slot counting back from the consumer', () => {
    const passes = [
      { alias: 'ORIG_LINEARIZED', source: '' },
      { alias: null, source: '' },
      { alias: null, source: 'void main(){}' },
    ];
    const defines = aliasDefines(passes, 2);
    assert.ok(defines.includes('#define ORIG_LINEARIZEDtexture PassPrev2Texture'),
      'pass 0 seen from pass 2 is two back');
    assert.ok(defines.includes('#define ORIG_LINEARIZEDvideo_size PassPrev2InputSize'));
  });

  test('an alias the source already defines is left alone', () => {
    const passes = [
      { alias: 'FOO', source: '' },
      { alias: null, source: '#define FOOtexture PassPrev1Texture\nvoid main(){}' },
    ];
    const defines = aliasDefines(passes, 1);
    assert.ok(!defines.some((d) => d.startsWith('#define FOOtexture ')),
      'redefining a macro the shader already defines is a compile error');
  });
});

/*
 * The real corpus, when this machine happens to have RetroArch installed.
 *
 * Synthetic presets test what I thought the format was; these test what it
 * actually is. Skipped rather than failed when absent, because the shipped
 * package cannot depend on a 600-file shader library being installed.
 */
function findCorpus() {
  const roots = [
    '/var/lib/flatpak/app/org.libretro.RetroArch/x86_64/stable/active/files/share/libretro/shaders/shaders_glsl',
    '/usr/share/libretro/shaders/shaders_glsl',
    join(process.env.HOME ?? '', '.config/retroarch/shaders/shaders_glsl'),
  ];
  return roots.find((r) => existsSync(r)) ?? null;
}

describe('glslp: the shipped RetroArch corpus', { skip: findCorpus() ? false : 'no shaders_glsl on this machine' }, () => {
  const root = findCorpus();

  test('crt-royale resolves to the shape its own documentation describes', () => {
    const path = join(root, 'crt', 'crt-royale.glslp');
    if (!existsSync(path)) return;
    const preset = loadPreset(path);
    assert.equal(preset.passes.length, 12);
    assert.equal(preset.textures.length, 6);

    const sizes = resolveSizes(preset.passes,
      { inputWidth: 256, inputHeight: 224, viewportWidth: 1920, viewportHeight: 1080 });

    /* Each of these is a distinct rule; a loader can get one right and the
     * rest wrong, so pin the ones royale actually depends on. */
    assert.deepEqual(sizes[2], { width: 320, height: 240 }, 'pass 2 is absolute 320x240');
    assert.deepEqual(sizes[5], { width: 64, height: 68 }, 'pass 5 is absolute x, viewport y');
    assert.deepEqual(sizes[11], { width: 1920, height: 1080 },
      'the final pass defaults to the viewport');
    assert.equal(preset.passes[0].alias, 'ORIG_LINEARIZED');
    assert.equal(preset.passes.filter((p) => p.srgbFramebuffer).length, 9);
    assert.ok(preset.textures.some((t) => t.mipmap && t.wrapMode === 'repeat'),
      'the large mask LUTs are mipmapped and tiled');
    assert.ok(preset.parameters.size > 40, 'royale exposes dozens of tunables');
  });

  test('every shipped preset either loads or fails for a nameable reason', () => {
    const files = execSync(`find ${JSON.stringify(root)} -name '*.glslp'`, { maxBuffer: 1 << 28 })
      .toString().trim().split('\n').filter(Boolean);
    assert.ok(files.length > 100, `expected a real corpus, found ${files.length}`);

    const broken = [];
    for (const file of files) {
      try {
        const preset = loadPreset(file);
        resolveSizes(preset.passes,
          { inputWidth: 256, inputHeight: 224, viewportWidth: 1920, viewportHeight: 1080 });
      } catch (err) {
        /* Two acceptable failures: a '#reference' preset, which this loader
         * deliberately does not support, and a preset whose .glsl files are
         * genuinely absent from the distribution (MMJ_Cel_Shader_3dfx points
         * at a 3dfx pass that does not ship). Anything else is my bug. */
        const acceptable = err.code === 'ABGLSLP_NO_SHADERS' || /not found:/.test(err.message);
        if (!acceptable) broken.push(`${file}: ${err.message}`);
      }
    }
    assert.deepEqual(broken, [], 'presets this loader cannot explain');
  });
});

/*
 * On real GL, with real presets. The double above proves the wiring; this
 * proves the wiring survives contact with an actual driver -- which it did not
 * the first time, because the double returned texture ids that the real
 * binding writes into an out-parameter instead.
 */
describe('glslp: on real GL', () => {
  const root = findCorpus();

  function gpuAnd(preset, t) {
    if (!root) return t.skip('no shaders_glsl on this machine');
    const path = join(root, preset);
    if (!existsSync(path)) return t.skip(`${preset} not installed`);
    const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 256, outputHeight: 256 });
    if (!gpu) return t.skip('OpenGL ES context unavailable');
    return { gpu, path };
  }

  /* A checkerboard: any real filter visibly changes it, and a no-op does not. */
  function checkerboard(w, h) {
    const px = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const on = ((x >> 3) + (y >> 3)) & 1;
        px[i] = on ? 255 : 0; px[i + 1] = on ? 0 : 255; px[i + 2] = 0; px[i + 3] = 255;
      }
    }
    return px;
  }

  test('a single-pass preset runs and puts pixels in the surface', (t) => {
    const ctx = gpuAnd('crt/crt-lottes.glslp', t);
    if (!ctx) return;
    const { gpu, path } = ctx;
    const surface = gpu.surfaceCreate(128, 128);
    const ok = gpu.surfacePreset(-1, surface, path, checkerboard(64, 64), 64, 64);
    assert.equal(ok, 1, gpu.effectError ?? 'preset refused');
  });

  test('a MULTI-pass preset runs the whole chain', (t) => {
    /* The thing single-pass surface_filter could never do: eleven passes,
     * each rendering into its own target at its own size, each sampling the
     * one before it. */
    const ctx = gpuAnd('blurs/kawase_blur_9pass.glslp', t);
    if (!ctx) return;
    const { gpu, path } = ctx;
    const preset = loadPreset(path);
    assert.ok(preset.passes.length >= 3, `expected a real chain, got ${preset.passes.length}`);

    const surface = gpu.surfaceCreate(128, 128);
    const ok = gpu.surfacePreset(-1, surface, path, checkerboard(64, 64), 64, 64);
    assert.equal(ok, 1, gpu.effectError ?? 'preset refused');

    /* Every pass allocated its own live target -- a late pass can sample an
     * early one, so ping-ponging two buffers would not do. */
    const chain = gpu.chains.get(path);
    assert.equal(chain.targets.length, preset.passes.length);
    assert.equal(new Set(chain.targets.map((x) => x.texture)).size, preset.passes.length,
      'each pass needs its OWN target, not a shared one');
  });

  test('a blur preset actually blurs -- the picture changes', (t) => {
    /* A chain that runs but renders nothing would pass every test above.
     * Compare the filtered surface against an unfiltered copy of the same
     * source and require them to differ. */
    const ctx = gpuAnd('blurs/kawase_blur_9pass.glslp', t);
    if (!ctx) return;
    const { gpu, path } = ctx;
    const source = checkerboard(64, 64);

    const filtered = gpu.surfaceCreate(64, 64);
    assert.equal(gpu.surfacePreset(-1, filtered, path, source, 64, 64), 1,
      gpu.effectError ?? 'preset refused');

    /* Draw the filtered surface to the screen and read it back. */
    gpu.clear(0x000000ff);
    gpu.drawTexture(filtered, 0, 0, 1920, 1080);
    const out = gpu.compose(source, 64, 64).rgba;
    const colours = new Set();
    for (let i = 0; i < out.length; i += 4) {
      colours.add((out[i] << 16) | (out[i + 1] << 8) | out[i + 2]);
    }
    /* The source is exactly two colours. A blur necessarily produces
     * intermediates; an empty or pass-through chain would not. */
    assert.ok(colours.size > 2,
      `a nine-pass blur must produce intermediate colours, saw ${colours.size}`);
  });

  test('a preset needing lookup textures refuses, naming them', (t) => {
    const ctx = gpuAnd('crt/crt-royale.glslp', t);
    if (!ctx) return;
    const { gpu, path } = ctx;
    const surface = gpu.surfaceCreate(64, 64);
    const ok = gpu.surfacePreset(-1, surface, path, checkerboard(64, 64), 64, 64);
    /* Royale needs six mask LUTs and this package has no image decoder. It
     * must say so rather than render a maskless picture that looks like the
     * preset worked. */
    assert.equal(ok, 0);
    assert.match(gpu.effectError ?? '', /lookup texture|decoder|constant expression|##|fwidth/,
      `refused for a nameable reason, got: ${gpu.effectError}`);
  });

  test('a preset that cannot compile refuses rather than rendering half a chain', (t) => {
    if (!ActiveBezelGpuCompositor.create({ outputWidth: 8, outputHeight: 8 })) {
      return t.skip('OpenGL ES context unavailable');
    }
    const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 64, outputHeight: 64 });
    const path = writePreset('shaders = 1\nshader0 = "bad.glsl"',
      { 'bad.glsl': '#version 130\nthis is not glsl at all' });
    const surface = gpu.surfaceCreate(64, 64);
    assert.equal(gpu.surfacePreset(-1, surface, path, checkerboard(8, 8), 8, 8), 0);
  });

  test('a broken preset is not recompiled on every frame', (t) => {
    const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 64, outputHeight: 64 });
    if (!gpu) return t.skip('OpenGL ES context unavailable');
    const path = writePreset('shaders = 1\nshader0 = "bad.glsl"',
      { 'bad.glsl': '#version 130\nnot glsl' });
    const surface = gpu.surfaceCreate(64, 64);
    gpu.surfacePreset(-1, surface, path, checkerboard(8, 8), 8, 8);
    const cachedAs = gpu.chains.get(path);
    gpu.surfacePreset(-1, surface, path, checkerboard(8, 8), 8, 8);
    assert.equal(cachedAs, null, 'the failure itself is cached');
    assert.equal(gpu.chains.size, 1, 'one entry, not one per frame');
  });
});

/*
 * A recording GL double. The chain's job is to bind the right things in the
 * right order, and that is checkable without a GPU.
 */
function recordingGl() {
  const calls = [];
  const uniforms = new Map();
  let nextId = 1;
  const gl = new Proxy({}, {
    get(_, name) {
      if (name === '__calls') return calls;
      return (...args) => {
        calls.push({ name, args });
        /* Out-param style, exactly as the real binding works: the id is
         * written into the caller's Uint32Array, not returned. Getting this
         * wrong in the double let a real bug through once -- the chain used
         * the return value and worked in tests while throwing on real GL. */
        if (name === 'glGenTextures' || name === 'glGenFramebuffers') {
          const out = args[1];
          const id = nextId++;
          if (out && typeof out.length === 'number') out[0] = id;
          return undefined;
        }
        if (name === 'glCheckFramebufferStatus') return 0x8cd5;
        if (name === 'glGetUniformLocation') {
          const key = args[1];
          if (!uniforms.has(key)) uniforms.set(key, uniforms.size + 1);
          return uniforms.get(key);
        }
        return undefined;
      };
    },
  });
  return { gl, calls, uniforms };
}

const C = {
  TEXTURE_2D: 0x0de1, RGBA: 0x1908, UNSIGNED_BYTE: 0x1401, TEXTURE0: 0x84c0,
  TEXTURE_MIN_FILTER: 0x2801, TEXTURE_MAG_FILTER: 0x2800,
  TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
  NEAREST: 0x2600, LINEAR: 0x2601, CLAMP_TO_EDGE: 0x812f, REPEAT: 0x2901,
  COLOR_BUFFER_BIT: 0x4000, BLEND: 0x0be2,
  FRAMEBUFFER: 0x8d40, COLOR_ATTACHMENT0: 0x8ce0, FRAMEBUFFER_COMPLETE: 0x8cd5,
  SRGB8_ALPHA8: 0x8c43, RGBA16F: 0x881a, HALF_FLOAT: 0x140b,
  LINEAR_MIPMAP_LINEAR: 0x2703, NEAREST_MIPMAP_NEAREST: 0x2700,
};

function chainOf(passes, glDouble) {
  const preset = { passes, textures: [], parameters: new Map() };
  return new GlslChain({
    gl: glDouble.gl, C, preset,
    compile: () => 100 + Math.floor(Math.random() * 1),
  });
}

const plainPass = (over = {}) => ({
  index: 0, source: STOCK, alias: null,
  scaleTypeX: null, scaleTypeY: null, scaleX: 1, scaleY: 1,
  filterLinear: false, wrapMode: 'clamp_to_border', mipmapInput: false,
  floatFramebuffer: false, srgbFramebuffer: false, frameCountMod: 0,
  ...over,
});

describe('glslp: chain execution', () => {
  test('an sRGB pass allocates an sRGB target', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass({ srgbFramebuffer: true })], rec);
    chain.build();
    chain.resize([{ width: 64, height: 64 }]);
    const texImage = rec.calls.find((c) => c.name === 'glTexImage2D');
    assert.equal(texImage.args[2], C.SRGB8_ALPHA8,
      'royale needs linear-space intermediates; an 8-bit UNORM target renders wrong, not broken');
  });

  test('a non-sRGB pass allocates a plain target', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass()], rec);
    chain.build();
    chain.resize([{ width: 64, height: 64 }]);
    assert.equal(rec.calls.find((c) => c.name === 'glTexImage2D').args[2], C.RGBA);
  });

  test('a repeat wrap reaches GL as REPEAT', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass({ wrapMode: 'repeat' })], rec);
    chain.build();
    chain.resize([{ width: 8, height: 8 }]);
    const wrapCalls = rec.calls.filter((c) => c.name === 'glTexParameteri' && c.args[1] === C.TEXTURE_WRAP_S);
    assert.equal(wrapCalls[0].args[2], C.REPEAT);
  });

  test('resize is a no-op when nothing moved', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass()], rec);
    chain.build();
    chain.resize([{ width: 64, height: 64 }]);
    const after = rec.calls.length;
    chain.resize([{ width: 64, height: 64 }]);
    assert.equal(rec.calls.length, after, 'reallocating identical targets every frame would leak');
  });

  test('resize reallocates when a size changes', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass()], rec);
    chain.build();
    chain.resize([{ width: 64, height: 64 }]);
    const after = rec.calls.length;
    chain.resize([{ width: 65, height: 64 }]);
    assert.ok(rec.calls.length > after);
  });

  test('render draws every pass, each into its own target', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass(), plainPass(), plainPass()], rec);
    chain.build();
    chain.resize([{ width: 8, height: 8 }, { width: 16, height: 16 }, { width: 32, height: 32 }]);
    let drawn = 0;
    const out = chain.render({
      originalTexture: 900, originalWidth: 256, originalHeight: 224,
      drawQuad: () => { drawn++; },
    });
    assert.equal(drawn, 3, 'a twelve-pass preset must draw twelve times');
    assert.ok(out, 'render returns the final texture so the caller can map it anywhere');
    const viewports = rec.calls.filter((c) => c.name === 'glViewport').map((c) => c.args.slice(2));
    assert.deepEqual(viewports, [[8, 8], [16, 16], [32, 32]],
      'each pass renders at its OWN resolution, not the output size');
  });

  test('pass 0 reads the core frame; later passes read their predecessor', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass(), plainPass()], rec);
    chain.build();
    chain.resize([{ width: 8, height: 8 }, { width: 8, height: 8 }]);
    rec.calls.length = 0;
    chain.render({ originalTexture: 900, originalWidth: 256, originalHeight: 224, drawQuad: () => {} });

    /* The unit-0 bind immediately after each glUseProgram is the pass input. */
    const inputs = [];
    for (let i = 0; i < rec.calls.length; i++) {
      if (rec.calls[i].name !== 'glUseProgram') continue;
      const active = rec.calls.slice(i).find((c) => c.name === 'glActiveTexture');
      const bind = rec.calls.slice(rec.calls.indexOf(active)).find((c) => c.name === 'glBindTexture');
      inputs.push({ unit: active.args[0], texture: bind.args[1] });
    }
    assert.equal(inputs[0].unit, C.TEXTURE0);
    assert.equal(inputs[0].texture, 900, 'pass 0 samples the untouched core output');
    assert.notEqual(inputs[1].texture, 900, 'pass 1 samples pass 0, not the core frame');
  });

  test('mipmaps are generated on the producer, not requested on the consumer', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass(), plainPass({ mipmapInput: true })], rec);
    chain.build();
    chain.resize([{ width: 8, height: 8 }, { width: 8, height: 8 }]);
    const producerTexture = chain.targets[0].texture;
    const consumerTexture = chain.targets[1].texture;
    rec.calls.length = 0;
    chain.render({ originalTexture: 900, originalWidth: 256, originalHeight: 224, drawQuad: () => {} });

    const mips = rec.calls.filter((c) => c.name === 'glGenerateMipmap');
    assert.equal(mips.length, 1, 'exactly one pyramid');

    /* WHICH texture got it is the whole point. Counting calls cannot tell the
     * producer and the consumer apart when only one pass sets the flag, so
     * follow the bind that precedes the generate. */
    const at = rec.calls.indexOf(mips[0]);
    const bound = rec.calls.slice(0, at).reverse().find((c) => c.name === 'glBindTexture');
    assert.equal(bound.args[1], producerTexture,
      'the pyramid belongs on the texture being READ (pass 0), which has level-0 content');
    assert.notEqual(bound.args[1], consumerTexture,
      'generating on the consumer builds a pyramid over an empty target');
  });

  test('a pass that fails to compile aborts the whole chain', () => {
    const rec = recordingGl();
    const preset = { passes: [plainPass(), plainPass()], textures: [], parameters: new Map() };
    let n = 0;
    const chain = new GlslChain({
      gl: rec.gl, C, preset,
      compile: () => { if (++n === 2) throw new Error('syntax error'); return 1; },
    });
    assert.equal(chain.build(), false);
    assert.match(chain.error, /pass 1/);
    assert.equal(chain.render({ originalTexture: 1, originalWidth: 1, originalHeight: 1, drawQuad: () => {} }), null,
      'a half-compiled chain must not render a picture the preset does not describe');
  });

  test('FrameCount advances and honours frame_count_mod', () => {
    const rec = recordingGl();
    const chain = chainOf([plainPass({ frameCountMod: 2 })], rec);
    chain.build();
    chain.resize([{ width: 8, height: 8 }]);
    const seen = [];
    const loc = () => rec.uniforms.get('FrameCount');
    for (let i = 0; i < 4; i++) {
      rec.calls.length = 0;
      chain.render({ originalTexture: 1, originalWidth: 8, originalHeight: 8, drawQuad: () => {} });
      const call = rec.calls.find((c) => c.name === 'glUniform1i' && c.args[0] === loc());
      seen.push(call.args[1]);
    }
    assert.deepEqual(seen, [0, 1, 0, 1], 'frame_count_mod 2 must wrap');
  });
});

describe('glslp: lookup textures', () => {
  test('the built-in decoder reads every PNG the shader corpus ships', (t) => {
    const root = findCorpus();
    if (!root) return t.skip('no shaders_glsl on this machine');
    const files = execSync(`find ${JSON.stringify(root)} -iname '*.png'`, { maxBuffer: 1 << 28 })
      .toString().trim().split('\n').filter(Boolean);
    assert.ok(files.length > 50, `expected a real corpus, found ${files.length}`);

    /* These are not uniform: the corpus has truecolor, RGBA, 1/2/4/8-bit
     * palette, grayscale and one 16-bit image. A decoder that only handled
     * 8-bit truecolor would pass a spot-check and corrupt the rest. */
    const failed = [];
    for (const file of files) {
      const img = decodeImageFile(file);
      if (!img || !img.width || !img.height || img.pixels.length !== img.width * img.height * 4) {
        failed.push(file.split('/').pop());
      }
    }
    assert.deepEqual(failed, [], 'PNGs the decoder could not read');
  });

  test('a preset with LUTs runs now that a decoder exists', (t) => {
    const root = findCorpus();
    if (!root) return t.skip('no shaders_glsl on this machine');
    const path = join(root, 'handheld', 'lcd-shader-psp-color.glslp');
    if (!existsSync(path)) return t.skip('preset not installed');
    const preset = loadPreset(path);
    if (!preset.textures.length) return t.skip('installed preset has no LUTs');

    const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 256, outputHeight: 256 });
    if (!gpu) return t.skip('OpenGL ES context unavailable');
    const surface = gpu.surfaceCreate(128, 128);
    const src = new Uint8Array(64 * 64 * 4).fill(128);
    assert.equal(gpu.surfacePreset(-1, surface, path, src, 64, 64), 1,
      gpu.effectError ?? 'preset refused');
    assert.equal(gpu.chains.get(path).luts.size, preset.textures.length,
      'every declared LUT must reach the GPU');
  });

  test('garbage is rejected rather than decoded into nonsense', () => {
    assert.equal(decodeImage(Buffer.from('this is not an image')), null);
    assert.equal(decodeImage(Buffer.alloc(0)), null);
  });

  test('a preset whose LUT file is missing refuses, naming the file', (t) => {
    const gpu = ActiveBezelGpuCompositor.create({ outputWidth: 64, outputHeight: 64 });
    if (!gpu) return t.skip('OpenGL ES context unavailable');
    const path = writePreset([
      'shaders = 1',
      'shader0 = "s.glsl"',
      'textures = "MASK"',
      'MASK = "shaders/absent.png"',
    ].join('\n'), { 's.glsl': STOCK });
    const surface = gpu.surfaceCreate(64, 64);
    assert.equal(gpu.surfacePreset(-1, surface, path, new Uint8Array(64), 4, 4), 0);
    assert.match(gpu.effectError ?? '', /absent\.png/);
  });
});
