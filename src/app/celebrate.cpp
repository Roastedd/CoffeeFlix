#include "app/celebrate.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "app/personal.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "ui/ui.hpp"

namespace celebrate {

namespace {

using gfx::Color;
using ui::Rect;

constexpr float FADE_IN = 0.8f, FADE_OUT = 1.4f;
constexpr int BALLOONS = 14, GLINTS = 36, BURSTS = 4, SPARKS = 12;

struct State {
    bool on = false, preview = false;
    birthday::Style style = birthday::STYLE_BALLOONS;
    std::string ribbon;
    float t = 0;
    uint64_t seed = 1;
};
State& st() {
    static State s;
    return s;
}

float frac(float v) { return v - std::floor(v); }
// A fixed pseudo-random number in 0..1 for a particle and a purpose, so the picture is the same each frame.
float rnd(uint64_t seed, int i, int k) {
    uint64_t h = seed ^ (uint64_t)(i * 7919 + k * 104729 + 12345);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return (float)(h & 0xFFFFFF) / (float)0x1000000;
}

uint8_t a8(float v) { return (uint8_t)std::clamp(v * 255.0f, 0.0f, 255.0f); }

void balloons(float t, float env, uint64_t seed) {
    static const uint32_t PALETTE[] = {0xFF6B8A, 0xFFB347, 0xFFE066, 0x6BCB77, 0x4D9DE0, 0xB388EB};
    for (int i = 0; i < BALLOONS; i++) {
        const float delay = rnd(seed, i, 0) * 2.2f;
        const float w = 50 + rnd(seed, i, 1) * 26, h = w * 1.22f;
        const float speed = 78 + rnd(seed, i, 2) * 52;  // pixels a second
        const float x0 = 90 + rnd(seed, i, 3) * (ui::W - 180);
        const float age = t - delay;
        if (age <= 0) continue;
        const float y = ui::H + h + 60 - age * speed;
        if (y < -h - 80) continue;
        const float x = x0 + std::sin(age * (0.9f + rnd(seed, i, 4) * 0.6f) + rnd(seed, i, 5) * 6) * 16;
        const uint32_t c = PALETTE[i % 6];
        const Color body((c >> 16) & 255, (c >> 8) & 255, c & 255, a8(0.86f * env));
        const Color dim(255, 255, 255, a8(0.32f * env));
        gfx::fill_rect(Rect(x - 1, y + h * 0.5f, 2, 70), dim);                                  // the string
        gfx::fill_triangle(x - 6, y + h * 0.5f + 9, x + 6, y + h * 0.5f + 9, x, y + h * 0.5f - 2, body);  // the knot
        gfx::fill_rrect(Rect(x - w * 0.5f, y - h * 0.5f, w, h), w * 0.5f, body);
        gfx::glow(Rect(x - w * 0.32f, y - h * 0.36f, w * 0.34f, w * 0.34f), Color(255, 255, 255, a8(0.5f * env)));  // a shine
    }
}

void sparklers(float t, float env, uint64_t seed) {
    for (int i = 0; i < GLINTS; i++) {
        const float period = 1.3f + rnd(seed, i, 0) * 1.1f, phase = rnd(seed, i, 1);
        const float k = twinkle(t, period, phase);  // 0..1, smooth
        if (k < 0.02f) continue;
        const float x = 60 + rnd(seed, i, 2) * (ui::W - 120);
        const float y = 70 + rnd(seed, i, 3) * (ui::H - 140) - t * (6 + rnd(seed, i, 4) * 8);
        const float s = 10 + rnd(seed, i, 5) * 16;
        const bool warm = rnd(seed, i, 6) > 0.4f;
        const Color c = warm ? Color(255, 226, 150, a8(0.9f * k * env)) : Color(255, 255, 255, a8(0.85f * k * env));
        gfx::glow(Rect(x - s * 1.4f, y - s * 1.4f, s * 2.8f, s * 2.8f), Color(c.r, c.g, c.b, a8(0.35f * k * env)));
        gfx::fill_rrect(Rect(x - s, y - 1.1f, s * 2, 2.2f), 1.1f, c);   // a four-point star: two thin crossed bars
        gfx::fill_rrect(Rect(x - 1.1f, y - s, 2.2f, s * 2), 1.1f, c);
    }
}

void fireworks(float t, float env, uint64_t seed) {
    static const uint32_t PALETTE[] = {0xFFD166, 0xEF476F, 0x06D6A0, 0x74C0FC, 0xC77DFF};
    for (int b = 0; b < BURSTS; b++) {
        const float start = 0.5f + b * 1.25f;
        const float age = t - start;
        if (age < 0 || age > 2.4f) continue;
        const float cx = 180 + rnd(seed, b, 0) * (ui::W - 360), cy = 120 + rnd(seed, b, 1) * (ui::H * 0.42f);
        const uint32_t c = PALETTE[(b + (int)(rnd(seed, b, 2) * 5)) % 5];
        const float grow = 1.0f - std::pow(1.0f - std::min(1.0f, age / 1.6f), 3.0f);  // quick at first, then easing
        const float life = std::clamp(1.0f - age / 2.4f, 0.0f, 1.0f);
        const float reach = 90 + rnd(seed, b, 3) * 70;
        // A soft swell where it bursts: low, and over a third of a second, not a flash.
        const float swell = std::sin(std::min(1.0f, age / 0.4f) * anim::PI) * 0.16f;
        gfx::glow(Rect(cx - 120, cy - 120, 240, 240), Color((c >> 16) & 255, (c >> 8) & 255, c & 255, a8(swell * env)));
        for (int i = 0; i < SPARKS; i++) {
            const float ang = (float)i / SPARKS * 2 * anim::PI + rnd(seed, b, 4) * 6;
            const float r = reach * grow * (0.85f + 0.3f * rnd(seed, b * 31 + i, 5));
            const float x = cx + std::cos(ang) * r, y = cy + std::sin(ang) * r + 40 * age * age;  // a little fall
            const float s = 5.5f - 2.5f * grow;
            gfx::fill_circle(x, y, s, Color((c >> 16) & 255, (c >> 8) & 255, c & 255, a8(0.9f * life * env)));
            gfx::glow(Rect(x - s * 2.4f, y - s * 2.4f, s * 4.8f, s * 4.8f), Color(255, 255, 255, a8(0.25f * life * env)));
        }
    }
}

// Reduce motion: a ribbon with the greeting, fading in and out, nothing moving.
void ribbon(const std::string& text, float env) {
    if (text.empty()) return;
    const ui::Theme& th = ui::theme();
    // In the strip along the top, between the logo and the clock: over nothing that is being read.
    const float w = std::min(700.0f, text::measure(ui::font::label, text) + 80);
    const Rect r(130 + (800 - w) * 0.5f, 12, w, 38);
    gfx::fill_rrect(r, 19, Color(10, 8, 14, a8(0.78f * env)));
    gfx::stroke_rrect(r, 19, 1.5f, th.accent.alpha(env));
    text::draw_fit(ui::font::label, r.cx(), r.cy() - text::line_height(ui::font::label) * 0.5f, r.w - 48, text, Color(255, 255, 255, a8(env)), text::CENTER);
}

}  // namespace

float envelope(float t) {
    if (t <= 0 || t >= DURATION) return 0;
    if (t < FADE_IN) return anim::smoothstep(t / FADE_IN);
    if (t > DURATION - FADE_OUT) return anim::smoothstep((DURATION - t) / FADE_OUT);
    return 1.0f;
}

// A slow swell up and down: a sine's square, so it starts and ends at zero without a corner.
float twinkle(float t, float period, float phase) {
    const float s = std::sin(frac(t / period + phase) * anim::PI);
    return s * s;
}

void start(birthday::Style style, const std::string& ribbon_text, bool preview) {
    State& s = st();
    s.on = true;
    s.preview = preview;
    s.style = style;
    s.ribbon = ribbon_text;
    s.t = 0;
    s.seed = (uint64_t)(ui::time() * 1e6) | 1;
}

void stop() { st().on = false; }
bool active() { return st().on; }
bool preview() { return st().on && st().preview; }

void draw(float dt) {
    State& s = st();
    if (!s.on) return;
    s.t += dt;
    if (s.t >= DURATION) {
        s.on = false;
        return;
    }
    const float env = envelope(s.t);
    if (personal::reduce_motion() || s.style == birthday::STYLE_NONE) {  // nothing moves: the greeting in a ribbon
        ribbon(s.ribbon, env);
        return;
    }
    switch (s.style) {
        case birthday::STYLE_BALLOONS: balloons(s.t, env, s.seed); break;
        case birthday::STYLE_SPARKLERS: sparklers(s.t, env, s.seed); break;
        case birthday::STYLE_FIREWORKS: fireworks(s.t, env, s.seed); break;
        default: break;
    }
}

}  // namespace celebrate
