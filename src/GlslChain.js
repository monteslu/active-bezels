/*
 * Execute a parsed `.glslp` preset as a chain of GL passes.
 *
 * GlslPreset turns a file into a plan; this runs it. The split matters because
 * the plan is testable without a GPU and this is not.
 *
 * The chain owns its own render targets rather than reusing the guest-facing
 * surface pool. Passes need things surfaces do not have -- sRGB targets,
 * mipmapped inputs, per-pass wrap modes -- and every intermediate has to stay
 * live for the whole frame, because a late pass can sample an early one
 * (crt-royale's pass 7 reads back three of them at once). Ping-ponging two
 * buffers is not enough.
 */

import { injectDefines, aliasDefines, toGlesShader } from './GlslPreset.js';

const VERTEX_STAGE = 'VERTEX';
const FRAGMENT_STAGE = 'FRAGMENT';

/*
 * Wrap-mode names as presets spell them, mapped to GL.
 *
 * CLAMP_TO_BORDER is GL_CLAMP_TO_BORDER, which core GLES3 does not have (it
 * arrives via an extension). It is also the DEFAULT wrap in this format, so
 * treating an unsupported value as fatal would reject nearly every preset.
 * Fall back to clamp-to-edge: the visible difference is confined to sampling
 * outside [0,1], where the two disagree only on whether the out-of-range
 * fetch reads the edge texel or the border colour.
 */
function wrapEnum(C, mode) {
  switch (mode) {
    case 'repeat': return C.REPEAT;
    case 'mirrored_repeat': return C.MIRRORED_REPEAT;
    case 'clamp':
    case 'clamp_to_edge': return C.CLAMP_TO_EDGE;
    case 'clamp_to_border': return C.CLAMP_TO_EDGE;
    default: return C.CLAMP_TO_EDGE;
  }
}

export class GlslChain {
  /*
   * `gl`, `C` and `compile` are injected rather than imported so this file has
   * no opinion about how GL was bound, and so the pass-wiring logic can be
   * exercised against a recording double in tests.
   *
   * `compile(vertexSource, fragmentSource)` returns a linked program.
   */
  constructor({ gl, C, compile, preset }) {
    this.gl = gl;
    this.C = C;
    this.compile = compile;
    this.preset = preset;
    this.programs = [];
    this.targets = [];
    this.luts = new Map();
    this.sizes = [];
    this.frameCount = 0;
    this.ready = false;
    this.error = null;
  }

  /*
   * Compile every pass once. A single failure aborts the whole chain: a
   * partially-compiled twelve-pass preset would render something that is not
   * what the preset describes, which is worse than not rendering it.
   */
  build() {
    const { passes } = this.preset;
    try {
      for (let i = 0; i < passes.length; i++) {
        const defines = aliasDefines(this.preset.passes, i);
        /* Stage defines first, then the GLES retarget: the retarget rewrites
         * `varying` differently per stage, so it has to know which stage it is
         * looking at, and the #define is what tells the shader that too. */
        const vs = toGlesShader(injectDefines(passes[i].source, VERTEX_STAGE, defines), VERTEX_STAGE);
        const fs = toGlesShader(injectDefines(passes[i].source, FRAGMENT_STAGE, defines), FRAGMENT_STAGE);
        this.programs.push(this.compile(vs, fs));
      }
      this.ready = true;
    } catch (err) {
      this.error = `pass ${this.programs.length}: ${err.message}`;
      this.dispose();
    }
    return this.ready;
  }

