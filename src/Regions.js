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
  [0x106, 'nes_ppu_regs'],
  [0x110, 'snes_oam'], [0x111, 'snes_cgram'], [0x112, 'snes_aram'],
  [0x113, 'snes_fillram'],
  [0x120, 'genesis_cram'], [0x121, 'genesis_vsram'], [0x122, 'genesis_vdp_regs'],
  [0x123, 'genesis_z80_ram'], [0x124, 'genesis_m68k'], [0x125, 'genesis_ym2612'],
  [0x126, 'genesis_psg'],
  [0x130, 'sms_vram'], [0x131, 'sms_cram'], [0x132, 'sms_vdp_regs'],
  [0x133, 'sms_z80_regs'], [0x134, 'gg_vram'], [0x135, 'gg_cram'],
  [0x140, 'gb_vram'], [0x141, 'gb_oam'], [0x142, 'gb_io'], [0x143, 'gb_hram'],
  [0x144, 'gb_bgpdata'], [0x145, 'gb_objpdata'], [0x146, 'gb_cpu_regs'],
  [0x150, 'a78_cpu_regs'],
  [0x160, 'a26_tia_regs'], [0x161, 'a26_cpu_regs'],
  [0x170, 'c64_color_ram'], [0x171, 'c64_vic_regs'], [0x172, 'c64_sid_regs'],
  [0x173, 'c64_cia1_regs'], [0x174, 'c64_cia2_regs'], [0x175, 'c64_cpu_regs'],
  [0x180, 'gba_cpu_regs'], [0x181, 'gba_io_regs'], [0x182, 'gba_palette'],
  [0x183, 'gba_oam'], [0x184, 'gba_iwram'],
  [0x190, 'lynx_cpu_regs'], [0x191, 'lynx_hw_regs'],
  [0x1a0, 'pce_vdc_vram'], [0x1a1, 'pce_vdc_satb'], [0x1a2, 'pce_vdc_regs'],
  [0x1a3, 'pce_vce_palette'], [0x1a4, 'pce_cpu_regs'], [0x1a5, 'pce_psg_regs'],
  [0x1c0, 'msx_vram'], [0x1c1, 'msx_vdp_regs'], [0x1c2, 'msx_vdp_status'],
  [0x1c3, 'msx_palette'], [0x1c4, 'msx_cpu_regs'], [0x1c5, 'msx_psg_regs'],
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
