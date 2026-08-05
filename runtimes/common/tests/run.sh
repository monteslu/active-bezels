#!/bin/sh
# Render-kit unit tests + a CONTROL THAT MUST FAIL.
#
# The control matters: "all tests pass" is meaningless unless the suite can
# detect a real break. It compiles the kit with colour-0 transparency removed
# and asserts the suite FAILS. If the control passes, the tests are broken,
# not the code.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
echo "== render kit =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_render_test tests/test_ab_render.c
/tmp/ab_render_test

echo "== NES profile =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_nes_test tests/test_ab_nes.c 2>/dev/null
/tmp/ab_nes_test

echo
echo "== control 1: broken atlas transparency (must FAIL) =="
mkdir -p /tmp/ab_render_ctrl
cp ab_render.h /tmp/ab_render_ctrl/
sed 's/row\[x\] = c ? pal\[c\] : 0u;/row[x] = pal[c];/' ab_render.c > /tmp/ab_render_ctrl/ab_render.c
sed 's|#include "ab_render.c"|#include "/tmp/ab_render_ctrl/ab_render.c"|' tests/test_ab_render.c > /tmp/ab_render_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_render_ctrl -o /tmp/ab_render_ctrl_test /tmp/ab_render_ctrl/test.c
if /tmp/ab_render_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the test suite cannot detect a broken renderer. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the suite has teeth."

echo
echo "== control 2: broken sprite priority (must FAIL) =="
mkdir -p /tmp/ab_nes_ctrl
cp ab_render.h ab_render.c ab_nes.h /tmp/ab_nes_ctrl/
# Draw behind-background sprites unconditionally: the priority rule is gone.
# (Early return inserted at the top of the behind-branch, so BOTH the bgpix
# path and the bgval fallback are neutralised. If this sed stops matching the
# header, the control below reports the suite as toothless -- by design.)
sed 's|if (spr & AB_NES_SPR_BEHIND) {|if (spr \& AB_NES_SPR_BEHIND) { return 1;|' \
  ab_nes.h > /tmp/ab_nes_ctrl/ab_nes.h
cp ab_nes.c /tmp/ab_nes_ctrl/
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_nes_ctrl/ab_render.c"|' \
    -e 's|#include "ab_nes.c"|#include "/tmp/ab_nes_ctrl/ab_nes.c"|' \
    tests/test_ab_nes.c > /tmp/ab_nes_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_nes_ctrl -o /tmp/ab_nes_ctrl_test /tmp/ab_nes_ctrl/test.c 2>/dev/null
if /tmp/ab_nes_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the NES tests cannot detect broken sprite priority. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the NES suite has teeth."

echo
echo "== GB profile =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_gb_test tests/test_ab_gb.c 2>/dev/null
/tmp/ab_gb_test

echo
echo "== control 3: broken GB sprite priority (must FAIL) =="
mkdir -p /tmp/ab_gb_ctrl
cp ab_render.h ab_render.c ab_gb.c /tmp/ab_gb_ctrl/
# Draw bgpriority sprites unconditionally: the GB priority rule is gone.
sed 's|  return (sp & AB_GB_PIX_SPRIO) == 0 \|\| (bp & AB_GB_PIX_ENTRY) == 0;|  return 1;|' \
  ab_gb.h > /tmp/ab_gb_ctrl/ab_gb.h
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_gb_ctrl/ab_render.c"|' \
    -e 's|#include "ab_gb.c"|#include "/tmp/ab_gb_ctrl/ab_gb.c"|' \
    tests/test_ab_gb.c > /tmp/ab_gb_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_gb_ctrl -o /tmp/ab_gb_ctrl_test /tmp/ab_gb_ctrl/test.c 2>/dev/null
if /tmp/ab_gb_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the GB tests cannot detect broken sprite priority. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the GB suite has teeth."

echo
echo "== control 4: GB valid-bit ignored (must FAIL) =="
mkdir -p /tmp/ab_gb_ctrl2
cp ab_render.h ab_render.c ab_gb.h /tmp/ab_gb_ctrl2/
# Trust the per-pixel colour plane even for pixels the core never wrote.
# 0 is a legal colour, so this silently renders unwritten pixels black --
# the bug that scored 10 carts a clean 0.000%.
sed 's|  if (f->have_percolour \&\& (bp \& AB_GB_PIX_VALID)) {|  if (f->have_percolour) {|' \
  ab_gb.c > /tmp/ab_gb_ctrl2/ab_gb.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_gb_ctrl2/ab_render.c"|' \
    -e 's|#include "ab_gb.c"|#include "/tmp/ab_gb_ctrl2/ab_gb.c"|' \
    tests/test_ab_gb.c > /tmp/ab_gb_ctrl2/test.c
