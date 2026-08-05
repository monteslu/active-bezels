-- Lua Native Starter -- the whole ab API with no compiler in the loop.
--
-- Contract: define a global tick(frame). init() and event(kind) are optional.
-- Iterate by editing this file and repacking; the runtime reloads it on
-- ab.EVENT.ASSETS_RELOADED.

local W, H = 1920, 1080
local font, badge

-- a GLSL ES 3.00 fragment shader over the whole composed scene (GPU path)
local VIGNETTE = [[#version 300 es
precision mediump float;
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_texture;
uniform vec2 u_resolution;
uniform float u_time;
void main() {
  vec4 c = texture(u_texture, v_uv);
  float d = distance(v_uv, vec2(0.5));
  c.rgb *= 1.0 - 0.45 * smoothstep(0.35, 0.75, d);
  out_color = c;
}]]

function init()
  font = ab.font('assets/roboto-medium.ttf')
  badge = ab.image('assets/badge.png')
  if not ab.effect_set(VIGNETTE) then
    ab.log('no shader backend (cpu reference render); scene is unfiltered')
  end
  ab.log('lua-native starter up')
end

function tick(frame)
  ab.clear(ab.rgb(16, 20, 32))
  ab.draw_game_fit(ab.FIT.CONTAIN, 0.5, 0.0, ab.SAMPLE.NEAREST)

  local t = ab.elapsed_ms() / 1000.0

  -- HUD bar
  ab.fill_rect(0, H - 120, W, 120, ab.rgb(8, 10, 16))

  -- decoded PNG, spinning gently on the GPU-tinted mesh path
  ab.push_transform()
  ab.translate(90, H - 60)
  ab.rotate(math.sin(t) * 0.4)
  ab.draw_texture(badge.texture, -badge.width / 2, -badge.height / 2, badge.width, badge.height)
  ab.pop_transform()

  -- real anti-aliased TrueType text, tinted per call from one white atlas
  ab.draw_text(font, 'Lua bezel: TTF + PNG + shaders, no compiler', 180, H - 45, 44, ab.rgb(235, 238, 245))
  local label = string.format('frame %d   dt %.1f ms', frame, ab.delta_ms())
  local width = ab.measure(font, label, 34)
  ab.draw_text(font, label, W - width - 40, H - 48, 34, ab.rgb(140, 220, 140))

  -- live memory, when the platform exposes it
  local ram = ab.region('system_ram')
  if ram then
    ab.draw_text(font, string.format('ram[0x00] = 0x%02X', ab.read_u8(ram, 0)), 180, H - 85, 30, ab.rgb(255, 176, 32))
  end

  -- start button lights the bar (input surface)
  if ab.input(0, ab.DEVICE.JOYPAD, 0, ab.BTN.START) ~= 0 then
    ab.fill_rect(0, H - 124, W, 4, ab.rgb(255, 176, 32))
  end
end
