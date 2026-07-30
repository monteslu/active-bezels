const LOGICAL_WIDTH = 1920;
const LOGICAL_HEIGHT = 1080;

function parseColor(value) {
  if (typeof value === 'number') return value >>> 0;
  const hex = String(value ?? '#000000').replace('#', '');
  const n = Number.parseInt(hex.padEnd(8, 'f'), 16);
  return Number.isFinite(n) ? n >>> 0 : 0x000000ff;
}

function fill(pixels, rgba) {
  const r = (rgba >>> 24) & 0xff;
  const g = (rgba >>> 16) & 0xff;
  const b = (rgba >>> 8) & 0xff;
  const a = rgba & 0xff;
  for (let i = 0; i < pixels.length; i += 4) {
    pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = a;
  }
}

function rgbaParts(rgba) {
  return [(rgba >>> 24) & 0xff, (rgba >>> 16) & 0xff, (rgba >>> 8) & 0xff, rgba & 0xff];
}

function blendPixel(dst, offset, rgba) {
  const [r, g, b, alpha] = rgbaParts(rgba);
  if (alpha === 255) {
    dst[offset] = r; dst[offset + 1] = g; dst[offset + 2] = b; dst[offset + 3] = 255;
    return;
  }
  if (alpha === 0) return;
  const a = alpha / 255;
  const inv = 1 - a;
  dst[offset] = Math.round(r * a + dst[offset] * inv);
  dst[offset + 1] = Math.round(g * a + dst[offset + 1] * inv);
  dst[offset + 2] = Math.round(b * a + dst[offset + 2] * inv);
  dst[offset + 3] = 255;
}

function logicalRect(rect, dw, dh) {
  return {
    x0: Math.max(0, Math.floor(rect.x * dw / LOGICAL_WIDTH)),
    y0: Math.max(0, Math.floor(rect.y * dh / LOGICAL_HEIGHT)),
    x1: Math.min(dw, Math.ceil((rect.x + rect.w) * dw / LOGICAL_WIDTH)),
    y1: Math.min(dh, Math.ceil((rect.y + rect.h) * dh / LOGICAL_HEIGHT)),
  };
}

function blitNearest(src, sw, sh, dst, dw, dh, rect) {
  const { x0, y0, x1, y1 } = logicalRect(rect, dw, dh);
  const rw = Math.max(1, x1 - x0);
  const rh = Math.max(1, y1 - y0);
  for (let y = y0; y < y1; y++) {
    const sy = Math.min(sh - 1, Math.floor((y - y0) * sh / rh));
    for (let x = x0; x < x1; x++) {
      const sx = Math.min(sw - 1, Math.floor((x - x0) * sw / rw));
      const si = (sy * sw + sx) * 4;
      const di = (y * dw + x) * 4;
      const a = src[si + 3] / 255;
      if (a >= 0.999) {
        dst[di] = src[si]; dst[di + 1] = src[si + 1]; dst[di + 2] = src[si + 2]; dst[di + 3] = 255;
      } else if (a > 0) {
        const inv = 1 - a;
        dst[di] = src[si] * a + dst[di] * inv;
        dst[di + 1] = src[si + 1] * a + dst[di + 1] * inv;
        dst[di + 2] = src[si + 2] * a + dst[di + 2] * inv;
        dst[di + 3] = 255;
      }
    }
  }
}

function blitLinear(src, sw, sh, dst, dw, dh, rect) {
  const { x0, y0, x1, y1 } = logicalRect(rect, dw, dh);
  const rw = Math.max(1, x1 - x0);
  const rh = Math.max(1, y1 - y0);
  for (let y = y0; y < y1; y++) {
    const fy = Math.max(0, Math.min(sh - 1, ((y - y0 + 0.5) * sh / rh) - 0.5));
    const ya = Math.floor(fy);
    const yb = Math.min(sh - 1, ya + 1);
    const ty = fy - ya;
    for (let x = x0; x < x1; x++) {
      const fx = Math.max(0, Math.min(sw - 1, ((x - x0 + 0.5) * sw / rw) - 0.5));
      const xa = Math.floor(fx);
      const xb = Math.min(sw - 1, xa + 1);
      const tx = fx - xa;
      const aa = (ya * sw + xa) * 4;
      const ab = (ya * sw + xb) * 4;
      const ba = (yb * sw + xa) * 4;
      const bb = (yb * sw + xb) * 4;
      const di = (y * dw + x) * 4;
      for (let channel = 0; channel < 4; channel++) {
        const top = src[aa + channel] * (1 - tx) + src[ab + channel] * tx;
        const bottom = src[ba + channel] * (1 - tx) + src[bb + channel] * tx;
        dst[di + channel] = Math.round(top * (1 - ty) + bottom * ty);
      }
    }
  }
}

function drawRect(dst, dw, dh, rect, rgba, clip) {
  const mapped = logicalRect(rect, dw, dh);
  const clipping = clip ? logicalRect(clip, dw, dh) : { x0: 0, y0: 0, x1: dw, y1: dh };
  const x0 = Math.max(mapped.x0, clipping.x0);
  const y0 = Math.max(mapped.y0, clipping.y0);
  const x1 = Math.min(mapped.x1, clipping.x1);
  const y1 = Math.min(mapped.y1, clipping.y1);
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) blendPixel(dst, (y * dw + x) * 4, rgba);
  }
}