$CC -std=c99 -O2 -I/tmp/ab_gb_ctrl2 -o /tmp/ab_gb_ctrl2_test /tmp/ab_gb_ctrl2/test.c 2>/dev/null
if /tmp/ab_gb_ctrl2_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the GB tests cannot detect an ignored valid bit. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the valid-bit rule is actually tested."

echo
echo "== MD profile =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_md_test tests/test_ab_md.c 2>/dev/null
/tmp/ab_md_test

echo
echo "== control 5: MD sprite presence ignored (must FAIL) =="
mkdir -p /tmp/ab_md_ctrl
cp ab_render.h ab_render.c ab_md.h /tmp/ab_md_ctrl/
# Sprites never drawn in the EMIT path (the one the tests exercise).
sed 's|else if (ob\[x \* 2 + 1\]) code = ob\[x \* 2\];|else if (0) code = ob[x * 2];|' \
  ab_md.c > /tmp/ab_md_ctrl/ab_md.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_md_ctrl/ab_render.c"|' \
    -e 's|#include "ab_md.c"|#include "/tmp/ab_md_ctrl/ab_md.c"|' \
    tests/test_ab_md.c > /tmp/ab_md_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_md_ctrl -o /tmp/ab_md_ctrl_test /tmp/ab_md_ctrl/test.c 2>/dev/null
if /tmp/ab_md_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MD tests cannot detect dropped sprites. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the MD suite has teeth."

echo
echo "== PCE profile =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_pce_test tests/test_ab_pce.c 2>/dev/null
/tmp/ab_pce_test

echo
echo "== control 6: PCE RGB565 quantisation dropped (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl/
# Expand the 3-bit VCE components the "obvious" way (c * 255 / 7) instead of
# quantising through RGB565 first. That is the exact mistake that scored 73%
# against the real core, and the suite must notice it.
sed 's@const uint32_t r8 = (r5 << 3) | (r5 >> 2);@const uint32_t r8 = (uint32_t)(((c >> 3) \& 7) * 255 / 7);@' \
  ab_pce.c > /tmp/ab_pce_ctrl/ab_pce.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_pce_ctrl/ab_render.c"|' \
    -e 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl/ab_pce.c"|' \
    tests/test_ab_pce.c > /tmp/ab_pce_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl -o /tmp/ab_pce_ctrl_test /tmp/ab_pce_ctrl/test.c 2>/dev/null
if /tmp/ab_pce_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the PCE tests cannot detect a wrong colour expansion. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the PCE colour rule is actually tested."

echo
echo "== control 7: PCE behind-background sprite priority ignored (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl2
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl2/
# Every sprite pixel claims the in-front tag, so a behind-background sprite
# wrongly covers opaque background.
sed 's@if (!priority \&\& (line\[xs\] \& 0x0F))@if (0)@' \
  ab_pce.c > /tmp/ab_pce_ctrl2/ab_pce.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_pce_ctrl2/ab_render.c"|' \
    -e 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl2/ab_pce.c"|' \
    tests/test_ab_pce.c > /tmp/ab_pce_ctrl2/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl2 -o /tmp/ab_pce_ctrl2_test /tmp/ab_pce_ctrl2/test.c 2>/dev/null
if /tmp/ab_pce_ctrl2_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the PCE tests cannot detect broken sprite priority. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the PCE priority rule is actually tested."

echo
echo "== control 8: PCE per-scanline scroll ignored (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl3
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl3/
# Consult pce_vdc_reglines but throw its scroll away, using the frame-end BYR
# for every line. That IS the pre-fix renderer, and it is what scored a
# parallax platformer 31% while the top of the screen was pixel-exact. If the suite does
# not notice, it is not actually testing the per-line path.
sed 's@cr = rl.cr; mwr = rl.mwr; bxr = rl.bxr; bg_y = rl.bgy;@cr = rl.cr; mwr = rl.mwr; bxr = rl.bxr; bg_y = (int)ab_pce_reg(f, AB_PCE_REG_BYR) + raster;@' \
  ab_pce.c > /tmp/ab_pce_ctrl3/ab_pce.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_pce_ctrl3/ab_render.c"|' \
    -e 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl3/ab_pce.c"|' \
    tests/test_ab_pce.c > /tmp/ab_pce_ctrl3/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl3 -o /tmp/ab_pce_ctrl3_test /tmp/ab_pce_ctrl3/test.c 2>/dev/null
