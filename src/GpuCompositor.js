import { createRequire } from 'node:module';
import {
  ActiveBezelCompositor, LOGICAL_WIDTH, LOGICAL_HEIGHT, forEachGlyphRect,
} from './Compositor.js';

/*
 * `native-gles` is a REQUIRED dependency -- every consumer of this package
 * (romdev, romdeck, retroemu) needs it to run -- but it is still bound lazily
 * rather than imported at the top.
 *
 * The reason is failure mode, not optionality. It is a native addon, so it can
 * be present-but-unloadable: a prebuild missing for the platform, a botched
 * install script, a mismatched Node ABI. A static import turns any of those
 * into "this module cannot be imported", which takes down package loading,
 * manifest validation and the CPU compositor along with it -- none of which
 * need GL.
 *
 * Bound here, the same failures land inside create()'s existing try/catch and
 * take the path that already existed for a failed GL context: create() returns
 * null and the caller falls back to the CPU compositor. A machine that cannot
 * do GL still validates, packs and composites; it just does it on the CPU.
 * (The dev machine already hits this -- GL context creation fails under X11
 * SDL there and packages run on the CPU compositor.)
 */
let gl = null;

function requireGl() {
  if (gl) return gl;
  const req = createRequire(import.meta.url);
  const mod = req('native-gles');
  gl = mod?.default ?? mod;
  return gl;
}

const C = {
  VERTEX_SHADER: 0x8b31, FRAGMENT_SHADER: 0x8b30,
  COMPILE_STATUS: 0x8b81, LINK_STATUS: 0x8b82,
  ARRAY_BUFFER: 0x8892, STREAM_DRAW: 0x88e0, FLOAT: 0x1406,
  TRIANGLES: 0x0004, TEXTURE_2D: 0x0de1, RGBA: 0x1908,
  UNSIGNED_BYTE: 0x1401, TEXTURE0: 0x84c0,
  TEXTURE_MIN_FILTER: 0x2801, TEXTURE_MAG_FILTER: 0x2800,
  TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
  NEAREST: 0x2600, LINEAR: 0x2601, CLAMP_TO_EDGE: 0x812f,
  COLOR_BUFFER_BIT: 0x4000, BLEND: 0x0be2,
  SRC_ALPHA: 0x0302, ONE_MINUS_SRC_ALPHA: 0x0303,
  SCISSOR_TEST: 0x0c11,
  FRAMEBUFFER: 0x8d40, COLOR_ATTACHMENT0: 0x8ce0,
  FRAMEBUFFER_COMPLETE: 0x8cd5,
};

function compile(type, source) {
  const shader = gl.glCreateShader(type);
  gl.glShaderSource(shader, source);
  gl.glCompileShader(shader);
  if (!gl.glGetShaderiv(shader, C.COMPILE_STATUS)) {
    throw new Error(String(gl.glGetShaderInfoLog(shader) || 'shader compile failed'));
  }
  return shader;
}

/* Vertex format for meshes: x, y, u, v, r, g, b, a, w (9 floats). */
function programVC(fragment) {
  const vertex = compile(C.VERTEX_SHADER, `#version 300 es
    in vec2 a_position;
    in vec2 a_uv;
    in vec4 a_color;
    in float a_w;
    out vec2 v_uv;
    out vec4 v_color;
    void main() {
      /* a_w is the perspective divisor quad() computes; premultiplying the
       * clip position by it and letting the rasteriser divide is exactly
       * what makes GL interpolate u/w, v/w and 1/w -- perspective-correct
       * texturing for free. With w = 1 (every ordinary mesh) this is the
       * identity, so nothing else changes. */
      gl_Position = vec4(a_position.x * a_w, -a_position.y * a_w, 0.0, a_w);
      v_uv = a_uv;
      v_color = a_color;
    }`);
  const pixel = compile(C.FRAGMENT_SHADER, fragment);
  const result = gl.glCreateProgram();
  gl.glAttachShader(result, vertex);
  gl.glAttachShader(result, pixel);
  gl.glLinkProgram(result);
  gl.glDeleteShader(vertex);
  gl.glDeleteShader(pixel);
  if (!gl.glGetProgramiv(result, C.LINK_STATUS)) {
    throw new Error(String(gl.glGetProgramInfoLog(result) || 'program link failed'));
  }
  return result;
}