  /*
   * (Re)allocate render targets. Cheap to call every frame: it returns early
   * unless a size actually moved, which happens when the core changes
   * resolution or the on-screen quad is resized.
   */
  resize(sizes) {
    const { gl, C } = this;
    const same = this.targets.length === sizes.length &&
      sizes.every((s, i) => this.sizes[i]?.width === s.width && this.sizes[i]?.height === s.height);
    if (same) return;

    this.disposeTargets();
    this.sizes = sizes.map((s) => ({ ...s }));

    for (let i = 0; i < sizes.length; i++) {
      const pass = this.preset.passes[i];
      const { width, height } = sizes[i];

      /* Out-param style, matching the binding: glGenTextures(n, out). */
      const textureIds = new Uint32Array(1);
      gl.glGenTextures(1, textureIds);
      const texture = textureIds[0];
      gl.glBindTexture(C.TEXTURE_2D, texture);
      /*
       * sRGB targets are load-bearing for royale: nine of its twelve passes
       * request one, and its whole gamma pipeline assumes intermediates are
       * linear. Writing 8-bit UNORM instead does not fail, it just renders
       * wrong, so honour the request where the format exists.
       */
      const internalFormat = pass.srgbFramebuffer ? C.SRGB8_ALPHA8
        : pass.floatFramebuffer ? C.RGBA16F
        : C.RGBA;
      const type = pass.floatFramebuffer ? C.HALF_FLOAT : C.UNSIGNED_BYTE;
      gl.glTexImage2D(C.TEXTURE_2D, 0, internalFormat, width, height, 0, C.RGBA, type, null);

      const min = pass.mipmapInput
        ? (pass.filterLinear ? C.LINEAR_MIPMAP_LINEAR : C.NEAREST_MIPMAP_NEAREST)
        : (pass.filterLinear ? C.LINEAR : C.NEAREST);
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MIN_FILTER, min);
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MAG_FILTER, pass.filterLinear ? C.LINEAR : C.NEAREST);
      const wrap = wrapEnum(C, pass.wrapMode);
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_S, wrap);
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_T, wrap);

      const fboIds = new Uint32Array(1);
      gl.glGenFramebuffers(1, fboIds);
      const fbo = fboIds[0];
      gl.glBindFramebuffer(C.FRAMEBUFFER, fbo);
      gl.glFramebufferTexture2D(C.FRAMEBUFFER, C.COLOR_ATTACHMENT0, C.TEXTURE_2D, texture, 0);
      const status = gl.glCheckFramebufferStatus(C.FRAMEBUFFER);
      gl.glBindFramebuffer(C.FRAMEBUFFER, 0);
      if (status !== C.FRAMEBUFFER_COMPLETE) {
        this.error = `pass ${i}: framebuffer incomplete (0x${status.toString(16)}) at ${width}x${height}`;
        this.ready = false;
        return;
      }
      this.targets.push({ texture, fbo, width, height, mipmap: pass.mipmapInput });
    }
  }

  /*
   * Bind the uniform set a pass expects.
   *
   * Names follow libretro's GLSL contract. Every lookup is unconditional and
   * an absent uniform simply returns -1, which GL ignores -- cheaper and far
   * less brittle than trying to predict which of these a given pass declares.
   */
  bindUniforms(program, index, originalTexture, originalSize, unitBase = 1) {
    const { gl, C } = this;
    const u = (name) => gl.glGetUniformLocation(program, name);
    const size = this.sizes[index];
    const inputSize = index === 0 ? originalSize : this.sizes[index - 1];

    gl.glUniform2f(u('OutputSize'), size.width, size.height);
    gl.glUniform2f(u('InputSize'), inputSize.width, inputSize.height);
    gl.glUniform2f(u('TextureSize'), inputSize.width, inputSize.height);
    gl.glUniform2f(u('OrigInputSize'), originalSize.width, originalSize.height);
    gl.glUniform2f(u('OrigTextureSize'), originalSize.width, originalSize.height);

    const pass = this.preset.passes[index];
    const mod = pass.frameCountMod;
    gl.glUniform1i(u('FrameCount'), mod > 0 ? this.frameCount % mod : this.frameCount);
    gl.glUniform1i(u('FrameDirection'), 1);

    /* Tunables. Global across the preset, pushed to whichever passes declare them. */
    for (const [id, p] of this.preset.parameters) {
      const loc = u(id);
      if (loc !== -1 && loc !== null && loc !== undefined) gl.glUniform1f(loc, p.value);
    }

    let unit = unitBase;

    /* The untouched core output, available to every pass as Orig*. */
    gl.glActiveTexture(C.TEXTURE0 + unit);
    gl.glBindTexture(C.TEXTURE_2D, originalTexture);
    gl.glUniform1i(u('OrigTexture'), unit);
    unit++;

    /*
     * Earlier passes, addressed two ways at once. PassPrev<n> counts backwards
     * from here (PassPrev1 is the pass before this one) and is what royale
     * uses; Pass<n> is the absolute 1-based index. Both are bound because
     * presets in the wild use both.
     */
    for (let i = 0; i < index; i++) {
      const back = index - i;
      const target = this.targets[i];
      if (!target) continue;
      gl.glActiveTexture(C.TEXTURE0 + unit);
      gl.glBindTexture(C.TEXTURE_2D, target.texture);
      gl.glUniform1i(u(`PassPrev${back}Texture`), unit);
      gl.glUniform1i(u(`Pass${i + 1}Texture`), unit);
      gl.glUniform2f(u(`PassPrev${back}InputSize`), target.width, target.height);
      gl.glUniform2f(u(`PassPrev${back}TextureSize`), target.width, target.height);
      gl.glUniform2f(u(`Pass${i + 1}InputSize`), target.width, target.height);
      gl.glUniform2f(u(`Pass${i + 1}TextureSize`), target.width, target.height);
      unit++;
    }

    /* LUTs bind under their own name from the preset. */
    for (const [name, texture] of this.luts) {
      gl.glActiveTexture(C.TEXTURE0 + unit);
      gl.glBindTexture(C.TEXTURE_2D, texture);
      gl.glUniform1i(u(name), unit);
      unit++;
    }
    return unit;
  }

  /*
   * Run the whole chain for one frame.
   *
   * `drawQuad(program)` uploads and draws the full-screen triangle pair with
   * whatever attribute names the caller's geometry helper uses; the chain
   * stays out of vertex-format business.
   *
   * Returns the texture holding the final pass's output, or null if the chain
   * is not runnable. The caller decides what to do with it -- draw it flat,
   * map it onto a curved quad, sample it again -- which is the whole point of
   * ending on a texture rather than on the screen.
   */
  render({ originalTexture, originalWidth, originalHeight, drawQuad }) {
    if (!this.ready || this.targets.length !== this.preset.passes.length) return null;
    const { gl, C } = this;
    const originalSize = { width: originalWidth, height: originalHeight };

    gl.glDisable(C.BLEND);
    for (let i = 0; i < this.programs.length; i++) {
      const target = this.targets[i];
      const program = this.programs[i];

      gl.glBindFramebuffer(C.FRAMEBUFFER, target.fbo);
      gl.glViewport(0, 0, target.width, target.height);
      gl.glClearColor(0, 0, 0, 1);
      gl.glClear(C.COLOR_BUFFER_BIT);
      gl.glUseProgram(program);

      /*
       * Unit 0 is this pass's own input -- the previous pass's output, or the
       * core frame for pass 0. Everything else the pass can reach (Orig,
       * PassPrev, LUTs) is bound from unit 1 up by bindUniforms.
       */
      const inputTexture = i === 0 ? originalTexture : this.targets[i - 1].texture;
      gl.glActiveTexture(C.TEXTURE0);
      gl.glBindTexture(C.TEXTURE_2D, inputTexture);
      gl.glUniform1i(gl.glGetUniformLocation(program, 'Texture'), 0);

      this.bindUniforms(program, i, originalTexture, originalSize, 1);
      drawQuad(program);

      /*
       * Mipmaps are generated on the SOURCE after it is written, not on the
       * consumer before it reads: the level-0 content has to exist first. A
       * pass that asks for a mipmapped input needs the pass feeding it to have
       * a full pyramid, so the flag is read from the consumer and applied to
       * the producer.
       */
      const consumer = this.preset.passes[i + 1];
      if (consumer?.mipmapInput) {
        gl.glBindTexture(C.TEXTURE_2D, target.texture);
        gl.glGenerateMipmap?.(C.TEXTURE_2D);
      }
    }

    gl.glBindFramebuffer(C.FRAMEBUFFER, 0);
    gl.glEnable(C.BLEND);
    this.frameCount++;
    return this.targets[this.targets.length - 1].texture;
  }

  disposeTargets() {
    const { gl } = this;
    for (const t of this.targets) {
      gl.glDeleteFramebuffers?.(1, new Uint32Array([t.fbo]));
      gl.glDeleteTextures?.(1, new Uint32Array([t.texture]));
    }
    this.targets = [];
    this.sizes = [];
  }

  dispose() {
    const { gl } = this;
    this.disposeTargets();
    for (const p of this.programs) gl.glDeleteProgram?.(p);
    this.programs = [];
    for (const t of this.luts.values()) gl.glDeleteTextures?.(1, new Uint32Array([t]));
    this.luts.clear();
    this.ready = false;
  }
}
