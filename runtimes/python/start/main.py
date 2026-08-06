# main.py -- Active Bezel scaffold (Python)
#
# Copy this next to the runtime's main.wasm, edit, reload. No compiler.
#
# THE CONTRACT
#   def init():        optional -- once, after the script loads
#   def tick(frame):   REQUIRED -- once per emulated frame; draw everything
#   def event(kind):   optional -- host lifecycle (see ab.EVENT)
#   def pre_frame(frame): optional -- BEFORE the core runs `frame`:
#     write regions / ab.input_override(port, dev, idx, id, value) to shape
#     the frame about to run (overrides clear each frame; ab.input still
#     reports the physical pad)
#
# A bezel owns the WHOLE 1920x1080 picture, including where the game goes.
# Coordinates are always on that logical grid no matter the output size.
#
# This is MicroPython: the object model, floats, string formatting and math
# are all here, but there is no filesystem, no os and no pip -- a bezel reads
# the machine and draws, and everything it needs arrives through `ab`.
#
# Errors never kill the session: they land on an on-screen panel, and the
# runtime re-reads this file when the host reloads assets.

import math

W, H = 1920, 1080

# Put the game on the left, keep a panel on the right. A 4:3 game in a
# 16:9 frame leaves ~1/4 of the screen; that leftover IS the bezel.
GAME_W = H * 4 // 3          # 1440
PANEL_X = GAME_W + 32
PANEL_W = W - GAME_W - 64

font = None    # TrueType handle (assets/font.ttf)
logo = None    # decoded image dict {'texture':, 'width':, 'height':}
ram = None     # live memory region id

# A GLSL ES 3.00 fragment shader over the finished scene. Single-pass
# RetroArch shaders port almost verbatim: rename Texture -> u_texture,
# vTexCoord -> v_uv, FragColor -> out_color, add the #version header.
# Gate by v_uv so the game gets the treatment and the panel stays clean.
EFFECT = """#version 300 es
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
}"""


def init():
    global font, logo, ram

    # Assets live in the package. Both are optional -- guard so the scaffold
    # runs before you have added any.
    if ab.asset('assets/font.ttf'):
        font = ab.font('assets/font.ttf')
    if ab.asset('assets/logo.png'):
        logo = ab.image('assets/logo.png')

    # Live memory. Region names are platform-independent: 'system_ram' is
    # NES RAM, SNES WRAM, Game Boy WRAM... 'cart_source' is the ROM itself.
    ram = ab.region('system_ram')

    # Shaders need the GPU backend; on the CPU reference compositor this
    # returns False and the scene simply renders unfiltered.
    if not ab.effect_set(EFFECT):
        ab.log('no shader backend; scene unfiltered')

    ab.log('bezel up: %dx%d' % (ab.logical_width(), ab.logical_height()))


def tick(frame):
    t = ab.elapsed_ms() / 1000.0

    # 1. Background. Colours are 0xRRGGBBAA; ab.rgb() builds one.
    ab.clear(ab.rgb(14, 16, 26))

    # 2. The live game. draw_game puts it exactly where you say; NEAREST
    #    keeps pixel art crisp. (draw_game_fit letterboxes it for you.)
    ab.draw_game(0, 0, GAME_W, H, ab.SAMPLE['NEAREST'])

    # 3. Panel background
    ab.fill_rect(GAME_W, 0, W - GAME_W, H, ab.rgb(20, 22, 34))

    # 4. Text. ab.draw_text uses the TrueType font (anti-aliased, any size);
    #    ab.text is a built-in 3x5 bitmap font -- fine for debug, not for UI.
    #    (It is draw_text, not print, because print() is Python's own.)
    if font:
        ab.draw_text(font, 'ACTIVE BEZEL', PANEL_X, 80, 44, ab.rgb(235, 238, 250))
        label = '%.1f ms' % ab.delta_ms()
        w = ab.measure(font, label, 28)
        ab.draw_text(font, label, PANEL_X + PANEL_W - w, 130, 28, ab.rgb(150, 160, 190))
    else:
        ab.text('ACTIVE BEZEL', PANEL_X, 80, 40, ab.rgb(235, 238, 250))

    # 5. Live memory: the whole point of the format. Read the machine and
    #    show something the game never displays.
    if ram is not None:
        b = ab.read_u8(ram, 0)
        ab.fill_rect(PANEL_X, 180, PANEL_W * (b / 255), 24, ab.rgb(120, 200, 255))
        if font:
            ab.draw_text(font, 'ram[0] = 0x%02X' % b, PANEL_X, 240, 26,
                         ab.rgb(150, 160, 190))

    # 6. Transforms + geometry. Everything between push/pop is transformed;
    #    a rotated rect becomes real geometry, not a stretched box.
    ab.push_transform()
    ab.translate(PANEL_X + PANEL_W / 2, H - 200)
    ab.rotate(t)
    ab.fill_rect(-40, -40, 80, 80, ab.rgb(255, 176, 32, 220))
    ab.pop_transform()

    # 7. Images decoded from the package (PNG/JPG/GIF/BMP).
    if logo:
        ab.draw_texture(logo['texture'], PANEL_X, H - 120, logo['width'], logo['height'])

    # 8. A TILTED surface: four arbitrary corners with perspective-correct
    #    texturing. quad() foreshortens the texture toward the far edge, so a
    #    receding plane reads as depth rather than as a PS1-style warp.
    #    (ab.skew() shears the transform stack if you want a lean instead.)
    if logo:
        lean = math.sin(t * 0.6) * 120
        ab.quad([
            {'x': PANEL_X + 120 + lean, 'y': H - 420},           # far, narrower
            {'x': PANEL_X + PANEL_W - 120 + lean, 'y': H - 420},
            {'x': PANEL_X + PANEL_W, 'y': H - 200},              # near, full width
            {'x': PANEL_X, 'y': H - 200},
        ], logo['texture'])

    # 9. A mesh: per-vertex colours in ONE command. Use this for gradients,
    #    fans, or anything that would otherwise be hundreds of rects.
    #    Vertices are dicts (readable) or 5-tuples (x, y, rgba, u, v).
    ab.mesh([
        {'x': PANEL_X,               'y': 300, 'rgba': ab.rgb(255, 80, 80)},
        {'x': PANEL_X + PANEL_W,     'y': 300, 'rgba': ab.rgb(80, 255, 160)},
        {'x': PANEL_X + PANEL_W / 2, 'y': 380, 'rgba': ab.rgb(80, 140, 255)},
    ])


def event(kind):
    # ab.EVENT['RESET'] / 'STATE_LOADED' / 'REWIND_JUMP' mean the machine
    # jumped: drop any cached decode of game state here.
    if kind == ab.EVENT['RESET']:
        ab.log('machine reset')