function program(fragment) {
  const vertex = compile(C.VERTEX_SHADER, `#version 300 es
    in vec2 a_position;
    in vec2 a_uv;
    out vec2 v_uv;
    void main() {
      gl_Position = vec4(a_position.x, -a_position.y, 0.0, 1.0);
      v_uv = a_uv;
    }`);
  const pixel = compile(C.FRAGMENT_SHADER, fragment);
  const result = gl.glCreateProgram();
  gl.glAttachShader(result, vertex);
  gl.glAttachShader(result, pixel);
  gl.glLinkProgram(result);
  gl.glDeleteShader(vertex);
  gl.glDeleteShader(pixel);
  if (!gl.glGetProgramiv(result, C.LINK_STATUS)) {
    throw new Error(String(gl.glGetProgramInfoLog(result) || 'program link failed'));
  }
  return result;
}

function rgba(color) {
  return [
    ((color >>> 24) & 255) / 255,
    ((color >>> 16) & 255) / 255,
    ((color >>> 8) & 255) / 255,
    (color & 255) / 255,
  ];
}

function xy(x, y) {
  return [x / LOGICAL_WIDTH * 2 - 1, y / LOGICAL_HEIGHT * 2 - 1];
}

/*
 * A textured quad. `uv` is the SOURCE sub-rectangle in normalised texture
 * coordinates, defaulting to the whole texture.
 *
 * Without this the UVs were hardcoded 0..1, so every atlas blit stretched the
 * ENTIRE sheet into one small cell -- on the GPU path a tile renderer came out
 * as noise while the CPU path (which honours sx/sy/sw/sh in blitNearest) was
 * correct. The two backends disagreeing is exactly what the pixel-equality
 * test exists to catch, and it only caught it once a real package used it.
 */
/*
 * Snap a logical rect to the device-pixel grid the CPU rasteriser uses
 * (floor of the left/top edge, ceil of the right/bottom), expressed back in
 * logical units. GL fills only pixels whose CENTRE is covered, so without this
 * any rect whose edge falls mid-pixel loses that column or row on the GPU and
 * keeps it on the CPU. Same fix as the glyph cells.
 */
function snapRect(x, y, w, h, dw, dh) {
  if (!(dw > 0) || !(dh > 0)) return [x, y, w, h];
  const sx = dw / LOGICAL_WIDTH;
  const sy = dh / LOGICAL_HEIGHT;
  const px0 = Math.floor(x * sx); const py0 = Math.floor(y * sy);
  const px1 = Math.ceil((x + w) * sx); const py1 = Math.ceil((y + h) * sy);
  return [px0 / sx, py0 / sy, (px1 - px0) / sx, (py1 - py0) / sy];
}

function quad(x, y, w, h, uv) {
  const [x0, y0] = xy(x, y);
  const [x1, y1] = xy(x + w, y + h);
  const u0 = uv ? uv.u0 : 0, v0 = uv ? uv.v0 : 0;
  const u1 = uv ? uv.u1 : 1, v1 = uv ? uv.v1 : 1;
  return new Float32Array([
    x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1,
    x0, y1, u0, v1, x1, y0, u1, v0, x1, y1, u1, v1,
  ]);
}

/* Normalised source rect for a command that may carry sx/sy/sw/sh. */
function sourceUv(command, sourceWidth, sourceHeight) {
  if (!(command.sw > 0) || !(command.sh > 0)) return null;
  return {
    u0: command.sx / sourceWidth,
    v0: command.sy / sourceHeight,
    u1: (command.sx + command.sw) / sourceWidth,
    v1: (command.sy + command.sh) / sourceHeight,
  };
}

export class ActiveBezelGpuCompositor extends ActiveBezelCompositor {
  static create(options) {
    const compositor = new ActiveBezelGpuCompositor(options);
    try {
      gl = requireGl();
      compositor.init();
      return compositor;
    } catch (err) {
      compositor.destroy();
      process.env.RETROEMU_DEBUG && console.error(`[active-bezel] GPU fallback: ${err.message}`);
      return null;
    }
  }

  constructor(options) {
    super(options);
    this.gpuReady = false;
    this.gpuTextures = new Map();
  }

