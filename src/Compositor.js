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

function blitNearest(src, sw, sh, dst, dw, dh, rect, clip) {
  /* The scissor applies to TEXTURES too. It never did: the blitters took no
   * clip argument, so a package that scissored a region and then drew a
   * texture straddling its edge painted straight over the boundary. That is
   * how a tile that should have been clipped at the strip edge ended up on top
   * of the live game frame. */
  let { x0, y0, x1, y1 } = logicalRect(rect, dw, dh);
  if (clip) {
    const c = logicalRect(clip, dw, dh);
    x0 = Math.max(x0, c.x0); y0 = Math.max(y0, c.y0);
    x1 = Math.min(x1, c.x1); y1 = Math.min(y1, c.y1);
    if (x1 <= x0 || y1 <= y0) return;
  }
  /* Sampling is parameterised by the FULL destination rect, not the clipped
   * one -- otherwise clipping an image would stretch what remains instead of
   * cropping it. That includes the SCREEN EDGE: logicalRect clamps to the
   * output, so a rect hanging off the right edge came back narrower and the
   * remaining pixels sampled a compressed texel grid -- the GPU backend
   * crops there, and the last strip tile diverged by whole texels. The
   * sampling basis must come from the unclamped extent. */
  const rw = Math.max(1, Math.ceil((rect.x + rect.w) * dw / LOGICAL_WIDTH) - Math.floor(rect.x * dw / LOGICAL_WIDTH));
  const rh = Math.max(1, Math.ceil((rect.y + rect.h) * dh / LOGICAL_HEIGHT) - Math.floor(rect.y * dh / LOGICAL_HEIGHT));
  const ox = Math.floor(rect.x * dw / LOGICAL_WIDTH), oy = Math.floor(rect.y * dh / LOGICAL_HEIGHT);
  // Optional SOURCE sub-rectangle. Without it a texture can only ever be drawn
  // whole, which forces atlas users into one texture per sprite -- and for a
  // tile renderer that means one command per PIXEL instead of per tile.
  const srcX = rect.sx ?? 0, srcY = rect.sy ?? 0;
  const srcW = rect.sw ?? sw, srcH = rect.sh ?? sh;
  for (let y = y0; y < y1; y++) {
    const sy = Math.min(sh - 1, srcY + Math.floor((y - oy) * srcH / rh));
    for (let x = x0; x < x1; x++) {
      const sx = Math.min(sw - 1, srcX + Math.floor((x - ox) * srcW / rw));
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

function blitLinear(src, sw, sh, dst, dw, dh, rect, clip) {
  /* The scissor applies to TEXTURES too. It never did: the blitters took no
   * clip argument, so a package that scissored a region and then drew a
   * texture straddling its edge painted straight over the boundary. That is
   * how a tile that should have been clipped at the strip edge ended up on top
   * of the live game frame. */
  let { x0, y0, x1, y1 } = logicalRect(rect, dw, dh);
  if (clip) {
    const c = logicalRect(clip, dw, dh);
    x0 = Math.max(x0, c.x0); y0 = Math.max(y0, c.y0);
    x1 = Math.min(x1, c.x1); y1 = Math.min(y1, c.y1);
    if (x1 <= x0 || y1 <= y0) return;
  }
  /* Unclamped sampling basis -- same screen-edge reasoning as blitNearest. */
  const rw = Math.max(1, Math.ceil((rect.x + rect.w) * dw / LOGICAL_WIDTH) - Math.floor(rect.x * dw / LOGICAL_WIDTH));
  const rh = Math.max(1, Math.ceil((rect.y + rect.h) * dh / LOGICAL_HEIGHT) - Math.floor(rect.y * dh / LOGICAL_HEIGHT));
  const oxl = Math.floor(rect.x * dw / LOGICAL_WIDTH), oyl = Math.floor(rect.y * dh / LOGICAL_HEIGHT);
  // Source sub-rectangle, as in blitNearest. Sampling is clamped INSIDE the
  // sub-rect so a filtered atlas tile cannot bleed in its neighbours' pixels.
  const srcX = rect.sx ?? 0, srcY = rect.sy ?? 0;
  const srcW = rect.sw ?? sw, srcH = rect.sh ?? sh;
  const maxX = srcX + srcW - 1, maxY = srcY + srcH - 1;
  for (let y = y0; y < y1; y++) {
    const fy = Math.max(srcY, Math.min(maxY, srcY + ((y - oyl + 0.5) * srcH / rh) - 0.5));
    const ya = Math.floor(fy);
    const yb = Math.min(maxY, ya + 1);
    const ty = fy - ya;
    for (let x = x0; x < x1; x++) {
      const fx = Math.max(srcX, Math.min(maxX, srcX + ((x - oxl + 0.5) * srcW / rw) - 0.5));
      const xa = Math.floor(fx);
      const xb = Math.min(maxX, xa + 1);
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

/*
 * A triangle with per-vertex colour, and optionally per-vertex UVs into a
 * texture. Barycentric interpolation, same edge test as the flat rasteriser so
 * the two agree on coverage.
 */
function drawMeshTriangle(dst, dw, dh, v0, v1, v2, tex, clip) {
  const sx = dw / LOGICAL_WIDTH;
  const sy = dh / LOGICAL_HEIGHT;
  const ax = v0.x * sx, ay = v0.y * sy;
  const bx = v1.x * sx, by = v1.y * sy;
  const cx = v2.x * sx, cy = v2.y * sy;
  const clipping = clip ? logicalRect(clip, dw, dh) : { x0: 0, y0: 0, x1: dw, y1: dh };
  const minX = Math.max(clipping.x0, Math.floor(Math.min(ax, bx, cx)));
  const maxX = Math.min(clipping.x1 - 1, Math.ceil(Math.max(ax, bx, cx)));
  const minY = Math.max(clipping.y0, Math.floor(Math.min(ay, by, cy)));
  const maxY = Math.min(clipping.y1 - 1, Math.ceil(Math.max(ay, by, cy)));
  const area = edge(ax, ay, bx, by, cx, cy);
  if (area === 0) return;
  for (let y = minY; y <= maxY; y++) {
    for (let x = minX; x <= maxX; x++) {
      const px = x + 0.5, py = y + 0.5;
      const w0 = edge(bx, by, cx, cy, px, py);
      const w1 = edge(cx, cy, ax, ay, px, py);
      const w2 = edge(ax, ay, bx, by, px, py);
      const inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
      if (!inside) continue;
      const l0 = w0 / area, l1 = w1 / area, l2 = w2 / area;
      let r, g, b, a;
      if (tex) {
        /* Perspective-correct when the vertices carry a w divisor (quad()
         * sets it): interpolate u/w, v/w and 1/w linearly in screen space,
         * then divide. With w = 1 everywhere this reduces exactly to the
         * affine case, so a plain mesh() is unaffected. */
        const w0 = v0.w ?? 1, w1 = v1.w ?? 1, w2 = v2.w ?? 1;
        const iw = l0 / w0 + l1 / w1 + l2 / w2;
        const u = (l0 * (v0.u ?? 0) / w0 + l1 * (v1.u ?? 0) / w1
                 + l2 * (v2.u ?? 0) / w2) / iw;
        const vv = (l0 * (v0.v ?? 0) / w0 + l1 * (v1.v ?? 0) / w1
                  + l2 * (v2.v ?? 0) / w2) / iw;
        const tx = Math.min(tex.width - 1, Math.max(0, Math.floor(u * tex.width)));
        const ty = Math.min(tex.height - 1, Math.max(0, Math.floor(vv * tex.height)));
        const ti = (ty * tex.width + tx) * 4;
        /* Modulate by the interpolated vertex colour -- the standard sprite
         * tint. A white atlas (glyphs, icons) can then be drawn in any colour
         * without one texture per tint. Vertices default to 0xffffffff, so a
         * mesh that never sets rgba is untouched. The GPU shader multiplies
         * identically. */
        const c0 = (v0.rgba ?? 0xffffffff) >>> 0;
        const c1 = (v1.rgba ?? 0xffffffff) >>> 0;
        const c2 = (v2.rgba ?? 0xffffffff) >>> 0;
        const mr = (l0 * ((c0 >>> 24) & 255) + l1 * ((c1 >>> 24) & 255) + l2 * ((c2 >>> 24) & 255)) / 255;
        const mg = (l0 * ((c0 >>> 16) & 255) + l1 * ((c1 >>> 16) & 255) + l2 * ((c2 >>> 16) & 255)) / 255;
        const mb = (l0 * ((c0 >>> 8) & 255) + l1 * ((c1 >>> 8) & 255) + l2 * ((c2 >>> 8) & 255)) / 255;
        const ma = (l0 * (c0 & 255) + l1 * (c1 & 255) + l2 * (c2 & 255)) / 255;
        r = tex.pixels[ti] * mr; g = tex.pixels[ti + 1] * mg;
        b = tex.pixels[ti + 2] * mb; a = tex.pixels[ti + 3] * ma;
      } else {
        const c0 = v0.rgba >>> 0, c1 = v1.rgba >>> 0, c2 = v2.rgba >>> 0;
        r = l0 * ((c0 >>> 24) & 255) + l1 * ((c1 >>> 24) & 255) + l2 * ((c2 >>> 24) & 255);
        g = l0 * ((c0 >>> 16) & 255) + l1 * ((c1 >>> 16) & 255) + l2 * ((c2 >>> 16) & 255);
        b = l0 * ((c0 >>> 8) & 255) + l1 * ((c1 >>> 8) & 255) + l2 * ((c2 >>> 8) & 255);
        a = l0 * (c0 & 255) + l1 * (c1 & 255) + l2 * (c2 & 255);
      }
      if (a <= 0) continue;
      blendPixel(dst, (y * dw + x) * 4,
        (((Math.round(r) & 255) << 24) | ((Math.round(g) & 255) << 16)
         | ((Math.round(b) & 255) << 8) | (Math.round(a) & 255)) >>> 0);
    }
  }
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

const GLYPH_ROWS = 5;
const GLYPH_COLS = 3;
const GLYPH_ADVANCE = 4;   /* 3 columns + 1 of spacing */

function drawText(dst, dw, dh, command, clip) {
  forEachGlyphRect(command, (x, y, w, h) => {
    drawRect(dst, dw, dh, { x, y, w, h }, command.rgba, clip);
  });
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
    this.transform = [1, 0, 0, 1, 0, 0];
    this.transformStack = [];
    this.commands = [];
  }

  clear(color) {
    this.clearColor = parseColor(color);
  }

  /*
   * The transform stack.
   *
   * A 2x3 affine matrix [a, b, c, d, e, f] mapping
   *   x' = a*x + c*y + e
   *   y' = b*x + d*y + f
   *
   * Applied HERE, at the single push chokepoint, rather than in each backend:
   * every command kind and both backends get transforms for free, and the two
   * cannot disagree about what a rotation means. Commands are stored already
   * transformed, so a compositor never has to know a stack existed.
   */
  pushTransform() {
    this.transformStack.push([...this.transform]);
    return this.transformStack.length;
  }

  popTransform() {
    if (this.transformStack.length) this.transform = this.transformStack.pop();
    return this.transformStack.length;
  }

  resetTransform() {
    this.transform = [1, 0, 0, 1, 0, 0];
    this.transformStack.length = 0;
  }

  /* this.transform = this.transform * m  (m applied first, then the existing) */
  _concat(m) {
    const t = this.transform;
    this.transform = [
      t[0] * m[0] + t[2] * m[1],
      t[1] * m[0] + t[3] * m[1],
      t[0] * m[2] + t[2] * m[3],
      t[1] * m[2] + t[3] * m[3],
      t[0] * m[4] + t[2] * m[5] + t[4],
      t[1] * m[4] + t[3] * m[5] + t[5],
    ];
  }

  translate(x, y) { this._concat([1, 0, 0, 1, x, y]); }
  scale(x, y) { this._concat([x, 0, 0, y === undefined ? x : y, 0, 0]); }
  rotate(radians) {
    const c = Math.cos(radians); const sn = Math.sin(radians);
    this._concat([c, sn, -sn, c, 0, 0]);
  }

  /*
   * Shear. `x` slides horizontally in proportion to y, `y` vertically in
   * proportion to x -- both as tangents, so skew(Math.PI/6, 0) leans 30
   * degrees. The transform stack was always a full 2x3 affine and could
   * represent this; only the verb was missing.
   *
   * A sheared rect is not a rect, so it takes the same path a rotated one
   * does: real geometry, not a stretched box.
   */
  skew(x, y = 0) { this._concat([1, Math.tan(y), Math.tan(x), 1, 0, 0]); }

  /* The escape hatch: concatenate an arbitrary 2x3 affine [a, b, c, d, e, f]
   * mapping (x, y) -> (a*x + c*y + e, b*x + d*y + f). Everything above is a
   * named case of this. */
  transform2d(a, b, c, d, e, f) { this._concat([a, b, c, d, e, f]); }

  _identityTransform() {
    const t = this.transform;
    return t[0] === 1 && t[1] === 0 && t[2] === 0 && t[3] === 1 && t[4] === 0 && t[5] === 0;
  }

  _point(x, y) {
    const t = this.transform;
    return [t[0] * x + t[2] * y + t[4], t[1] * x + t[3] * y + t[5]];
  }

  /*
   * Apply the current transform to a command.
   *
   * An axis-aligned rect stays a rect under translate/scale, so those keep
   * their fast rect path. Anything with rotation or skew is converted to two
   * triangles, because a rotated "rect" is not a rect and drawing it as one
   * would silently ignore the rotation.
   */
  _transformCommand(command) {
    if (this._identityTransform()) return [command];
    const t = this.transform;
    /* Off-diagonal terms mean rotation OR shear; either way the axis-aligned
     * fast path would silently drop the transform, so both take the
     * emit-real-geometry route below. */
    const rotated = t[1] !== 0 || t[2] !== 0;
    const k = command.kind;

    if (k === 'triangle') {
      const [x1, y1] = this._point(command.x1, command.y1);
      const [x2, y2] = this._point(command.x2, command.y2);
      const [x3, y3] = this._point(command.x3, command.y3);
      return [{ ...command, x1, y1, x2, y2, x3, y3 }];
    }
    if (k === 'mesh') {
      const verts = command.vertices.map((v) => {
        const [x, y] = this._point(v.x, v.y);
        return { ...v, x, y };
      });
      return [{ ...command, vertices: verts }];
    }
    if (k === 'text' || k === 'scissor' || k === 'scissor-reset' || k === 'game-fit') {
      /* Text and clipping are axis-aligned by definition; translate/scale the
       * origin but do not attempt to rotate them. */
      if (command.x === undefined) return [command];
      const [x, y] = this._point(command.x, command.y);
      const sizeScale = Math.abs(t[0]) || 1;
      return [{ ...command, x, y,
        ...(command.w !== undefined ? { w: command.w * t[0], h: command.h * t[3] } : {}),
        ...(k === 'text' ? { size: command.size * sizeScale } : {}) }];
    }
    if (command.x === undefined || command.w === undefined) return [command];

    if (!rotated) {
      const [x, y] = this._point(command.x, command.y);
      const w = command.w * t[0];
      const h = command.h * t[3];
      return [{ ...command, x, y, w, h }];
    }

    /* Rotated: emit real geometry. A rotated "rect" is not a rect. */
    const [ax, ay] = this._point(command.x, command.y);
    const [bx, by] = this._point(command.x + command.w, command.y);
    const [cx, cy] = this._point(command.x + command.w, command.y + command.h);
    const [dx, dy] = this._point(command.x, command.y + command.h);
    if (k === 'rect') {
      return [
        { kind: 'triangle', x1: ax, y1: ay, x2: bx, y2: by, x3: cx, y3: cy, rgba: command.rgba },
        { kind: 'triangle', x1: ax, y1: ay, x2: cx, y2: cy, x3: dx, y3: dy, rgba: command.rgba },
      ];
    }
    if (k === 'texture') {
      /* Rotated textures become a textured MESH: both backends already
       * rasterize those with UV sampling. The old path attached a `quad`
       * field nobody consumed, so a rotated sprite silently rendered
       * axis-aligned at its UNtransformed position -- the Lua starter's
       * spinning badge landed in the screen corner. White vertex colour is
       * the modulation identity. */
      const tex = this.textures.get(command.handle);
      if (tex) {
        const useSrc = (command.sw ?? 0) > 0 && (command.sh ?? 0) > 0;
        const u0 = useSrc ? command.sx / tex.width : 0;
        const v0 = useSrc ? command.sy / tex.height : 0;
        const u1 = useSrc ? (command.sx + command.sw) / tex.width : 1;
        const v1 = useSrc ? (command.sy + command.sh) / tex.height : 1;
        const W = 0xffffffff;
        return [{ kind: 'mesh', handle: command.handle, vertices: [
          { x: ax, y: ay, u: u0, v: v0, rgba: W },
          { x: bx, y: by, u: u1, v: v0, rgba: W },
          { x: cx, y: cy, u: u1, v: v1, rgba: W },
          { x: ax, y: ay, u: u0, v: v0, rgba: W },
          { x: cx, y: cy, u: u1, v: v1, rgba: W },
          { x: dx, y: dy, u: u0, v: v1, rgba: W },
        ] }];
      }
    }
    /* game/surface under rotation stays axis-aligned at the transformed
     * origin: those carry raw pixels, not a persistent texture the mesh
     * path can sample. Position is honoured; the rotation is not. */
    return [{ ...command, x: Math.min(ax, bx, cx, dx), y: Math.min(ay, by, cy, dy) }];
  }

  _push(command) {
    for (const out of this._transformCommand(command)) {
      if (this.commands.length >= this.maxCommands) {
        throw new Error(`Active Bezel command limit exceeded (${this.maxCommands})`);
      }
      this.commands.push(out);
    }
  }

  /*
   * A batch of triangles with per-vertex colour and optional UVs.
   *
   * `vertices` is a flat list of {x, y, rgba, u, v}; every three make a
   * triangle. One command instead of N keeps a gradient, a polygon fan or a
   * textured mesh inside the command budget -- the same reason the tile
   * renderer needed draw_texture_rect.
   */
  mesh(vertices, handle) {
    if (!Array.isArray(vertices) || vertices.length < 3) return 0;
    this._push({ kind: 'mesh', vertices, handle: handle ?? 0 });
    return 1;
  }

  /*
   * A textured quad with PERSPECTIVE-CORRECT sampling: four corners in any
   * convex arrangement, the texture mapped as if the quad were a plane in
   * 3D. This is what makes a tilt read as a receding surface rather than a
   * PS1-style affine warp.
   *
   * `corners` is [tl, tr, br, bl], each {x, y} (UVs are the unit square).
   * The trick is that a projective map only needs ONE extra number per
   * vertex: the perspective divisor w. Split the quad on a diagonal and set
   * each corner's w from where the diagonals cross -- the standard
   * "quad with correct texture" construction -- then let the existing
   * mesh path interpolate u/w, v/w and 1/w and divide at the end.
   *
   * Falls back to a plain affine mesh when the diagonals are parallel (a
   * parallelogram), where the two are identical anyway.
   */
  /* Pass this as quad()'s handle to map the LIVE GAME FRAME onto the quad
   * instead of a texture. A game is not a texture handle -- it arrives as
   * pixels each frame -- so it needs a sentinel rather than an id. */
  static get GAME_TEXTURE() { return -1; }

  quad(corners, handle, rgba = 0xffffffff) {
    if (!Array.isArray(corners) || corners.length !== 4) return 0;
    const [tl, tr, br, bl] = corners;

    /* Intersect the diagonals tl->br and tr->bl. */
    const ax = br.x - tl.x, ay = br.y - tl.y;
    const bx = bl.x - tr.x, by = bl.y - tr.y;
    const den = ax * by - ay * bx;

    let w = [1, 1, 1, 1];
    if (Math.abs(den) > 1e-9) {
      const s = ((tr.x - tl.x) * by - (tr.y - tl.y) * bx) / den;
      const t = ((tr.x - tl.x) * ay - (tr.y - tl.y) * ax) / den;
      /* distances from each corner to the crossing point, along its own
       * diagonal, give the homogeneous weights */
      if (s > 1e-9 && s < 1 - 1e-9 && t > 1e-9 && t < 1 - 1e-9) {
        w = [1 / (1 - s), 1 / (1 - t), 1 / s, 1 / t];
      }
    }

    const v = (c, u, vv, k) => ({ x: c.x, y: c.y, u, v: vv, w: w[k], rgba });
    this._push({ kind: 'mesh', handle: handle ?? 0, vertices: [
      v(tl, 0, 0, 0), v(tr, 1, 0, 1), v(br, 1, 1, 2),
      v(tl, 0, 0, 0), v(br, 1, 1, 2), v(bl, 0, 1, 3),
    ] });
    return 1;
  }

  /* --- Offscreen surfaces --------------------------------------------------
   *
   * A surface is a guest-allocated render target that persists across frames.
   * It can be drawn into like the screen, filtered with its own shader, and
   * then used anywhere a texture handle is accepted -- draw_texture, mesh,
   * quad.
   *
   * This exists because the scene-wide effect pass runs LAST, over the
   * finished composition. That is wrong for a bezel that puts the game
   * inside an object: filtering there filters the object too, and any warp
   * in the shader fights the perspective the game was already mapped
   * through. Rendering to a surface filters FIRST, flat, at the source's own
   * scale -- so a CRT shader behaves the way it was written to, on an
   * upright picture -- and the geometry happens exactly once, afterwards.
   *
   * The CPU compositor has no shader stage, so surfaceFilter reports failure
   * rather than quietly returning an unfiltered picture that would look like
   * the effect ran and did nothing.
   */
  surfaceCreate(width, height) {
    if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height)
      || width <= 0 || height <= 0 || width * height > 16_777_216) return 0;
    const handle = this.nextTexture++;
    this.textures.set(handle, {
      pixels: new Uint8Array(width * height * 4),
      width,
      height,
      surface: true,
    });
    return handle;
  }

  /* Direct subsequent draw commands into `handle` instead of the screen. */
  surfaceTarget(handle) {
    const target = this.textures.get(handle);
    if (!target || !target.surface) return 0;
    this._push({ kind: 'surface-target', handle });
    return 1;
  }

  surfaceEnd() {
    this._push({ kind: 'surface-end' });
    return 1;
  }

  surfaceFilter(source, destination, shaderSource) {
    void source; void destination; void shaderSource;
    return 0;                          /* no shader stage on the CPU path */
  }

  /*
   * Run a multi-pass `.glslp` preset from the package into a surface.
   *
   * Single-pass shaders go through surfaceFilter; this is for the presets that
   * are a CHAIN -- crt-royale is twelve passes with six lookup textures --
   * which cannot be expressed as one fragment shader no matter how large.
   *
   * Same contract as surfaceFilter: the CPU compositor reports failure rather
   * than quietly producing an unfiltered picture.
   */
  surfacePreset(source, destination, presetPath) {
    void source; void destination; void presetPath;
    return 0;
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

  /* Per-texture sampling: 0 nearest (default), 1 linear, 2 bicubic,
   * 3 palette-indexed bicubic (needs setTexturePalette). Bit 4 (+16) =
   * REPEAT wrap. The CPU backend renders NEAREST regardless (documented
   * divergence: filtering is a GPU nicety); the GPU backend honours it in
   * the mesh/texture paths. */
  setTextureFilter(handle, mode) {
    if (!this.textures.has(handle)) return 0;
    (this.textureFilters ??= new Map()).set(handle, mode | 0);
    return 1;
  }

  /* Associate a 256x1 RGBA palette texture with an index texture whose RED
   * channel holds i/255. Mode-7-style planes stay static while the 1KB
   * palette animates every frame. */
  setTexturePalette(handle, paletteHandle) {
    if (!this.textures.has(handle) || !this.textures.has(paletteHandle)) return 0;
    (this.texturePalettes ??= new Map()).set(handle, paletteHandle);
    return 1;
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

  /* Patch a sub-rectangle of a persistent texture in place. Exists so
   * streaming planes (Mode 7 tilemaps) update a few rows per tick instead
   * of re-creating and re-uploading a 64MB texture. CPU side patches the
   * stored pixels; the GPU subclass also glTexSubImage2Ds the live GL
   * texture. */
  updateTexture(handle, x, y, w, h, pixels) {
    const texture = this.textures.get(handle);
    if (!texture) return 0;
    if (!Number.isSafeInteger(x) || !Number.isSafeInteger(y)
      || !Number.isSafeInteger(w) || !Number.isSafeInteger(h)
      || x < 0 || y < 0 || w <= 0 || h <= 0
      || x + w > texture.width || y + h > texture.height
      || pixels.length < w * h * 4) return 0;
    for (let row = 0; row < h; row++) {
      texture.pixels.set(
        pixels.subarray(row * w * 4, (row + 1) * w * 4),
        ((y + row) * texture.width + x) * 4,
      );
    }
    return 1;
  }

  destroyTexture(handle) {
    this.textureFilters?.delete(handle);
    this.texturePalettes?.delete(handle);
    return this.textures.delete(handle) ? 1 : 0;
  }

  drawTexture(handle, x, y, w, h, sx, sy, sw, sh) {
    const texture = this.textures.get(handle);
    if (!texture) return 0;
    // A source rect of (0,0,0,0) means "whole texture" so the 5-arg form and
    // every existing package keep working unchanged.
    const useSrc = (sw ?? 0) > 0 && (sh ?? 0) > 0;
    // kind:'texture' (not 'surface') so the GPU backend can use the PERSISTENT
    // texture it uploaded at createTexture time. Emitting 'surface' made the
    // GPU path re-upload the whole atlas through the shared game texture every
    // single draw -- and that branch ignored the source rect, so an atlas blit
    // came out as noise while the CPU path was correct.
    this._push({
      kind: 'texture', handle,
      pixels: texture.pixels,
      width: texture.width, height: texture.height,
      x, y, w, h,
      ...(useSrc ? { sx: sx | 0, sy: sy | 0, sw: sw | 0, sh: sh | 0 } : {}),
    });
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
    /* A host may TRANSFER the returned frame (retroemu ships it to its video
     * worker, which detaches the backing ArrayBuffer). That is a legitimate
     * zero-copy pattern, so the compositor reallocates instead of trapping:
     * a detached buffer has byteLength 0. */
    if (this.output.buffer.byteLength === 0) {
      this.output = new Uint8ClampedArray(this.outputWidth * this.outputHeight * 4);
    }
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
          gamePixels, gameWidth, gameHeight,
          this.output, this.outputWidth, this.outputHeight, command, clip,
        );
      } else if (command.kind === 'surface' || command.kind === 'texture') {
        /* Both carry pixels + an optional source rect; the CPU backend blits
         * them identically. The distinction only matters to the GPU backend,
         * which keeps a persistent texture for 'texture'. */
        (command.sampling ? blitLinear : blitNearest)(
          command.pixels, command.width, command.height,
          this.output, this.outputWidth, this.outputHeight, command, clip,
        );
      } else if (command.kind === 'rect') {
        drawRect(this.output, this.outputWidth, this.outputHeight, command, command.rgba, clip);
      } else if (command.kind === 'triangle') {
        drawTriangle(this.output, this.outputWidth, this.outputHeight, command, clip);
      } else if (command.kind === 'mesh') {
        /* handle -1 means the live game frame; anything else is a texture. */
        const tex = command.handle === -1
          ? { pixels: gamePixels, width: gameWidth, height: gameHeight }
          : (command.handle ? this.textures.get(command.handle) : null);
        for (let i = 0; i + 2 < command.vertices.length; i += 3) {
          drawMeshTriangle(this.output, this.outputWidth, this.outputHeight,
            command.vertices[i], command.vertices[i + 1], command.vertices[i + 2], tex, clip);
        }
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

/* The bitmap font and its layout constants are exported so the GPU backend can
 * emit the IDENTICAL glyph rectangles as geometry instead of re-rasterising the
 * whole scene on the CPU and blending it over. Sharing the table is what keeps
 * the two backends pixel-identical for text. */
export { LOGICAL_WIDTH, LOGICAL_HEIGHT, FONT, GLYPH_ROWS, GLYPH_COLS, GLYPH_ADVANCE };

/*
 * Walk a text command's glyphs, calling `emit(x, y, w, h)` for every lit cell.
 * Both backends drive their own rectangle drawing from this one traversal, so
 * a change to spacing or glyph shape cannot desync them.
 */
export function forEachGlyphRect(command, emit) {
  const scale = Math.max(1, command.size / GLYPH_ROWS);
  let cursor = command.x;
  for (const raw of command.text) {
    const glyph = FONT[raw.toUpperCase()] ?? FONT['?'];
    for (let row = 0; row < GLYPH_ROWS; row++) {
      for (let col = 0; col < GLYPH_COLS; col++) {
        if (glyph[row][col] === '1') {
          emit(cursor + col * scale, command.y + row * scale, scale, scale);
        }
      }
    }
    cursor += GLYPH_ADVANCE * scale;
  }
}