function edge(ax, ay, bx, by, px, py) {
  return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

function drawTriangle(dst, dw, dh, command, clip) {
  const sx = dw / LOGICAL_WIDTH;
  const sy = dh / LOGICAL_HEIGHT;
  const ax = command.x1 * sx; const ay = command.y1 * sy;
  const bx = command.x2 * sx; const by = command.y2 * sy;
  const cx = command.x3 * sx; const cy = command.y3 * sy;
  const clipping = clip ? logicalRect(clip, dw, dh) : { x0: 0, y0: 0, x1: dw, y1: dh };
  const minX = Math.max(clipping.x0, Math.floor(Math.min(ax, bx, cx)));
  const maxX = Math.min(clipping.x1 - 1, Math.ceil(Math.max(ax, bx, cx)));
  const minY = Math.max(clipping.y0, Math.floor(Math.min(ay, by, cy)));
  const maxY = Math.min(clipping.y1 - 1, Math.ceil(Math.max(ay, by, cy)));
  const area = edge(ax, ay, bx, by, cx, cy);
  if (area === 0) return;
  for (let y = minY; y <= maxY; y++) {
    for (let x = minX; x <= maxX; x++) {
      const w0 = edge(bx, by, cx, cy, x + 0.5, y + 0.5);
      const w1 = edge(cx, cy, ax, ay, x + 0.5, y + 0.5);
      const w2 = edge(ax, ay, bx, by, x + 0.5, y + 0.5);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
        blendPixel(dst, (y * dw + x) * 4, command.rgba);
      }
    }
  }
}

const FONT = Object.freeze({
  ' ': ['000','000','000','000','000'],
  '?': ['110','001','010','000','010'],
  '.': ['000','000','000','000','010'],
  ':': ['000','010','000','010','000'],
  '-': ['000','000','111','000','000'],
  '/': ['001','001','010','100','100'],
  '0': ['111','101','101','101','111'], '1': ['010','110','010','010','111'],
  '2': ['110','001','111','100','111'], '3': ['110','001','111','001','110'],
  '4': ['101','101','111','001','001'], '5': ['111','100','110','001','110'],
  '6': ['011','100','111','101','111'], '7': ['111','001','010','010','010'],
  '8': ['111','101','111','101','111'], '9': ['111','101','111','001','110'],
  A: ['010','101','111','101','101'], B: ['110','101','110','101','110'],
  C: ['011','100','100','100','011'], D: ['110','101','101','101','110'],
  E: ['111','100','110','100','111'], F: ['111','100','110','100','100'],
  G: ['011','100','101','101','011'], H: ['101','101','111','101','101'],
  I: ['111','010','010','010','111'], J: ['001','001','001','101','010'],
  K: ['101','101','110','101','101'], L: ['100','100','100','100','111'],
  M: ['101','111','111','101','101'], N: ['101','111','111','111','101'],
  O: ['010','101','101','101','010'], P: ['110','101','110','100','100'],
  Q: ['010','101','101','111','011'], R: ['110','101','110','101','101'],
  S: ['011','100','010','001','110'], T: ['111','010','010','010','010'],
  U: ['101','101','101','101','111'], V: ['101','101','101','101','010'],
  W: ['101','101','111','111','101'], X: ['101','101','010','101','101'],
  Y: ['101','101','010','010','010'], Z: ['111','001','010','100','111'],
});

function drawText(dst, dw, dh, command, clip) {
  const scale = Math.max(1, command.size / 5);
  let cursor = command.x;
  for (const raw of command.text) {
    const glyph = FONT[raw.toUpperCase()] ?? FONT['?'];
    for (let row = 0; row < 5; row++) {
      for (let col = 0; col < 3; col++) {
        if (glyph[row][col] === '1') {
          drawRect(dst, dw, dh, {
            x: cursor + col * scale, y: command.y + row * scale, w: scale, h: scale,
          }, command.rgba, clip);
        }
      }
    }
    cursor += 4 * scale;
  }
}

export class ActiveBezelCompositor {
  constructor({ outputWidth = 1920, outputHeight = 1080, maxCommands = 16_384 } = {}) {
    this.outputWidth = outputWidth;
    this.outputHeight = outputHeight;
    this.maxCommands = maxCommands;
    this.output = new Uint8ClampedArray(outputWidth * outputHeight * 4);
    this.textures = new Map();
    this.nextTexture = 1;
    this.reset();
  }

  reset() {
    this.clearColor = 0x000000ff;
    this.commands = [];
  }

  clear(color) {
    this.clearColor = parseColor(color);
  }

  _push(command) {
    if (this.commands.length >= this.maxCommands) {
      throw new Error(`Active Bezel command limit exceeded (${this.maxCommands})`);
    }
    this.commands.push(command);
  }

  drawGame(x, y, w, h, sampling = 0) {
    this._push({ kind: 'game', x, y, w, h, sampling });
  }

