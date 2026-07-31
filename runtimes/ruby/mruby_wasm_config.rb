# mruby cross-build for the Active Bezel Ruby runtime (emscripten, no OS deps).
#
# The host build exists ONLY to produce mrbc/presym; the emscripten build is
# the one the bezel links. Derived from wasmcart-mruby's proven config, minus
# mruby-onig-regexp and mruby-json: a bezel draws from emulator memory, it does
# not parse text, and dropping Onigmo alone saves several hundred KB of wasm.
MRuby::Build.new do |conf|
  conf.toolchain :gcc
  # Must match the cross build's MRB_INT64 (see below): mrbc's irep dump format
  # is integer-width sensitive (src/dump.c branches on MRB_INT64), so a 32-bit
  # host mrbc feeding a 64-bit target VM produces bytecode it misreads.
  conf.cc.defines << 'MRB_INT64'
  conf.gem core: 'mruby-compiler'
end

MRuby::CrossBuild.new('emscripten') do |conf|
  conf.toolchain :clang
  # Old config.subs reject wasm triples; nothing here runs a configure-time
  # execution check, so the triple only has to be one autotools recognises.
  conf.host_target = 'i686-pc-linux-gnu'
  conf.cc.command = ENV['EMCC'] || 'emcc'
  conf.cc.flags << '-Oz'
  # MRB_INT64 is REQUIRED, not a preference. mruby's default word boxing on a
  # 32-bit target shifts fixnums by one bit, so MRB_FIXNUM_MAX is INT32_MAX>>1
  # (~1.07e9) and mrb_int_value RAISES RangeError above it. Every colour a
  # bezel writes is 0xRRGGBBAA -- 0xff0000ff is 4.28e9 -- and ab_tick's frame
  # counter is a uint64. Without this, `AB.clear(0xff0000ff)` and even
  # `tick(1)` die with "integer overflow (RangeError)".
  conf.cc.defines << 'MRB_INT64'
  # MANDATORY on BOTH cc and linker: mruby's exception handling is
  # setjmp/longjmp, and emscripten's default JS-trampoline form cannot work
  # under a host that provides only the ab_host import module.
  conf.cc.flags << '-sSUPPORT_LONGJMP=wasm'
  conf.linker.command = ENV['EMCC'] || 'emcc'
  conf.linker.flags << '-sSUPPORT_LONGJMP=wasm'
  conf.archiver.command = ENV['EMAR'] || 'emar'

  # mruby-compiler is required: bezels ship main.rb as SOURCE and the runtime
  # parses it at boot, which is what makes edit + repack the whole iteration
  # story (no mrbc in the loop).
  conf.gem core: 'mruby-compiler'
  conf.gem core: 'mruby-math'
  conf.gem core: 'mruby-sprintf'
  conf.gem core: 'mruby-string-ext'
  conf.gem core: 'mruby-array-ext'
  conf.gem core: 'mruby-hash-ext'
  conf.gem core: 'mruby-numeric-ext'
  conf.gem core: 'mruby-object-ext'
  conf.gem core: 'mruby-enum-ext'
  conf.gem core: 'mruby-struct'
  conf.gem core: 'mruby-symbol-ext'
  conf.gem core: 'mruby-kernel-ext'
  conf.gem core: 'mruby-metaprog'
  conf.gem core: 'mruby-random'
  # mruby-error gives the C side mrb_protect, which is how a script error
  # becomes an on-screen panel instead of a dead session.
  conf.gem core: 'mruby-error'
end
