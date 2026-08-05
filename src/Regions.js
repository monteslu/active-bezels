import { resolveCoreModule, coreHeap, coreMemoryObject } from './Host.js';

// Flags are part of active-bezel-1, not libretro:
//   1 = readable, 2 = writable, 4 = live core memory, 8 = immutable.
//
// 0x100+ are the stable romdev extensions implemented by the patched cores
// retroemu already consumes. Keeping the same names and numeric IDs makes an
// Active Bezel portable between retroemu and romdev's inspection tools.
export const CORE_REGIONS = [
  [0x000, 'save_ram'], [0x001, 'rtc'], [0x002, 'system_ram'], [0x003, 'video_ram'],
  [0x100, 'nes_nametables'], [0x101, 'nes_palette'], [0x102, 'nes_oam'],
  [0x103, 'nes_chr'], [0x104, 'nes_apu_regs'], [0x105, 'nes_cpu_regs'],
  [0x106, 'nes_ppu_regs'], [0x107, 'nes_cart_ram'],
  // 0x108: the PPU's LATCHED scroll (x, y, ntSelect, pad). PPUSCROLL is
  // write-only, so this is the ONLY generic scroll source -- the alternative is
  // per-cartridge RAM reverse engineering that is also a frame stale.
  [0x108, 'nes_ppu_scroll'],
  // 0x109: PER-SCANLINE scroll, 240 x [x, y, ntSelect, pad]. The only way to
  // reproduce a mid-frame scroll change -- see the note on 0x108.
  [0x109, 'nes_ppu_scroll_lines'],
  // 0x10A: per-scanline BACKGROUND CHR (240 x 4KB). Needed for MMC2/MMC4, which
  // swap a CHR bank on tile fetch, so one tile index means different pixels in
  // different parts of a single frame.
  [0x10A, 'nes_chr_lines'],
  // 0x10B: per-TILE background pattern bytes captured at fetch time
  // (240 x 34 x 2). The only representation correct for MMC2/MMC4.
  [0x10B, 'nes_bgfetch'],
  // 0x10C: per-scanline EVALUATED sprites (240 x 8 x [lo,hi,x,attr]). Correct
  // even when a game rewrites OAM mid-frame; attr 0xFF marks an empty slot.
  [0x10C, 'nes_sprlines'],
  // 0x10D: logical -> physical nametable map (the mapper's own vnapage). The
  // authoritative mirroring; do not re-derive it from the iNES header.
  [0x10D, 'nes_ntmap'],
  // 0x10E: per-scanline palette (240 x 32). Games recolour mid-frame; a single
  // frame-end read misses colours that are genuinely on screen.
  [0x10E, 'nes_pallines'],
  // 0x10F: per-scanline PPUMASK. Layer-enable and the left-column clip are
  // changed mid-frame; a frame-end read hides layers that are on screen.
  [0x10F, 'nes_masklines'],
  // 0x110: per-scanline nametable map. Mirroring can change MID-FRAME
  // (AxROM one-screen flips), so the frame-end map is not enough.
  [0x110, 'nes_ntmaplines'],
  // 0x111: per-PIXEL resolved background palette index (240 x 256). Ground
  // truth for the background layer -- no reconstruction hazards at all.
  [0x111, 'nes_bgpix'],
  // 0x112: per-scanline backdrop (palette entry 0) sampled where the PPU
  // commits to it, not at line entry -- see the core patch for why.
  [0x112, 'nes_backdrop'],
  // 0x113: per-scanline DRAWN sprites -- what RefreshSprites actually paints,
  // which differs from the evaluated set when sprites are re-enabled mid-frame.
  [0x113, 'nes_sprdrawn'],
  // 0x114: per-PIXEL resolved background palette VALUE. Removes the pallines
  // lookup, and with it every mid-line palette-write hazard.
  [0x114, 'nes_bgval'],
  // 0x115: per-PIXEL PPUMASK -- emphasis and greyscale change mid-line.
  [0x115, 'nes_maskpix'],
  // 0x116/0x117: the FINAL emitted planes -- XBuf pixels and the per-line
  // emphasis byte, captured after greyscale and emphasis are applied.
  [0x116, 'nes_linepix'], [0x117, 'nes_linedeemp'],
  // 0x118: the ACTIVE 64-entry PPU palette as RGB. VS System carts select
  // rp2c03/rp2c04 variants, so a baked-in NTSC table renders them wrong.
  [0x118, 'nes_palrgb'],
  // 0x110-0x113 are shared with the NES redraw planes above. This is NOT a
  // mistake to "fix" by renumbering: both id sets are baked into the
  // COMPILED cores (ROMDEV_MEMORY_* in the fceumm/snes9x patches), and only
  // one core is ever loaded at a time, so the ids are disjoint in practice.
  // The catalog is a union of every platform, which is why they look like
  // collisions here. Renumbering the JS alone makes the host ask a core for
  // ids it does not implement -- verified: SNES reads went empty.
  [0x110, 'snes_oam'], [0x111, 'snes_cgram'], [0x112, 'snes_aram'],
  [0x113, 'snes_fillram'],
  [0x120, 'genesis_cram'], [0x121, 'genesis_vsram'], [0x122, 'genesis_vdp_regs'],
  [0x123, 'genesis_z80_ram'], [0x124, 'genesis_m68k'], [0x125, 'genesis_ym2612'],
  [0x126, 'genesis_psg'],
  // gpgx resolved-layer capture (md-redraw). File-scope arrays in
  // vdp_render.c, stable pointers, same seam serves genesis/sms/gg (the
  // md_* names resolve on every gpgx platform; the redraw bezel probes
  // md_/sms_/gg_ prefixes and takes whichever the host registered).
  [0x127, 'md_linepix'], [0x128, 'md_bgpix'], [0x129, 'md_objpix'],
  [0x12A, 'md_pixrgb'], [0x12B, 'md_linestate'], [0x12C, 'md_pixlines'],
  // snes9x resolved-frame capture (snes-redraw): final RGB565 lines + regs
  [0x1B0, 'snes_linepix'], [0x1B1, 'snes_linestate'], [0x1B2, 'snes_frameinfo'],
  [0x1B3, 'snes_m7lines'],
  [0x1B4, 'snes_linedepth'],
  [0x1B5, 'snes_cliplines'],
  [0x130, 'sms_vram'], [0x131, 'sms_cram'], [0x132, 'sms_vdp_regs'],
  [0x133, 'sms_z80_regs'], [0x134, 'gg_vram'], [0x135, 'gg_cram'],
  [0x140, 'gb_vram'], [0x141, 'gb_oam'], [0x142, 'gb_io'], [0x143, 'gb_hram'],
  [0x144, 'gb_bgpdata'], [0x145, 'gb_objpdata'], [0x146, 'gb_cpu_regs'],
  // GB/GBC universal-redraw capture planes. NOT in SNAPSHOT_IDS: these are
  // file-scope arrays in the core, so their pointers are stable for the
  // lifetime of the module — unlike the fill-a-buffer getters above.
  [0x147, 'gb_lineregs'], [0x148, 'gb_bgpix'], [0x149, 'gb_sprpix'],
  [0x14a, 'gb_palline'], [0x14b, 'gb_bgcol15'], [0x14c, 'gb_sprcol15'],
  [0x150, 'a78_cpu_regs'],
  [0x160, 'a26_tia_regs'], [0x161, 'a26_cpu_regs'],
  [0x170, 'c64_color_ram'], [0x171, 'c64_vic_regs'], [0x172, 'c64_sid_regs'],
  [0x173, 'c64_cia1_regs'], [0x174, 'c64_cia2_regs'], [0x175, 'c64_cpu_regs'],
  [0x180, 'gba_cpu_regs'], [0x181, 'gba_io_regs'], [0x182, 'gba_palette'],
  [0x183, 'gba_oam'], [0x184, 'gba_iwram'],
  [0x190, 'lynx_cpu_regs'], [0x191, 'lynx_hw_regs'],
  [0x1a0, 'pce_vdc_vram'], [0x1a1, 'pce_vdc_satb'], [0x1a2, 'pce_vdc_regs'],
  [0x1a3, 'pce_vce_palette'], [0x1a4, 'pce_cpu_regs'], [0x1a5, 'pce_psg_regs'],
  // Per-scanline latched VDC state. NOT in SNAPSHOT_IDS: a file-scope array in
  // the core, so its pointer is stable — unlike pce_cpu_regs/pce_psg_regs,
  // which are fill-a-buffer getters.
  [0x1a6, 'pce_vdc_reglines'],
  // 0x1a7: per-scanline VCE palette (263 x 512 x u16). m_color_table is
  // written by the CPU at any time and resolved LIVE per pixel, so a
  // frame-end pce_vce_palette read misses mid-frame recolours -- one
  // title screen scored 43.9% with two blues swapped over most of the frame.
  // Same class as nes_pallines.
  [0x1a7, 'pce_vce_pallines'],
  // 0x1a8: the VDC's OWN resolved line buffer, per scanline (263 x 1024 u16).
  // VRAM is DMA'd DURING display (HuC6270::VRAMTransfer runs from Clock), so
  // a post-frame pce_vdc_vram snapshot holds only the final patterns while
  // the frame showed several. The resolved buffer is what the VRAM was for --
  // same rule as nes_bgpix/nes_linepix: read what the core resolved.
  [0x1a8, 'pce_vdc_linepix'],
  // 0x1a9: per-scanline framebuffer x where the VDC's picture starts
  // (0xFFFF = no picture). The VDC's HSW/HDS chain and the VCE's
  // m_screen_start_x are different clock domains, so the obvious arithmetic
  // is right for most games and 8px wrong for others.
  [0x1a9, 'pce_vce_xofflines'],
  // 0x1aa: companion to 0x1a9 -- WHICH line-buffer pixel the row's first
  // copied pixel came from. Non-zero when the VDC's active window is wider
  // than the VCE copies, so the picture is CLIPPED on the left (Legend of
  // carts programming a 352px window into a 341px line).
  [0x1aa, 'pce_vce_srclines'],
  // 0x1ab: dot-stamped VCE palette write log -- {count u16, truncated u8, pad}
  // + 8192 x {vpos u16, hpos u16, dot u16, index u16, oldv u16, newv u16}.
  // pallines is per-LINE (end-of-line table); a mid-line colour write (Eaggy)
  // renders the row's head through the OLD value, unrecoverable from any
  // per-line snapshot. Undo this line's entries (oldv, reverse) from
  // pallines[vpos] to get the line-START table, redo forward at each dot.
  // Same silent-fallback trap as every optional region in this table.
  [0x1ab, 'pce_paldeltas'],
  [0x1c0, 'msx_vram'], [0x1c1, 'msx_vdp_regs'], [0x1c2, 'msx_vdp_status'],
  [0x1c3, 'msx_palette'], [0x1c4, 'msx_cpu_regs'], [0x1c5, 'msx_psg_regs'],
  // 0x1c6: per-scanline VDP state, 256 records of {regs[64], palette[16],
  // status2, valid, line, firstLine, activeLines, displayOffest}. Without it a
  // bezel silently falls back to the single frame snapshot and loses every
  // mid-frame change -- one cart scores 43.7% instead of 99.8%. The fallback is
  // byte-identical to the pre-reglines renderer, so the omission produces NO
  // error anywhere; it just quietly renders the old, worse picture.
  [0x1c6, 'msx_vdp_reglines'],
  // 0x1c7: per-frame VRAM delta log -- {count u16, truncated u8, pad} + 8192 x
  // {line u16, oldv u8, newv u8, addr u32}. Without it a bezel cannot follow a
  // game that rewrites VRAM mid-frame with constant registers (one cart
  // scored 91% from the snapshot alone). Same silent-fallback trap as 0x1c6:
  // leaving it out of THIS table makes ab_region_find fail with no error.
  [0x1c7, 'msx_vram_deltas'],
  // 0x1c8: fossil-row snapshot -- {cutLine u16, cutOffset s16, rows u16,
  // width u16, firstRow u16, pad} + 32 rows x 544 u16 framebuffer pixels,
  // captured by the CORE at frame start (framebuffer still holds the previous
  // presentation). The rows at/beyond the frame-end cut were never
  // re-rendered by the frame the state describes; no state-only renderer can
  // produce them, and bezel ticks fire per COMPOSE (not per frame) so
  // "retain our own prior composite" has no usable prior on a first compose
  // and a CORRUPT one on a stale compose. Same silent-fallback trap as 0x1c6.
  [0x1c8, 'msx_fb_tail'],
].map(([id, name]) => ({ id, name, flags: 1 | 2 | 4 }));