if /tmp/ab_pce_ctrl3_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the PCE tests cannot detect an ignored per-line scroll. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the PCE per-line scroll is actually tested."

echo
echo "== control 9: PCE reglines valid bit ignored (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl4
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl4/
# Trust every slot in the per-line table, including ones the VDC never wrote.
# Scroll 0 is a LEGAL value, so an unwritten slot reads as "scroll to 0" and
# silently renders the wrong BAT row -- the same class of bug as the GB
# valid-bit control above.
sed 's@  if (!p\[12\]) return 0;@  if (0) return 0;@' \
  ab_pce.c > /tmp/ab_pce_ctrl4/ab_pce.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_pce_ctrl4/ab_render.c"|' \
    -e 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl4/ab_pce.c"|' \
    tests/test_ab_pce.c > /tmp/ab_pce_ctrl4/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl4 -o /tmp/ab_pce_ctrl4_test /tmp/ab_pce_ctrl4/test.c 2>/dev/null
if /tmp/ab_pce_ctrl4_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the PCE tests cannot detect an ignored valid bit. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the PCE valid-bit rule is actually tested."

echo
echo "== control 14: PCE burst mode painted as backdrop (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl4
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl4/
# Burst is literal black (HUC6270_PIXEL_BLACK bypasses the colour table).
# Paint it as the 0x100 idle pixel instead -- the bug that rendered Parasol
# Stars' blanked screen transition flat blue against the hardware's black.
sed 's|line\[i\] = AB_PCE_LB_BLACK;|line[i] = AB_PCE_LB_SPRITE;|g' \
  ab_pce.c > /tmp/ab_pce_ctrl4/ab_pce.c
sed 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl4/ab_pce.c"|' \
  tests/test_ab_pce.c > /tmp/ab_pce_ctrl4/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl4 -o /tmp/ab_pce_ctrl4_test /tmp/ab_pce_ctrl4/test.c 2>/dev/null
if /tmp/ab_pce_ctrl4_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the burst-black rule is NOT tested. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the burst-black rule is actually tested."

echo
echo "== MSX profile =="
$CC -std=c99 -Wall -O2 -I. -o /tmp/ab_msx_test tests/test_ab_msx.c 2>/dev/null
/tmp/ab_msx_test

echo
echo "== control 10: MSX 512-wide geometry collapsed (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl/
# Present SCREEN 6/7 at 272 like every other mode. That IS the bug that scored
# a pixel-exact SCREEN 7 renderer ~30%: the libretro shim widens the whole
# image buffer, so those frames are 544 and are never downsampled. If the suite
# does not notice, it is not testing the geometry.
sed 's|  st->out_w = st->wide ? AB_MSX_MAXW : AB_MSX_W;|  st->out_w = AB_MSX_W;|' \
  ab_msx.c > /tmp/ab_msx_ctrl/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl -o /tmp/ab_msx_ctrl_test /tmp/ab_msx_ctrl/test.c 2>/dev/null
if /tmp/ab_msx_ctrl_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect collapsed 512-wide geometry. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the MSX geometry rule is actually tested."

echo
echo "== control 11: MSX mode-2 sprite terminator wrong (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl2
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl2/
# Use the TMS9918's 208 terminator in the V9938 sprite plane. 216 is the
# mode-2 sentinel; taking 208 truncates every sprite list at the first sprite
# parked on line 208, which is a common idle position.
sed 's|    if (sy == (ab_msx_test_sprend_208 ? AB_MSX_SPR_END : AB_MSX_SPR_END2)) break;|    if (sy == AB_MSX_SPR_END) break;|' \
  ab_msx.c > /tmp/ab_msx_ctrl2/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl2/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl2/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl2/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl2 -o /tmp/ab_msx_ctrl2_test /tmp/ab_msx_ctrl2/test.c 2>/dev/null
if /tmp/ab_msx_ctrl2_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect the wrong sprite terminator. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the mode-2 terminator is actually tested."

