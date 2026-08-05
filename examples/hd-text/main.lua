-- HD text: replace a game's 8x8 font tiles with real letterforms.
--
-- Substitution is PER TILE, never per string. Each frame we walk the
-- tilemap, mask every cell holding a font tile, and print one glyph in
-- its place. Nothing here understands words or layout -- that is the
-- point. A glyph lands exactly where its tile was, so the typewriter
-- reveal, menu columns, centered titles, indentation and the selection
-- cursor are all automatically correct.
--
-- See docs/HD_TEXT.md for the reasoning and the traps. The short list:
--   * the font MUST be stretched (tools/make_tile_font.py); an ordinary
--     font leaves gaps that dissolve word shapes
--   * mask ALL cells before printing ANY glyph, or descenders get eaten
--   * verify the charset by rendering tiles at 10x, never by guessing
--   * text colour is the attribute line's colour INDEX 1
--
-- This example targets the NES; the only platform-specific part is
-- cells_on_screen(). Port that and the rest is unchanged.

local SCALE  = 4
local CELL   = 8 * SCALE            -- on-screen cell pitch
local COLS, ROWS = 32, 30
-- centre the 1024x896 (256x224 at 4x) game view in 1080p
local OX, OY = (1920 - 1024) // 2, (1080 - 896) // 2

local TEXT_PX  = 39                 -- cap height ~= 0.875 * CELL
local BASELINE = 28

-- ---------------------------------------------------------------- charset --
-- Tile id -> string, decoded from the GAME'S OWN screens: find text you can
-- see, dump the tilemap, align. Ids left unmapped keep their original art,
-- which is how cursors/arrows survive and unknown punctuation degrades
-- gracefully. A few entries are two characters because some games pack them
-- into one tile (here: period + closing quote).
local GLYPH = {}
for i = 0, 9  do GLYPH[i]        = string.char(48 + i) end   -- 0-9
for i = 0, 25 do GLYPH[0x0A + i] = string.char(97 + i) end   -- a-z
for i = 0, 25 do GLYPH[0x24 + i] = string.char(65 + i) end   -- A-Z
GLYPH[0x47] = '.'
GLYPH[0x48] = ','
GLYPH[0x49] = '-'
GLYPH[0x4B] = '?'
GLYPH[0x4C] = '!'
GLYPH[0x4E] = ')'
GLYPH[0x4F] = '('
GLYPH[0x40] = "'"
GLYPH[0x50] = "'"
GLYPH[0x51] = "'"
GLYPH[0x53] = "'"
GLYPH[0x52] = ".'"      -- genuinely one tile holding two characters

local font, r_nt, r_pal, r_prgb, r_scroll, r_ntmap

function init()
  r_nt     = ab.region('nes_nametables')
  r_pal    = ab.region('nes_palette')
  r_prgb   = ab.region('nes_palrgb')
  r_scroll = ab.region('nes_ppu_scroll')
  r_ntmap  = ab.region('nes_ntmap')
  if not (r_nt and r_pal and r_prgb) then
    error('hd-text: required NES regions missing')
  end
  font = ab.font('assets/tilefont.ttf')
  return 0
end

-- Text colour: the tile's attribute palette line, COLOUR INDEX 1, resolved
-- live. ("Brightest colour in the line" renders white text pink.) Reading it
-- live also means palette swaps and animation follow for free.
local function text_color(line)
  local v = ab.read_u8(r_pal, line * 4 + 1) & 0x3F
  return ab.rgb(ab.read_u8(r_prgb, v * 3),
                ab.read_u8(r_prgb, v * 3 + 1),
                ab.read_u8(r_prgb, v * 3 + 2))
end

-- The one platform-specific piece: which tilemap byte backs each screen
-- cell. NES free-scrolls in four directions, so world position is base
-- nametable origin + scroll, resolved through the live mirroring table.
local function cells_on_screen()
  local sx   = ab.read_u8(r_scroll, 0)
  local sy   = ab.read_u8(r_scroll, 1)
  local base = ab.read_u8(r_scroll, 2) & 3
  -- text only appears at rest; skip partially scrolled frames entirely
  if (sx % 8) ~= 0 or (sy % 8) ~= 0 then return {}, {} end

  local wx0 = (base & 1) * 256 + sx
  local wy0 = (base >> 1) * 240 + sy

  local cells, n = {}, 0
  local rowsig = {}                 -- for the scroll-doubling guard below
  for r = 0, ROWS - 2 do
    local wy    = (wy0 + 8 + r * 8) % 480
    local ntrow = (wy % 240) // 8
    for c = 0, COLS - 1 do
      local wx     = (wx0 + c * 8) % 512
      local lognt  = ((wy >= 240) and 2 or 0) + ((wx >= 256) and 1 or 0)
      local page   = ab.read_u8(r_ntmap, lognt)
      local ntcol  = (wx % 256) // 8
      local addr   = page * 0x400 + ntrow * 32 + ntcol
      local tile   = ab.read_u8(r_nt, addr)
      local glyph  = GLYPH[tile]
      if glyph then
        local attr = ab.read_u8(r_nt, page * 0x400 + 0x3C0
                                      + (ntrow // 4) * 8 + ntcol // 4)
        local line = (attr >> (((ntrow % 4) // 2) * 4
                             + ((ntcol % 4) // 2) * 2)) & 3
        n = n + 1
        cells[n] = { x = OX + c * CELL, y = OY + r * CELL,
                     g = glyph, line = line, r = r }
        rowsig[r] = (rowsig[r] or '') .. c .. ':' .. tile .. ';'
      end
    end
  end
  return cells, rowsig
end

local function draw_text_layer()
  local cells, rowsig = cells_on_screen()

  -- Pass 1: mask EVERY text cell first. Lowercase descenders reach into
  -- the row below, so a single combined loop erases the tails of the line
  -- above on consecutive dialogue rows.
  for i = 1, #cells do
    ab.fill_rect(cells[i].x, cells[i].y, CELL, CELL, ab.rgb(0, 0, 0))
  end

  -- Pass 2: print, centred in the cell. During a dialogue scroll-up the
  -- game can hold the same line in two adjacent rows for a frame or two
  -- (copy up, clear later); print only the upper copy or it visibly
  -- doubles.
  for i = 1, #cells do
    local cell = cells[i]
    if rowsig[cell.r] ~= rowsig[cell.r - 1] then
      local adv = ab.measure(font, cell.g, TEXT_PX)
      ab.draw_text(font, cell.g, cell.x + (CELL - adv) // 2,
               cell.y + BASELINE, TEXT_PX, text_color(cell.line))
    end
  end
end

function tick(frame)
  ab.clear(ab.rgb(12, 12, 20))
  ab.draw_game(OX, OY, COLS * CELL, (ROWS - 2) * CELL)
  draw_text_layer()
end

function event(kind) end
