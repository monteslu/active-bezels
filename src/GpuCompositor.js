import { createRequire } from 'node:module';
import {
  ActiveBezelCompositor, LOGICAL_WIDTH, LOGICAL_HEIGHT,
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

function quad(x, y, w, h) {
  const [x0, y0] = xy(x, y);
  const [x1, y1] = xy(x + w, y + h);
  return new Float32Array([
    x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1,
    x0, y1, 0, 1, x1, y0, 1, 0, x1, y1, 1, 1,
  ]);
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
    this.overlay = new ActiveBezelCompositor(options);
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
    this.gpuReady = true;
  }

  _newTexture() {
    const ids = new Uint32Array(1);
    gl.glGenTextures(1, ids);
    gl.glBindTexture(C.TEXTURE_2D, ids[0]);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_S, C.CLAMP_TO_EDGE);
    gl.glTexParameteri(C.TEXTURE_2D, C.TEXTURE_WRAP_T, C.CLAMP_TO_EDGE);
    return ids[0];
  }

  reset() {
    super.reset();
    this.overlay?.reset();
  }

  text(value, x, y, size, color) {
    super.text(value, x, y, size, color);
    this.overlay.text(value, x, y, size, color);
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
    this._geometry(this.textureProgram, quad(command.x, command.y, command.w, command.h));
    gl.glUniform1i(gl.glGetUniformLocation(this.textureProgram, 'u_texture'), 0);
    gl.glDrawArrays(C.TRIANGLES, 0, 6);
  }

  compose(gamePixels, gameWidth, gameHeight) {
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
        this._drawColor(quad(command.x, command.y, command.w, command.h), command.rgba);
      } else if (command.kind === 'triangle') {
        const [x1, y1] = xy(command.x1, command.y1);
        const [x2, y2] = xy(command.x2, command.y2);
        const [x3, y3] = xy(command.x3, command.y3);
        this._drawColor(new Float32Array([
          x1, y1, 0, 0, x2, y2, 0, 0, x3, y3, 0, 0,
        ]), command.rgba);
      } else if (command.kind === 'texture') {
        const texture = this.textures.get(command.handle);
        const id = this.gpuTextures.get(command.handle);
        if (texture && id) this._drawTexture(id, null, texture.width, texture.height, command);
      }
    }
    if (clip) gl.glDisable(C.SCISSOR_TEST);
    gl.glFinish();
    gl.glReadPixels(0, 0, this.outputWidth, this.outputHeight, C.RGBA, C.UNSIGNED_BYTE, this.readback);
    const row = this.outputWidth * 4;
    for (let y = 0; y < this.outputHeight; y++) {
      this.output.set(this.readback.subarray((this.outputHeight - y - 1) * row, (this.outputHeight - y) * row), y * row);
    }
    for (let i = 3; i < this.output.length; i += 4) this.output[i] = 255;
    if (this.commands.some((command) => command.kind === 'text')) {
      this.overlay.clear(0x00000000);
      const text = this.overlay.compose(new Uint8Array(4), 1, 1).rgba;
      for (let i = 0; i < text.length; i += 4) {
        const alpha = text[i + 3] / 255;
        if (!alpha) continue;
        const inv = 1 - alpha;
        this.output[i] = text[i] * alpha + this.output[i] * inv;
        this.output[i + 1] = text[i + 1] * alpha + this.output[i + 1] * inv;
        this.output[i + 2] = text[i + 2] * alpha + this.output[i + 2] * inv;
        this.output[i + 3] = 255;
      }
    }
    return { rgba: this.output, width: this.outputWidth, height: this.outputHeight };
  }

  drawTexture(handle, x, y, w, h) {
    if (!this.textures.has(handle)) return 0;
    this._push({ kind: 'texture', handle, x, y, w, h, sampling: 0 });
    return 1;
  }

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
