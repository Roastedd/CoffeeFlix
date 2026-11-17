#include "app/volume_keys.hpp"

#include <algorithm>
#include <cmath>

#include "app/sound_settings.hpp"
#include "audio/mixer.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "ui/ui.hpp"

namespace volume_keys {

namespace {
bool g_b_armed = false;   // B is down and may still turn out to be a plain press
bool g_b_used = false;    // ... or was used with Plus or Minus, and isn't
bool g_stick_held = false;
double g_stick_next = 0;
double g_changed = -10;   // when the level last changed, for showing it
bool g_hidden = false;    // a screen shows it itself this frame

int icon_for(float v) { return v <= 0.001f ? ic::VOLUME_OFF : v < 0.5f ? ic::VOLUME_DOWN : ic::VOLUME_UP; }
}  // namespace

void changed() { g_changed = ui::time(); }

void nudge(int steps) {
    const float before = sound_settings::media_volume();
    if (sound_settings::nudge_media_volume(steps) == before) audio::play(audio::SFX_BUMP, 0.5f);  // at an end
    changed();
}

void hide_level() { g_hidden = true; }

void begin_frame(Input& in, bool active) {
    g_hidden = false;
    // B as a modifier: held back while it's down, and handed on as a press when it's let go alone.
    if (active && in.pressed_(BTN_B)) {
        in.eat(BTN_B);
        g_b_armed = true;
        g_b_used = false;
    }
    if (g_b_armed) {
        if (in.down(BTN_B)) {
            in.eat(BTN_B);
            if (in.rep(BTN_PLUS)) { in.eat(BTN_PLUS); nudge(1); g_b_used = true; }
            if (in.rep(BTN_MINUS)) { in.eat(BTN_MINUS); nudge(-1); g_b_used = true; }
        } else {
            g_b_armed = false;
            if (!g_b_used) {  // the press it held back, now
                in.pressed |= bit(BTN_B);
                in.repeat |= bit(BTN_B);
            }
        }
    }
    // The right stick up and down: a step, then steps while it's held
    if (active && std::fabs(in.ry) > 0.6f && std::fabs(in.ry) > std::fabs(in.rx)) {
        const double now = ui::time();
        if (!g_stick_held || now >= g_stick_next) {
            nudge(in.ry > 0 ? 1 : -1);
            g_stick_next = now + (g_stick_held ? 0.09 : 0.4);
            g_stick_held = true;
        }
    } else {
        g_stick_held = false;
    }
}

void draw_level() {
    const float a = std::clamp(1.0f - ((float)(ui::time() - g_changed) - 1.2f) / 0.3f, 0.0f, 1.0f);
    if (a <= 0 || g_hidden) return;
    using ui::W;
    const ui::Theme& t = ui::theme();
    const float v = sound_settings::media_volume();
    const ui::Rect r(W - 64 - 300, 40, 300, 56);
    const ui::Rect bar(r.x + 62, r.cy() - 4, 150, 8);
    gfx::push_alpha(a);
    gfx::fill_rrect(r, 28, gfx::Color(0, 0, 0, 170));
    text::icon(icon_for(v), 26, r.x + 34, r.cy(), gfx::WHITE);
    gfx::fill_rrect(bar, 4, gfx::Color(255, 255, 255, 60));
    const float fill = bar.w * sound_settings::media_slider();
    if (fill > 0) gfx::fill_rrect(ui::Rect(bar.x, bar.y, std::max(bar.h, fill), bar.h), 4, t.accent);
    gfx::fill_rect(ui::Rect(bar.cx() - 1, bar.y - 5, 2, bar.h + 10), gfx::Color(255, 255, 255, 140));  // 100%: as it came
    text::draw(ui::font::small_bold, r.r() - 24, r.cy() - text::line_height(ui::font::small_bold) * 0.5f,
               util::fmt("%d%%", (int)std::lround(v * 100)), gfx::WHITE, text::RIGHT);
    gfx::pop_alpha();
}

}  // namespace volume_keys