  init() {
    if (!gl.createContext(this.outputWidth, this.outputHeight)) throw new Error('no OpenGL ES context');
    this.contextOwned = true;
    gl.makeCurrent?.();
    this.colorProgram = program(`#version 300 es
      precision mediump float;
      uniform vec4 u_color;
      out vec4 out_color;
      void main() { out_color = u_color; }`);
    this.meshProgram = programVC(`#version 300 es
      precision mediump float;
      in vec4 v_color;
      out vec4 out_color;
      void main() { out_color = v_color; }`);
    this.meshTextureProgram = programVC(`#version 300 es
      precision mediump float;
      uniform sampler2D u_texture;
      in vec2 v_uv;
      in vec4 v_color;
      out vec4 out_color;
      /* texture * vertex colour: same modulation as the CPU rasteriser. */
      void main() { out_color = texture(u_texture, v_uv) * v_color; }`);
    this.textureProgram = program(`#version 300 es
      precision mediump float;
      uniform sampler2D u_texture;
      in vec2 v_uv;
      out vec4 out_color;
      void main() { out_color = texture(u_texture, v_uv); }`);
    const vao = new Uint32Array(1);
    const vbo = new Uint32Array(1);
    gl.glGenVertexArrays(1, vao);
    gl.glGenBuffers(1, vbo);
    this.vao = vao[0];
    this.vbo = vbo[0];
    this.gameTexture = this._newTexture();
    this.readback = new Uint8Array(this.output.length);

    /*
     * Offscreen scene target.
     *
     * The scene is rendered HERE rather than straight to the default
     * framebuffer, so a picture effect can sample the finished composition as
     * a texture and write the filtered result. Without an FBO there is nothing
     * for a shader to read: you cannot sample the framebuffer you are drawing
     * into. The final readback then happens from whichever target ran last, so
     * screenshots, frame hashes and the livestream all still observe the same
     * pixels the effect produced -- which is what the format promises.
     */
    this.sceneTexture = this._newTexture();
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MIN_FILTER, C.LINEAR);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MAG_FILTER, C.LINEAR);
    gl.glTexImage2D(C.TEXTURE_2D, 0, C.RGBA, this.outputWidth, this.outputHeight,
      0, C.RGBA, C.UNSIGNED_BYTE, null);
    const fbo = new Uint32Array(1);
    gl.glGenFramebuffers(1, fbo);
    this.sceneFbo = fbo[0];
    gl.glBindFramebuffer(C.FRAMEBUFFER, this.sceneFbo);
    gl.glFramebufferTexture2D(C.FRAMEBUFFER, C.COLOR_ATTACHMENT0, C.TEXTURE_2D, this.sceneTexture, 0);
    if (gl.glCheckFramebufferStatus(C.FRAMEBUFFER) !== C.FRAMEBUFFER_COMPLETE) {
      gl.glBindFramebuffer(C.FRAMEBUFFER, 0);
      throw new Error('scene framebuffer incomplete');
    }
    gl.glBindFramebuffer(C.FRAMEBUFFER, 0);

    this.effect = null;
    this.gpuReady = true;
  }

  /*
   * Compile a picture-effect fragment shader.
   *
   * The author supplies GLSL ES 3.00 fragment source with `v_uv` in and
   * `out_color` out, and may sample `u_texture` (the composed scene) plus the
   * uniforms below. A compile failure is reported and the effect is DROPPED
   * rather than throwing -- a bad shader must not take down an emulation
   * session mid-play, and the unfiltered picture is a truthful fallback.
   */
  setEffect(source) {
    if (!this.gpuReady) return 0;
    if (this.effect) { gl.glDeleteProgram(this.effect.program); this.effect = null; }
    if (!source) return 1;
    try {
      this.effect = { program: program(source) };
      return 1;
    } catch (err) {
      this.effect = null;
      process.env.RETROEMU_DEBUG && console.error(`[active-bezel] effect: ${err.message}`);
      this.effectError = String(err.message || err);
      return 0;
    }
  }

  _newTexture() {
    const ids = new Uint32Array(1);
    gl.glGenTextures(1, ids);
    gl.glBindTexture(C.TEXTURE_2D, ids[0]);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_S, C.CLAMP_TO_EDGE);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_T, C.CLAMP_TO_EDGE);
    return ids[0];
  }

  createTexture(pixels, width, height) {
    const handle = super.createTexture(pixels, width, height);
    if (handle && this.gpuReady) this._uploadPersistent(handle);
    return handle;
  }

  _uploadPersistent(handle) {
    const texture = this.textures.get(handle);
    if (!texture) return;
    const id = this._newTexture();
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MIN_FILTER, C.NEAREST);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MAG_FILTER, C.NEAREST);
    gl.glTexImage2D(C.TEXTURE_2D, 0, C.RGBA, texture.width, texture.height,
      0, C.RGBA, C.UNSIGNED_BYTE, texture.pixels);
    this.gpuTextures.set(handle, id);
  }

  destroyTexture(handle) {
    const id = this.gpuTextures.get(handle);
    if (id) gl.glDeleteTextures(1, new Uint32Array([id]));
    this.gpuTextures.delete(handle);
    return super.destroyTexture(handle);
  }

  _geometry(programId, vertices) {
    gl.glUseProgram(programId);
    gl.glBindVertexArray(this.vao);
    gl.glBindBuffer(C.ARRAY_BUFFER, this.vbo);
    gl.glBufferData(C.ARRAY_BUFFER, new Uint8Array(vertices.buffer), C.STREAM_DRAW);
    const pos = gl.glGetAttribLocation(programId, 'a_position');
    const uv = gl.glGetAttribLocation(programId, 'a_uv');
    if (pos >= 0) {
      gl.glEnableVertexAttribArray(pos);
      gl.glVertexAttribPointer(pos, 2, C.FLOAT, false, 16, 0);
    }
    if (uv >= 0) {
      gl.glEnableVertexAttribArray(uv);
      gl.glVertexAttribPointer(uv, 2, C.FLOAT, false, 16, 8);
    }
  }

  _geometryVC(programId, vertices) {
    gl.glUseProgram(programId);
    gl.glBindVertexArray(this.vao);
    gl.glBindBuffer(C.ARRAY_BUFFER, this.vbo);
    gl.glBufferData(C.ARRAY_BUFFER, new Uint8Array(vertices.buffer), C.STREAM_DRAW);
    const stride = 36;                 /* 9 floats: xy, uv, rgba, w */
    const bind = (name, size, offset) => {
      const loc = gl.glGetAttribLocation(programId, name);
      if (loc >= 0) {
        gl.glEnableVertexAttribArray(loc);
        gl.glVertexAttribPointer(loc, size, C.FLOAT, false, stride, offset);
      }
    };
    bind('a_position', 2, 0);
    bind('a_uv', 2, 8);
    bind('a_color', 4, 16);
    bind('a_w', 1, 32);
  }

  /* Triangles with per-vertex colour (and optional texture), one draw call. */
  _drawMesh(command) {
    const verts = new Float32Array(command.vertices.length * 9);
    for (let i = 0; i < command.vertices.length; i++) {
      const v = command.vertices[i];
      const [px, py] = xy(v.x, v.y);
      const c = (v.rgba ?? 0xffffffff) >>> 0;
      const o = i * 9;
      verts[o] = px; verts[o + 1] = py;
      verts[o + 2] = v.u ?? 0; verts[o + 3] = v.v ?? 0;
      verts[o + 4] = ((c >>> 24) & 255) / 255;
      verts[o + 5] = ((c >>> 16) & 255) / 255;
      verts[o + 6] = ((c >>> 8) & 255) / 255;
      verts[o + 7] = (c & 255) / 255;
      verts[o + 8] = v.w ?? 1;
    }
    const id = command.handle ? this.gpuTextures.get(command.handle) : null;
    if (id) {
      gl.glActiveTexture(C.TEXTURE0);
      gl.glBindTexture(C.TEXTURE_2D, id);
      this._geometryVC(this.meshTextureProgram, verts);
      gl.glUniform1i(gl.glGetUniformLocation(this.meshTextureProgram, 'u_texture'), 0);
    } else {
      this._geometryVC(this.meshProgram, verts);
    }
    gl.glDrawArrays(C.TRIANGLES, 0, command.vertices.length);
  }

  _drawColor(vertices, color) {
    this._geometry(this.colorProgram, vertices);
    const location = gl.glGetUniformLocation(this.colorProgram, 'u_color');
    gl.glUniform4fv(location, new Float32Array(rgba(color)));
    gl.glDrawArrays(C.TRIANGLES, 0, vertices.length / 4);
  }

  _drawTexture(id, pixels, sourceWidth, sourceHeight, command) {
    gl.glActiveTexture(C.TEXTURE0);
    gl.glBindTexture(C.TEXTURE_2D, id);
    if (pixels) {
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MIN_FILTER, command.sampling ? C.LINEAR : C.NEAREST);
      gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_MAG_FILTER, command.sampling ? C.LINEAR : C.NEAREST);
      gl.glTexImage2D(C.TEXTURE_2D, 0, C.RGBA, sourceWidth, sourceHeight,
        0, C.RGBA, C.UNSIGNED_BYTE, pixels);
    }
    /* NEAREST parity with the CPU blitter.
     *
     * blitNearest picks texel floor((x - x0) * srcW / rw): it samples at the
     * output pixel's LEFT EDGE. GL samples at the fragment CENTER (x + 0.5),
     * one texel later wherever a texel boundary falls inside a pixel -- on
     * the 256x224 -> 1411x1080 game rect that is ~276k differing pixels on a
     * checkerboard. Shifting the UV mapping back half an OUTPUT pixel makes
     * the fragment-center sample land exactly on the CPU's sample points.
     *
     * The +1.5e-4 texel bias handles sample points that land EXACTLY on a
     * texel boundary (possible when srcW/rw is rational with a small
     * denominator, e.g. tests at 160x90 where 16/120 = 2/15): float32
     * interpolation wobbles either side of the exact integer and floor()
     * would pick a texel at random. Sizing matters: sample points can sit
     * 1/rw texel from a boundary (256/1411 puts one 7e-4 away), so the bias
     * must stay under half that gap -- 1e-3 flipped a real column at
     * x=237 -- while staying above interpolation noise (~3e-5 texel).
     * Linear sampling is already center-based on the CPU, so no shift. */
    let uv = sourceUv(command, sourceWidth, sourceHeight)
      ?? { u0: 0, v0: 0, u1: 1, v1: 1 };
    if (!command.sampling) {
      const sx = this.outputWidth / LOGICAL_WIDTH;
      const sy = this.outputHeight / LOGICAL_HEIGHT;
      const rw = Math.ceil((command.x + command.w) * sx) - Math.floor(command.x * sx);
      const rh = Math.ceil((command.y + command.h) * sy) - Math.floor(command.y * sy);
      if (rw > 0 && rh > 0) {
        const spanU = uv.u1 - uv.u0, spanV = uv.v1 - uv.v0;
        const srcW = command.sw > 0 ? command.sw : sourceWidth;
        const srcH = command.sh > 0 ? command.sh : sourceHeight;
        const du = spanU * 0.5 / rw - (spanU / srcW) * 1.5e-4;
        const dv = spanV * 0.5 / rh - (spanV / srcH) * 1.5e-4;
        uv = { u0: uv.u0 - du, v0: uv.v0 - dv, u1: uv.u1 - du, v1: uv.v1 - dv };
      }
    }
    this._geometry(this.textureProgram,
      quad(command.x, command.y, command.w, command.h, uv));
    gl.glUniform1i(gl.glGetUniformLocation(this.textureProgram, 'u_texture'), 0);
    gl.glDrawArrays(C.TRIANGLES, 0, 6);
  }

  compose(gamePixels, gameWidth, gameHeight) {
    /* Same transferred-frame contract as the CPU compositor: reallocate if
     * the host detached last frame's buffer. */
    if (this.output.buffer.byteLength === 0) {
      this.output = new Uint8ClampedArray(this.outputWidth * this.outputHeight * 4);
    }
    if (!this.commands.length) {
      const aspect = gameWidth / gameHeight;
      const w = LOGICAL_HEIGHT * aspect;
      this.drawGame((LOGICAL_WIDTH - w) / 2, 0, w, LOGICAL_HEIGHT);
    }
    // Resolve fit/integer-placement commands once through the shared CPU
    // geometry implementation, without rasterizing it.
    this.commands = this.commands.flatMap((command) => {
      if (command.kind !== 'game-fit') return [command];
      const mode = command.mode;
      let w; let h;
      if (mode === 2) { w = LOGICAL_WIDTH; h = LOGICAL_HEIGHT; }
      else if (mode === 3) {
        const scale = Math.max(1, Math.floor(Math.min(this.outputWidth / gameWidth, this.outputHeight / gameHeight)));
        w = gameWidth * scale * LOGICAL_WIDTH / this.outputWidth;
        h = gameHeight * scale * LOGICAL_HEIGHT / this.outputHeight;
      } else {
        const sourceAspect = gameWidth / gameHeight;
        const targetAspect = LOGICAL_WIDTH / LOGICAL_HEIGHT;
        const byWidth = mode === 1 ? sourceAspect < targetAspect : sourceAspect > targetAspect;
        w = byWidth ? LOGICAL_WIDTH : LOGICAL_HEIGHT * sourceAspect;
        h = byWidth ? LOGICAL_WIDTH / sourceAspect : LOGICAL_HEIGHT;
      }
      return [{
        ...command, kind: 'game',
        x: (LOGICAL_WIDTH - w) * Math.max(0, Math.min(1, command.alignX)),
        y: (LOGICAL_HEIGHT - h) * Math.max(0, Math.min(1, command.alignY)),
        w, h,
      }];
    });
    gl.makeCurrent?.();
    /* With an effect loaded, draw the scene into the OFFSCREEN target so the
     * shader has something to sample; otherwise draw straight to the default
     * framebuffer as before. Binding this per frame (rather than once at init)
     * keeps effect-on and effect-off frames correct when a guest toggles one. */
    gl.glBindFramebuffer(C.FRAMEBUFFER, this.effect ? this.sceneFbo : 0);
    gl.glViewport(0, 0, this.outputWidth, this.outputHeight);
    const [r, g, b, a] = rgba(this.clearColor);
    gl.glClearColor(r, g, b, a);
    gl.glClear(C.COLOR_BUFFER_BIT);
    gl.glEnable(C.BLEND);
    gl.glBlendFunc(C.SRC_ALPHA, C.ONE_MINUS_SRC_ALPHA);
    let clip = null;
    for (const command of this.commands) {
      if (command.kind === 'scissor') {
        clip = command;
        gl.glEnable(C.SCISSOR_TEST);
        const x = Math.floor(command.x * this.outputWidth / LOGICAL_WIDTH);
        const y = Math.floor((LOGICAL_HEIGHT - command.y - command.h) * this.outputHeight / LOGICAL_HEIGHT);
        const w = Math.ceil(command.w * this.outputWidth / LOGICAL_WIDTH);
        const h = Math.ceil(command.h * this.outputHeight / LOGICAL_HEIGHT);
        gl.glScissor(x, y, w, h);
      } else if (command.kind === 'scissor-reset') {
        clip = null;
        gl.glDisable(C.SCISSOR_TEST);
      } else if (command.kind === 'game') {
        this._drawTexture(this.gameTexture, gamePixels, gameWidth, gameHeight, command);
      } else if (command.kind === 'surface') {
        this._drawTexture(this.gameTexture, command.pixels, command.width, command.height, command);
      } else if (command.kind === 'rect') {
        this._drawColor(quad(...snapRect(command.x, command.y, command.w, command.h,
          this.outputWidth, this.outputHeight)), command.rgba);
      } else if (command.kind === 'triangle') {
        const [x1, y1] = xy(command.x1, command.y1);
        const [x2, y2] = xy(command.x2, command.y2);
        const [x3, y3] = xy(command.x3, command.y3);
        this._drawColor(new Float32Array([
          x1, y1, 0, 0, x2, y2, 0, 0, x3, y3, 0, 0,
        ]), command.rgba);
      } else if (command.kind === 'mesh') {
        this._drawMesh(command);
      } else if (command.kind === 'text') {
        /* Text as GEOMETRY, batched into one draw call.
         *
         * This used to be handled after the GL pass by re-composing the ENTIRE
         * 1920x1080 scene on a second CPU compositor and alpha-blending the
         * result over the readback -- +4.4 ms/frame measured, a 2.2x penalty on
         * any frame containing a single string. The glyphs are 3x5 bitmap cells,
         * i.e. plain rectangles, so the GPU can draw exactly the same shapes.
         * forEachGlyphRect is shared with the CPU backend so the two cannot
         * drift apart. */
        /* Snap each glyph cell to the SAME device-pixel grid the CPU
         * rasteriser uses (floor of the left/top edge, ceil of the right/
         * bottom), then convert back to logical units. GL fills only pixels
         * whose CENTRE is covered, so without this a one-unit-wide cell that
         * straddles a pixel boundary is dropped on GPU and kept on CPU --
         * measured as 212 differing pixels along glyph edges. */
        const sx = this.outputWidth / LOGICAL_WIDTH;
        const sy = this.outputHeight / LOGICAL_HEIGHT;
        const verts = [];
        forEachGlyphRect(command, (gx, gy, gw, gh) => {
          const px0 = Math.floor(gx * sx), py0 = Math.floor(gy * sy);
          const px1 = Math.ceil((gx + gw) * sx), py1 = Math.ceil((gy + gh) * sy);
          const [ax, ay] = xy(px0 / sx, py0 / sy);
          const [bx, by] = xy(px1 / sx, py1 / sy);
          verts.push(ax, ay, 0, 0, bx, ay, 0, 0, ax, by, 0, 0,
                     ax, by, 0, 0, bx, ay, 0, 0, bx, by, 0, 0);
        });
        if (verts.length) this._drawColor(new Float32Array(verts), command.rgba);
      } else if (command.kind === 'texture') {
        const texture = this.textures.get(command.handle);
        const id = this.gpuTextures.get(command.handle);
        if (texture && id) this._drawTexture(id, null, texture.width, texture.height, command);
      }
    }
    if (clip) gl.glDisable(C.SCISSOR_TEST);
    /* If an effect is loaded the scene above went into the offscreen target;
     * run the filter over it into the default framebuffer, then read THAT. */
    if (this.effect) {
      gl.glBindFramebuffer(C.FRAMEBUFFER, 0);
      gl.glViewport(0, 0, this.outputWidth, this.outputHeight);
      gl.glDisable(C.BLEND);
      gl.glUseProgram(this.effect.program);
      gl.glActiveTexture(C.TEXTURE0);
      gl.glBindTexture(C.TEXTURE_2D, this.sceneTexture);
      /* The scene was RENDERED into this texture, so its rows run bottom-up
       * (GL framebuffer origin); sampling it with the same top-down UV
       * convention as an uploaded texture shows the whole frame upside
       * down -- the Lua starter's first shader run flipped everything,
       * vignette included. Invert V for the effect quad only. */
      this._geometry(this.effect.program,
        quad(0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT, { u0: 0, v0: 1, u1: 1, v1: 0 }));
      const u = (name) => gl.glGetUniformLocation(this.effect.program, name);
      gl.glUniform1i(u('u_texture'), 0);
      gl.glUniform2f(u('u_resolution'), this.outputWidth, this.outputHeight);
      gl.glUniform1f(u('u_time'), (this.effectTimeMs ?? 0) / 1000);
      gl.glDrawArrays(C.TRIANGLES, 0, 6);
      gl.glEnable(C.BLEND);
    }
    gl.glFinish();
    gl.glReadPixels(0, 0, this.outputWidth, this.outputHeight, C.RGBA, C.UNSIGNED_BYTE, this.readback);
    const row = this.outputWidth * 4;
    for (let y = 0; y < this.outputHeight; y++) {
      this.output.set(this.readback.subarray((this.outputHeight - y - 1) * row, (this.outputHeight - y) * row), y * row);
    }
    for (let i = 3; i < this.output.length; i += 4) this.output[i] = 255;
    return { rgba: this.output, width: this.outputWidth, height: this.outputHeight };
  }

  /* No drawTexture override: the base class already emits kind:'texture' with
   * the source rect. The override that used to live here took only 5 args and
   * silently dropped sx/sy/sw/sh, so every atlas blit on the GPU path drew the
   * WHOLE sheet -- noise where tiles belonged, while the CPU path was correct.
   * A subclass narrowing its parent's signature is a quiet way to lose data. */

  destroy() {
    if (this.gpuReady) {
      for (const id of this.gpuTextures.values()) gl.glDeleteTextures(1, new Uint32Array([id]));
      if (this.gameTexture) gl.glDeleteTextures(1, new Uint32Array([this.gameTexture]));
      if (this.vbo) gl.glDeleteBuffers(1, new Uint32Array([this.vbo]));
      if (this.vao) gl.glDeleteVertexArrays(1, new Uint32Array([this.vao]));
      if (this.colorProgram) gl.glDeleteProgram(this.colorProgram);
      if (this.textureProgram) gl.glDeleteProgram(this.textureProgram);
    }
    this.gpuTextures.clear();
    super.destroy();
    if (this.contextOwned) {
      try { gl.destroyContext(); } catch {}
      this.contextOwned = false;
    }
    this.gpuReady = false;
  }
}