echo
echo "== control 12: MSX VRAM interleave dropped (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl3
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl3/
# Read VRAM linearly in modes 7..12. MAP_VRAM puts even bytes in the low 64K
# and odd bytes in the high 64K; skipping it turns every SCREEN 7/8 pixel into
# noise rather than producing a subtle error.
sed 's|  if (!ab_msx_test_no_interleave \&\& st->mode >= 7 \&\& st->mode <= 12)|  if (0)|' \
  ab_msx.c > /tmp/ab_msx_ctrl3/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl3/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl3/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl3/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl3 -o /tmp/ab_msx_ctrl3_test /tmp/ab_msx_ctrl3/test.c 2>/dev/null
if /tmp/ab_msx_ctrl3_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect a dropped VRAM interleave. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the MAP_VRAM interleave is actually tested."

echo
echo "== control 13: MSX SCREEN 0 uses the TMS9918 hAdjustSc0 (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl4
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl4/
# vdpCreate sets hAdjustSc0 to -2 on the TMS9918 parts and +1 on the V9938/
# V9958. romdev runs an MSX2+ (V9958); taking -2 shifts every SCREEN 0
# character three pixels left and costs the whole text area.
sed 's|  st->hadjust_sc0 = 1;|  st->hadjust_sc0 = -2;|' \
  ab_msx.c > /tmp/ab_msx_ctrl4/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl4/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl4/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl4/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl4 -o /tmp/ab_msx_ctrl4_test /tmp/ab_msx_ctrl4/test.c 2>/dev/null
if /tmp/ab_msx_ctrl4_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect the wrong SCREEN 0 adjust. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the SCREEN 0 hAdjustSc0 is actually tested."

echo
echo "== control 14: MSX reglines valid bit ignored (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl5
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl5/
# Trust every per-scanline record, written or not. An unrendered line is all
# zeroes and 0 is a LEGAL register value, so an unchecked read renders mode 1
# with every table base at 0 instead of falling back to the frame snapshot.
# Same class of bug as the PCE profile's reglines valid bit.
sed 's|  if (!rl->valid \&\& !ab_msx_test_reglines_novalid) {|  if (0) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl5/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl5/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl5/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl5/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl5 -o /tmp/ab_msx_ctrl5_test /tmp/ab_msx_ctrl5/test.c 2>/dev/null
if /tmp/ab_msx_ctrl5_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect an ignored reglines valid bit. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the reglines valid bit is actually tested."

echo
echo "== control 15: MSX per-scanline records ignored (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl6
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl6/
# Always use the frame snapshot. That is the pre-reglines renderer, and it is
# what scored one cart 43.69% while the same frame is 99.82% once the per-line
# records are followed. If the suite does not notice, the per-line path is not
# actually being tested.
sed 's|  if (!f->have_reglines \|\| ab_msx_test_no_reglines) {|  if (1) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl6/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl6/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl6/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl6/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl6 -o /tmp/ab_msx_ctrl6_test /tmp/ab_msx_ctrl6/test.c 2>/dev/null
if /tmp/ab_msx_ctrl6_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect an ignored per-line path. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the per-scanline path is actually tested."

echo
echo "== control 16: MSX wide-mode emit squeezed to narrow footprint (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl7
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl7/
# Force the old unconditional "squeeze every mode into AB_MSX_W * scale"
# behaviour. A bezel that sized its layout from the REAL 544 width then draws
# at half the width it reserved, and every scored column past the first samples
# the wrong place: 0 of 6 wide-mode carts passed, scoring 24%-94%. This is the
# THIRD width-source mismatch in this kit (ab_pce fb_width, ab_pce render_line,
# this) -- it needs a control that names it.
sed 's|  c.px = v->fit_width ? (v->scale \* (double)AB_MSX_W / (double)f->st.out_w)|  c.px = 1 ? (v->scale * (double)AB_MSX_W / (double)f->st.out_w)|' \
  ab_msx.c > /tmp/ab_msx_ctrl7/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl7/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl7/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl7/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl7 -o /tmp/ab_msx_ctrl7_test /tmp/ab_msx_ctrl7/test.c 2>/dev/null
if /tmp/ab_msx_ctrl7_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect a squeezed wide-mode emit. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the wide-mode emit scaling is actually tested."

echo
echo "== control 17: MSX VRAM delta replay unwired (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl8
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl8/
# Never trust the delta log: every row renders from the end-of-frame snapshot.
# That IS the pre-replay renderer, and it scores one cart 91.0233% against
# a frame the replay renders at 100.0000%. If the suite does not notice, the
# replay path is not actually being tested.
sed 's|  if (f->have_vdeltas \&\& !ab_msx_test_no_vdeltas) {|  if (0) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl8/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl8/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl8/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl8/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl8 -o /tmp/ab_msx_ctrl8_test /tmp/ab_msx_ctrl8/test.c 2>/dev/null
if /tmp/ab_msx_ctrl8_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect an unwired VRAM replay. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the VRAM delta replay is actually tested."

