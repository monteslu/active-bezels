/*
 * Host-side image decoding, for shader lookup textures.
 *
 * A `.glslp` preset can reference LUT images -- phosphor masks, dither
 * patterns, bezel overlays, palettes. Over 200 of the presets RetroArch ships
 * need at least one, so without a decoder those presets cannot run at all.
 *
 * The decoder is stb_image compiled to wasm -- the SAME stb_image the four
 * language runtimes link for ab.image(). Guests decode inside their own
 * module; the host previously had no way to decode anything. Reusing the
 * vendored copy means one image implementation in this codebase rather than a
 * second hand-written one, and no native dependency.
 *
 * The module is loaded lazily and kept for the process: a preset chain is
 * compiled once and its LUTs decoded once, so this is a handful of calls per
 * session, not per frame.
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));

let instance = null;
let unavailable = false;

function load() {
  if (instance || unavailable) return instance;
  try {
    const bytes = readFileSync(join(HERE, 'imgdec.wasm'));
    const module = new WebAssembly.Module(bytes);
    /* The module imports nothing -- WASI stubs are linked in -- so there is
     * no import object to build and no failure mode around one. */
    instance = new WebAssembly.Instance(module, {}).exports;
    instance._initialize?.();
  } catch {
    /* A missing or unloadable decoder is not fatal: callers refuse the preset
     * with a message naming the textures, which beats taking down the frame. */
    unavailable = true;
  }
  return instance;
}

/**
 * Decode an encoded image to 8-bit RGBA, top row first.
 *
 * @param {Uint8Array|Buffer} bytes encoded image (PNG, JPEG, BMP, TGA)
 * @returns {{pixels: Uint8Array, width: number, height: number}|null}
 */
export function decodeImage(bytes) {
  const wasm = load();
  if (!wasm) return null;

  const ptr = wasm.ab_img_alloc(bytes.length);
  if (!ptr) return null;
  try {
    /* The heap view must be re-read after every call that can allocate:
     * growing wasm memory detaches the old ArrayBuffer. */
    new Uint8Array(wasm.memory.buffer).set(bytes, ptr);
    if (!wasm.ab_img_decode(ptr, bytes.length)) return null;

    const width = wasm.ab_img_width();
    const height = wasm.ab_img_height();
    const pixels = wasm.ab_img_pixels();
    if (!width || !height || !pixels) return null;

    /* Copied out, not referenced: the next decode frees this allocation, and
     * a view into wasm memory would silently become garbage. */
    return {
      pixels: new Uint8Array(wasm.memory.buffer, pixels, width * height * 4).slice(),
      width,
      height,
    };
  } finally {
    wasm.ab_img_free(ptr);
  }
}

/**
 * Decode an image file to RGBA, or null if it cannot be read or decoded.
 */
export function decodeImageFile(path) {
  try {
    return decodeImage(readFileSync(path));
  } catch {
    return null;
  }
}