  drawGameFit(mode = 0, alignX = 0.5, alignY = 0.5, sampling = 0) {
    this._push({ kind: 'game-fit', mode, alignX, alignY, sampling });
  }

  drawSurface(pixels, width, height, x = 0, y = 0, w = LOGICAL_WIDTH, h = LOGICAL_HEIGHT) {
    this._push({ kind: 'surface', pixels, width, height, x, y, w, h });
  }

  createTexture(pixels, width, height) {
    if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height)
      || width <= 0 || height <= 0 || width * height > 16_777_216) return 0;
    const handle = this.nextTexture++;
    this.textures.set(handle, {
      pixels: Uint8Array.from(pixels.subarray(0, width * height * 4)),
      width,
      height,
    });
    return handle;
  }

  destroyTexture(handle) {
    return this.textures.delete(handle) ? 1 : 0;
  }

  drawTexture(handle, x, y, w, h) {
    const texture = this.textures.get(handle);
    if (!texture) return 0;
    this.drawSurface(texture.pixels, texture.width, texture.height, x, y, w, h);
    return 1;
  }

  destroy() {
    this.textures.clear();
  }

  fillRect(x, y, w, h, rgba) {
    this._push({ kind: 'rect', x, y, w, h, rgba: parseColor(rgba) });
  }

  triangle(x1, y1, x2, y2, x3, y3, rgba) {
    this._push({ kind: 'triangle', x1, y1, x2, y2, x3, y3, rgba: parseColor(rgba) });
  }

  text(value, x, y, size, rgba) {
    this._push({ kind: 'text', text: String(value), x, y, size, rgba: parseColor(rgba) });
  }

  scissor(x, y, w, h) {
    this._push({ kind: 'scissor', x, y, w, h });
  }

  resetScissor() {
    this._push({ kind: 'scissor-reset' });
  }

  compose(gamePixels, gameWidth, gameHeight) {
    fill(this.output, this.clearColor);
    if (this.commands.length === 0) {
      const gameAspect = gameWidth / gameHeight;
      const logicalAspect = LOGICAL_WIDTH / LOGICAL_HEIGHT;
      const w = gameAspect > logicalAspect ? LOGICAL_WIDTH : LOGICAL_HEIGHT * gameAspect;
      const h = gameAspect > logicalAspect ? LOGICAL_WIDTH / gameAspect : LOGICAL_HEIGHT;
      this.drawGame((LOGICAL_WIDTH - w) / 2, (LOGICAL_HEIGHT - h) / 2, w, h);
    }
    let clip = null;
    const commands = this.commands.map((command) => {
      if (command.kind !== 'game-fit') return command;
      if (command.mode === 2) {
        return { ...command, kind: 'game', x: 0, y: 0, w: LOGICAL_WIDTH, h: LOGICAL_HEIGHT };
      }
      let w;
      let h;
      if (command.mode === 3) {
        const scale = Math.max(1, Math.floor(Math.min(this.outputWidth / gameWidth, this.outputHeight / gameHeight)));
        w = gameWidth * scale * LOGICAL_WIDTH / this.outputWidth;
        h = gameHeight * scale * LOGICAL_HEIGHT / this.outputHeight;
      } else {
        const sourceAspect = gameWidth / gameHeight;
        const targetAspect = LOGICAL_WIDTH / LOGICAL_HEIGHT;
        const cover = command.mode === 1;
        const byWidth = cover ? sourceAspect < targetAspect : sourceAspect > targetAspect;
        w = byWidth ? LOGICAL_WIDTH : LOGICAL_HEIGHT * sourceAspect;
        h = byWidth ? LOGICAL_WIDTH / sourceAspect : LOGICAL_HEIGHT;
      }
      return {
        ...command,
        kind: 'game',
        x: (LOGICAL_WIDTH - w) * Math.max(0, Math.min(1, command.alignX)),
        y: (LOGICAL_HEIGHT - h) * Math.max(0, Math.min(1, command.alignY)),
        w,
        h,
      };
    });
    for (const command of commands) {
      if (command.kind === 'game') {
        (command.sampling ? blitLinear : blitNearest)(
          gamePixels, gameWidth, gameHeight, this.output, this.outputWidth, this.outputHeight, command,
        );
      } else if (command.kind === 'surface') {
        (command.sampling ? blitLinear : blitNearest)(
          command.pixels, command.width, command.height, this.output, this.outputWidth, this.outputHeight, command,
        );
      } else if (command.kind === 'rect') {
        drawRect(this.output, this.outputWidth, this.outputHeight, command, command.rgba, clip);
      } else if (command.kind === 'triangle') {
        drawTriangle(this.output, this.outputWidth, this.outputHeight, command, clip);
      } else if (command.kind === 'text') {
        drawText(this.output, this.outputWidth, this.outputHeight, command, clip);
      } else if (command.kind === 'scissor') {
        clip = command;
      } else if (command.kind === 'scissor-reset') {
        clip = null;
      }
    }
    return { rgba: this.output, width: this.outputWidth, height: this.outputHeight };
  }
}

export { LOGICAL_WIDTH, LOGICAL_HEIGHT };