echo
echo "== control 18: MSX sub-line split rendering disabled (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl9
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl9/
# Apply the state log per-LINE only: a mid-line palette/scroll write then lands
# at a line boundary the core plainly did not honour. One cart's palette
# gradient measures 99.8928% this way and 100.0000% with the splits.
sed 's|      if (!ab_msx_test_no_subline) {|      if (0) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl9/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl9/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl9/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl9/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl9 -o /tmp/ab_msx_ctrl9_test /tmp/ab_msx_ctrl9/test.c 2>/dev/null
if /tmp/ab_msx_ctrl9_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect disabled split rendering. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the sub-line split path is actually tested."

echo
echo "== control 19b/20: MSX frame-end-cut retention disabled (must FAIL) =="
mkdir -p /tmp/ab_msx_ctrl10
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl10/
# Never substitute the caller's prior composite. The never-re-rendered tail of
# the frame's final scanline then renders fresh where the core retains fossil
# pixels: one cart's four dumps read exactly 14/14/14/220 px short.
sed 's|    if (f->have_cut \&\& !ab_msx_test_no_retention) {|    if (0) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl10/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl10/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl10/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl10/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl10 -o /tmp/ab_msx_ctrl10_test /tmp/ab_msx_ctrl10/test.c 2>/dev/null
if /tmp/ab_msx_ctrl10_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the MSX tests cannot detect disabled retention. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the cut retention is actually tested."

echo
echo "== control 21: MSX core fossil snapshot (msx_fb_tail) unwired (must FAIL) =="
# Simulates the silent-fallback trap for the NEWEST region: a consumer that
# never trusts the core snapshot loses the first-compose fossil rows. The
# armed-prev_rows fallback is masked too, so the control isolates the
# snapshot itself.
mkdir -p /tmp/ab_msx_ctrl11
cp ab_render.h ab_render.c ab_msx.h /tmp/ab_msx_ctrl11/
sed 's|        if (f->have_fbtail \&\& !ab_msx_test_no_fbtail) {|        if (0) {|; s|        if (!done \&\& f->prev_rows \&\& f->retain_valid) {|        if (0) {|' \
  ab_msx.c > /tmp/ab_msx_ctrl11/ab_msx.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_msx_ctrl11/ab_render.c"|' \
    -e 's|#include "ab_msx.c"|#include "/tmp/ab_msx_ctrl11/ab_msx.c"|' \
    tests/test_ab_msx.c > /tmp/ab_msx_ctrl11/test.c
$CC -std=c99 -O2 -I/tmp/ab_msx_ctrl11 -o /tmp/ab_msx_ctrl11_test /tmp/ab_msx_ctrl11/test.c 2>/dev/null
if /tmp/ab_msx_ctrl11_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the suite cannot detect a lost core fossil snapshot. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the core fossil snapshot is actually tested."

echo
echo "== control 22: PCE mid-row palette split unwired (must FAIL) =="
mkdir -p /tmp/ab_pce_ctrl22
cp ab_render.h ab_render.c ab_pce.h /tmp/ab_pce_ctrl22/
sed 's|    int pev_n = ab_pce_pal_events_for_row(f, lut_vpos, pev, 16);|    int pev_n = 0;|' \
  ab_pce.c > /tmp/ab_pce_ctrl22/ab_pce.c
sed -e 's|#include "ab_render.c"|#include "/tmp/ab_pce_ctrl22/ab_render.c"|' \
    -e 's|#include "ab_pce.c"|#include "/tmp/ab_pce_ctrl22/ab_pce.c"|' \
    tests/test_ab_pce.c > /tmp/ab_pce_ctrl22/test.c
$CC -std=c99 -O2 -I/tmp/ab_pce_ctrl22 -o /tmp/ab_pce_ctrl22_test /tmp/ab_pce_ctrl22/test.c 2>/dev/null
if /tmp/ab_pce_ctrl22_test >/dev/null 2>&1; then
  echo "CONTROL PASSED -- the suite cannot detect a lost mid-row palette split. FIX THE TESTS."
  exit 1
fi
echo "control failed as required: the mid-row palette split is actually tested."