export class ActiveBezelRegions {
  constructor(host, romBytes) {
    this.host = host;
    this.romBytes = Buffer.from(romBytes);
    this.generation = 1;
    this.refresh();
  }

  refresh() {
    const next = [];
    const mod = resolveCoreModule(this.host);
    for (const spec of CORE_REGIONS) {
      const ptr = mod?._retro_get_memory_data(spec.id) >>> 0;
      const size = mod?._retro_get_memory_size(spec.id) >>> 0;
      if (ptr && size) {
        next.push({ ...spec, size, ptr, memory: coreMemoryObject(mod) ?? coreHeap(mod)?.buffer });
      }
    }
    next.push({ id: 0x10000, name: 'cart_source', flags: 1 | 8, size: this.romBytes.length, bytes: this.romBytes });
    this.regions = next;
    this.byName = new Map(next.map((region) => [region.name, region]));
    this.generation++;
    return next;
  }

  validateRequirements(requirements) {
    const missing = [];
    for (const requirement of requirements) {
      const region = this.byName.get(requirement.region);
      if (!region || region.size < (requirement.minSize ?? 1)) missing.push(requirement);
    }
    return missing;
  }

  /* Some romdev regions are SNAPSHOT BUFFERS, not live pointers: the core's
   * retro_get_memory_data fills a staging buffer at CALL time (NES CHR is
   * memcpy'd out of VPage[], the APU/CPU register files are synthesised). A
   * guest that resolves the pointer once at init then reads it every frame sees
   * whatever was in that buffer at BOOT -- for CHR RAM that is all $FF, which
   * decodes to a single flat colour and looks exactly like a broken renderer.
   *
   * Re-invoking the getter is what refills them, so do that once per frame for
   * the affected ids. Live-pointer regions (system_ram, nametables, palette,
   * OAM, cart RAM) are unaffected and skipped. */
  // Regions whose getter FILLS A BUFFER at call time rather than returning a
  // pointer to live memory. These MUST be re-resolved every tick: a pointer
  // resolved once at init reads the boot-time contents forever. 0x103 (CHR)
  // silently did exactly that -- every tile decoded to one flat colour -- and
  // 0x108 (scroll) has the same shape, so it belongs here too.
  // 0x147-0x14C (GB capture planes) are here for a DIFFERENT reason than the
  // NES entries: their pointers ARE stable, but the bezel resolves regions
  // during init — before the first retro_run — and gambatte only allocates /
  // fills them once emulation starts. A pointer captured at init reads zeros
  // forever, which rendered whole frames black while the same regions read
  // correctly over MCP. Re-resolving per tick costs one call and cannot go
  // stale.
  // 0x127-0x12B (gpgx md_* capture planes): same shape as the GB entries --
  // stable file-scope arrays, but a pointer captured at load read all-zeros
  // in practice (the coin-catch smoke rendered pure black while the same
  // regions read correctly over MCP). Re-resolving per tick fixed it there
  // and fixes it here.
  static SNAPSHOT_IDS = new Set([0x103, 0x104, 0x105, 0x108, 0x109, 0x10A, 0x10B, 0x10C, 0x10D, 0x10E, 0x10F, 0x110, 0x111, 0x112, 0x113, 0x114, 0x115, 0x116, 0x117, 0x118,
    0x127, 0x128, 0x129, 0x12A, 0x12B, 0x12C, 0x1B0, 0x1B1, 0x1B2, 0x1B3, 0x1B4, 0x1B5,
    0x147, 0x148, 0x149, 0x14A, 0x14B, 0x14C]);

