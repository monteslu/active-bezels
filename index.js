/*
 * active-bezel — the reference runtime for the Active Bezel format.
 *
 * An Active Bezel is an optional executable companion to a specific ROM,
 * distributed as a `.ab` file (an ordinary ZIP). It starts with the ROM, runs
 * once per emulated frame, reads the emulator's live memory regions, and
 * renders the complete final scene: maps, HUDs, reconstructed world graphics,
 * artwork, hints, or effects, around or over the original game picture.
 *
 * The host keeps emulation timing, input, audio, windowing, presentation, its
 * own trusted overlays, package loading, and compatibility matching. The
 * package owns the visual composition for that frame.
 *
 * See docs/ACTIVE_BEZELS.md for the format and ABI.
 */

export { ActiveBezelPackage, validateManifest } from './src/Package.js';
export { matchActiveBezel } from './src/Matcher.js';
export { ActiveBezelRegions, CORE_REGIONS } from './src/Regions.js';
export { ActiveBezelConfig } from './src/Config.js';
export { ActiveBezelRuntime, AB_EVENT } from './src/Runtime.js';
export { ActiveBezelCompositor } from './src/Compositor.js';
// Inject a host's already-loaded native-gles so a SYMLINKED copy of this
// package cannot pull in a SECOND native addon. See GpuCompositor.js.
export { setGlModule } from './src/GpuCompositor.js';
export { resolveCoreModule, coreHeap, coreMemoryObject, asBezelHost } from './src/Host.js';

/*
 * The GPU compositor is deliberately NOT re-exported here.
 *
 * It needs a GL context, and the common cases for this package are headless:
 * a screenshot pipeline, a test run, an MCP server compositing without a
 * window. `Runtime` already falls back to the CPU compositor when GL is
 * unavailable, so nothing is lost by making the GL path an explicit import:
 *
 *     import { ActiveBezelGpuCompositor } from 'active-bezel/gpu-compositor';
 */
