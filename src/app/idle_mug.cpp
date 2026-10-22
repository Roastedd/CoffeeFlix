#include "app/idle_mug.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/personal.hpp"
#include "core/store.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "ui/ui.hpp"

namespace idle_mug {

namespace {
using gfx::Color;
using ui::Rect;

constexpr float FADE_IN = 2.0f, FADE_OUT = 0.3f, MAX_ALPHA = 0.6f;
float g_alpha = 0;
float g_time = 0;

uint8_t a8(float v) { return (uint8_t)std::clamp(v * 255.0f, 0.0f, 255.0f); }

void mug(float cx, float cy, float time, float a) {
    const ui::Theme& t = ui::theme();
    const Color cup = gfx::lerp(t.accent, gfx::WHITE, 0.25f).alpha(a);
    const Color rim = gfx::lerp(t.accent2, gfx::WHITE, 0.45f).alpha(a);
    const Color coffee(70, 42, 28, a8(0.95f * a));
    const float w = 70, h = 58;
    gfx::fill_rrect(Rect(cx - 58, cy + h * 0.5f + 4, 116, 9), 4.5f, Color(255, 255, 255, a8(0.22f * a)));         // the saucer
    gfx::stroke_arc(cx + w * 0.5f + 8, cy + 2, 17, -anim::PI * 0.5f, anim::PI * 0.5f, 7, cup);                    // the handle
    gfx::fill_rrect_vgrad(Rect(cx - w * 0.5f, cy - h * 0.5f, w, h), 16, rim, cup);                                // the cup
    gfx::fill_rrect(Rect(cx - w * 0.5f + 6, cy - h * 0.5f + 5, w - 12, 12), 6, coffee);                           // the coffee
    // Steam: three soft puffs that rise, drift and fade, each on its own beat.
    for (int i = 0; i < 3; i++) {
        const float p = std::fmod(time * 0.18f + i / 3.0f, 1.0f);
        const float x = cx + (i - 1) * 15 + std::sin(p * 5.0f + i * 2.1f) * 7;
        const float y = cy - h * 0.5f - 8 - p * 58;
        const float r = 11 + 12 * p;
        gfx::glow(Rect(x - r, y - r, r * 2, r * 2), Color(255, 240, 225, a8(1.0f * std::sin(p * anim::PI) * a)));
    }
}
}  // namespace

bool enabled() { return store::get_bool("idle_mug", true); }
void set_enabled(bool on) { store::set_bool("idle_mug", on); }

float visibility_after(double idle) {
    if (idle < IDLE_SECONDS) return 0;
    return std::min(1.0f, (float)(idle - IDLE_SECONDS) / FADE_IN) * MAX_ALPHA;
}

void draw(float dt, bool eligible) {
    const double idle = ui::input().idle_time;
    static const double after = getenv("COFFEEFLIX_IDLE_MUG") ? atof(getenv("COFFEEFLIX_IDLE_MUG")) : IDLE_SECONDS;  // seconds, for the screenshot tests
    const bool want = eligible && enabled() && idle >= after;
    if (want) g_alpha = std::min(MAX_ALPHA, g_alpha + dt / FADE_IN * MAX_ALPHA);
    else g_alpha = std::max(0.0f, g_alpha - dt / FADE_OUT * MAX_ALPHA);
    if (g_alpha <= 0.005f) return;
    if (!personal::reduce_motion()) g_time += dt;  // a still mug (steam at one moment) when motion is reduced
    mug(ui::W - 118, ui::H - 168, personal::reduce_motion() ? 0.9f : g_time, g_alpha);
}

}  // namespace idle_mug