  refreshSnapshots() {
    const mod = resolveCoreModule(this.host);
    if (!mod) return;
    for (const region of this.regions) {
      if (!ActiveBezelRegions.SNAPSHOT_IDS.has(region.id)) continue;
      const ptr = mod._retro_get_memory_data(region.id) >>> 0;
      if (ptr) region.ptr = ptr;
    }
  }

  read(index, offset) {
    const region = this.regions[index];
    if (!region || offset < 0 || offset >= region.size) return 0;
    if (region.bytes) return region.bytes[offset];
    // Resolved per access, never cached: an Emscripten heap is detached and
    // replaced on growth, and a stale view reads zeros rather than throwing --
    // which looks like "the game state is empty", the worst way to fail here.
    const heap = coreHeap(resolveCoreModule(this.host));
    return heap ? heap[region.ptr + offset] : 0;
  }

  /* Copy a SPAN of a region into a caller-provided Uint8Array view.
   *
   * The per-byte `read` above is correct but costs a wasm->JS crossing per
   * byte: a guest snapshotting two 256x240 NES planes made 122,880 calls a
   * frame, which measured as the dominant cost of an otherwise-C renderer.
   * One subarray().set() replaces all of them.
   *
   * Returns the number of bytes copied (0 if the region or range is bad).
   * The heap is resolved per call for the same reason `read` does it: an
   * Emscripten heap is detached and replaced on growth, and a cached view
   * silently reads zeros -- which looks like "the game state is empty".
   */
  readInto(index, offset, dst) {
    const region = this.regions[index];
    if (!region || !dst || offset < 0 || offset >= region.size) return 0;
    const n = Math.min(dst.length, region.size - offset);
    if (n <= 0) return 0;
    if (region.bytes) {
      dst.set(region.bytes.subarray(offset, offset + n));
      return n;
    }
    const heap = coreHeap(resolveCoreModule(this.host));
    if (!heap) return 0;
    const start = region.ptr + offset;
    dst.set(heap.subarray(start, start + n));
    return n;
  }

  write(index, offset, value) {
    const region = this.regions[index];
    if (!region || !(region.flags & 2) || offset < 0 || offset >= region.size) return 0;
    const heap = coreHeap(resolveCoreModule(this.host));
    if (!heap) return 0;
    heap[region.ptr + offset] = value & 0xff;
    return 1;
  }

  describe() {
    return this.regions.map(({ id, name, size, flags, ptr }) => ({
      id, name, size, flags, offset: ptr ?? 0,
    }));
  }
}
