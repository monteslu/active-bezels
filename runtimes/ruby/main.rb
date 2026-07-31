# main.rb -- Active Bezel scaffold (Ruby)
#
# Copy this next to the runtime's main.wasm, edit, reload. No compiler.
#
# THE CONTRACT
#   def init          optional -- once, after the script loads
#   def tick(frame)   REQUIRED -- once per emulated frame; draw everything
#   def event(kind)   optional -- host lifecycle (see AB::EVENT)
#
# A bezel owns the WHOLE 1920x1080 picture, including where the game goes.
# Coordinates are always on that logical grid no matter the output size.
#
# This is mruby: blocks, classes, Struct, Comparable, string formatting. There
# is no filesystem, no require of gems, and no Regexp in this build -- a bezel
# reads the machine and draws, and everything it needs arrives through AB.
#
# Errors never kill the session: they land on an on-screen panel, and the
# runtime re-reads this file when the host reloads assets.

W = 1920
H = 1080

# Put the game on the left, keep a panel on the right. A 4:3 game in a
# 16:9 frame leaves ~1/4 of the screen; that leftover IS the bezel.
GAME_W = H * 4 / 3          # 1440
PANEL_X = GAME_W + 32
PANEL_W = W - GAME_W - 64

# A GLSL ES 3.00 fragment shader over the finished scene. Single-pass
# RetroArch shaders port almost verbatim: rename Texture -> u_texture,
# vTexCoord -> v_uv, FragColor -> out_color, add the #version header.
# Gate by v_uv so the game gets the treatment and the panel stays clean.
EFFECT = <<~GLSL
  #version 300 es
  precision mediump float;
  in vec2 v_uv;
  out vec4 out_color;
  uniform sampler2D u_texture;
  uniform vec2 u_resolution;
  uniform float u_time;
  void main() {
    vec4 c = texture(u_texture, v_uv);
    float gx = 1440.0 / 1920.0;
    if (v_uv.x < gx) {
      // a gentle vignette on the game only
      float d = distance(v_uv / vec2(gx, 1.0), vec2(0.5));
      c.rgb *= 1.0 - 0.35 * smoothstep(0.4, 0.85, d);
    }
    out_color = c;
  }
GLSL

def init
  # Assets live in the package. Both are optional -- guard so the scaffold
  # runs before you have added any.
  $font = AB.asset('assets/font.ttf') ? AB.font('assets/font.ttf') : nil
  $logo = AB.asset('assets/logo.png') ? AB.image('assets/logo.png') : nil

  # Live memory. Region names are platform-independent: 'system_ram' is
  # NES RAM, SNES WRAM, Game Boy WRAM... 'cart_source' is the ROM itself.
  $ram = AB.region('system_ram')

  # Shaders need the GPU backend; on the CPU reference compositor this
  # returns false and the scene simply renders unfiltered.
  AB.log('no shader backend; scene unfiltered') unless AB.effect_set(EFFECT)

  AB.log("bezel up: #{AB.logical_width}x#{AB.logical_height}")
end

def tick(frame)
  t = AB.elapsed_ms / 1000.0

  # 1. Background. Colours are 0xRRGGBBAA; AB.rgb builds one.
  AB.clear(AB.rgb(14, 16, 26))

  # 2. The live game. draw_game puts it exactly where you say; NEAREST
  #    keeps pixel art crisp. (draw_game_fit letterboxes it for you.)
  AB.draw_game(0, 0, GAME_W, H, AB::SAMPLE[:NEAREST])

  # 3. Panel background
  AB.fill_rect(GAME_W, 0, W - GAME_W, H, AB.rgb(20, 22, 34))

  # 4. Text. AB.draw_text uses the TrueType font (anti-aliased, any size);
  #    AB.text is a built-in 3x5 bitmap font -- fine for debug, not for UI.
  #    It is draw_text, NOT print: a bare print would reach Kernel#print.
  #    (AB.print is bound as an alias if you prefer it, but be explicit.)
  if $font
    AB.draw_text($font, 'ACTIVE BEZEL', PANEL_X, 80, 44, AB.rgb(235, 238, 250))
    label = format('%.1f ms', AB.delta_ms)
    w = AB.measure($font, label, 28)
    AB.draw_text($font, label, PANEL_X + PANEL_W - w, 130, 28, AB.rgb(150, 160, 190))
  else
    AB.text('ACTIVE BEZEL', PANEL_X, 80, 40, AB.rgb(235, 238, 250))
  end

  # 5. Live memory: the whole point of the format. Read the machine and
  #    show something the game never displays.
  if $ram
    b = AB.read_u8($ram, 0)
    AB.fill_rect(PANEL_X, 180, PANEL_W * (b / 255.0), 24, AB.rgb(120, 200, 255))
    if $font
      AB.draw_text($font, format('ram[0] = 0x%02X', b), PANEL_X, 240, 26,
                   AB.rgb(150, 160, 190))
    end
  end

  # 6. Transforms + geometry. Everything between push/pop is transformed;
  #    a rotated rect becomes real geometry, not a stretched box.
  AB.push_transform
  AB.translate(PANEL_X + PANEL_W / 2, H - 200)
  AB.rotate(t)
  AB.fill_rect(-40, -40, 80, 80, AB.rgb(255, 176, 32, 220))
  AB.pop_transform

  # 7. Images decoded from the package (PNG/JPG/GIF/BMP).
  if $logo
    AB.draw_texture($logo[:texture], PANEL_X, H - 120, $logo[:width], $logo[:height])
  end

  # 8. A mesh: per-vertex colours in ONE command. Use this for gradients,
  #    fans, or anything that would otherwise be hundreds of rects.
  AB.mesh([
    { x: PANEL_X,                 y: 300, rgba: AB.rgb(255, 80, 80) },
    { x: PANEL_X + PANEL_W,       y: 300, rgba: AB.rgb(80, 255, 160) },
    { x: PANEL_X + PANEL_W / 2,   y: 380, rgba: AB.rgb(80, 140, 255) }
  ])
end

def event(kind)
  # AB::EVENT[:RESET] / :STATE_LOADED / :REWIND_JUMP mean the machine
  # jumped: drop any cached decode of game state here.
  AB.log('machine reset') if kind == AB::EVENT[:RESET]
end
