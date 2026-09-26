#include "app/ambient.hpp"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/text.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace ambient {

namespace {

using namespace ui;

// Intro timeline (seconds): the logo pops in, the name follows, then the logo
// flies into its place on the rail while the backdrop fades away.
constexpr float POP = 0.55f, OUT = 1.25f, OUT_LEN = 0.6f;
constexpr float LOGO_R = 64.0f, RAIL_X = 42.0f, RAIL_Y = 58.0f, RAIL_R = 22.0f;

float g_intro_t = 0;
bool g_intro_done = false;
bool g_intro_sound = false;

bool g_saver_on = false;
float g_saver = 0;      // 0..1 visibility
float g_saver_t = 0;    // seconds shown, drives the drift

void swallow(Input& in) {
    in.eat_all();
    in.pointer_moved = false;
    suspend_nav();
}

// The app logo: accent gradient disc with a cup, drawn at its nominal size and
// scaled with a transform so the icon isn't re-rasterized at every size.
void logo(float cx, float cy, float r, float shadow_a) {
    const Theme& t = theme();
    if (r < 0.5f) return;
    gfx::push_transform(r / LOGO_R, cx, cy);
    Rect disc(cx - LOGO_R, cy - LOGO_R, LOGO_R * 2, LOGO_R * 2);
    if (shadow_a > 0.01f) gfx::shadow(disc.offset(0, 10), 34, Color(0, 0, 0, (uint8_t)(160 * shadow_a)));
    gfx::fill_rrect_hgrad(disc, LOGO_R, t.accent, t.accent2);
    text::icon(ic::LOCAL_CAFE, LOGO_R * 1.18f, cx, cy, gfx::rgb(0x1A1016));
    gfx::pop_transform();
}

// Soft steam puffs curling up from the cup.
void steam(float cx, float cy, float r, float time, float a) {
    if (a <= 0.01f) return;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 3; i++) {
            float p = std::fmod(time * 0.5f + i / 3.0f + k * 0.17f, 1.0f);
            float x = cx + (k ? 0.14f : -0.12f) * r + std::sin(p * 5.0f + k * 2.3f) * r * 0.1f;
            float y = cy - r * 1.05f - p * r * 0.8f;
            float rr = r * (0.16f + 0.12f * p);
            float fade = std::sin(p * anim::PI) * a;
            gfx::glow(Rect(x - rr, y - rr, rr * 2, rr * 2), Color(255, 240, 228, (uint8_t)(120 * fade)));
        }
    }
}

void draw_intro() {
    const Theme& t = theme();
    float T = g_intro_t;
    float pop = anim::ease_out_back(T / POP, 2.2f);
    float fly = anim::ease_in_out_cubic((T - OUT) / OUT_LEN);
    float bg = 1 - anim::smoothstep((T - OUT - 0.12f) / (OUT_LEN - 0.12f));

    gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top.alpha(bg), t.bg_bottom.alpha(bg));
    gfx::glow(Rect(W * 0.5f - 420, 290 - 380, 840, 760), t.accent.alpha(0.30f * anim::clamp01(T / POP) * bg));
    gfx::glow(Rect(W * 0.5f - 200, 290 - 140, 700, 560), t.accent2.alpha(0.14f * anim::clamp01(T / POP) * bg));

    float cx = anim::lerp(W * 0.5f, RAIL_X, fly);
    float cy = anim::lerp(290.0f, RAIL_Y, fly);
    float r = anim::lerp(LOGO_R * std::max(0.0f, pop), RAIL_R, fly);

    // Two rings ripple out as the logo lands.
    for (int k = 0; k < 2; k++) {
        float p = (T - 0.18f - k * 0.2f) / 0.9f;
        if (p <= 0 || p >= 1) continue;
        float e = anim::ease_out_cubic(p);
        gfx::stroke_circle(cx, cy, LOGO_R + e * 170, 3.0f * (1 - p) + 1, t.accent.alpha((1 - p) * 0.55f * (1 - fly)));
    }
    steam(cx, cy, r, T, anim::clamp01((T - 0.35f) / 0.4f) * (1 - anim::clamp01(fly * 3)));
    logo(cx, cy, r, 1 - fly);

    // Name and tagline.
    float na = anim::smoothstep((T - 0.28f) / 0.45f) * (1 - anim::clamp01((T - OUT) / 0.22f));
    if (na > 0.01f) {
        text::Font big = text::font(text::BOLD, 60);
        float rise = (1 - anim::ease_out_cubic((T - 0.28f) / 0.5f)) * 26;
        float w1 = text::measure(big, "Coffee"), w2 = text::measure(big, "Flix");
        float x = W * 0.5f - (w1 + w2) * 0.5f, y = 290 + LOGO_R + 34 + rise;
        text::draw(big, x, y, "Coffee", t.text.alpha(na));
        text::draw(big, x + w1, y, "Flix", t.accent.alpha(na));
        float ta = anim::smoothstep((T - 0.5f) / 0.45f) * (1 - anim::clamp01((T - OUT) / 0.22f));
        text::draw(font::label, W * 0.5f, y + 84, tr("Movies \xC2\xB7 Videos \xC2\xB7 Live \xC2\xB7 Radio \xC2\xB7 Podcasts"),
                   t.text2.alpha(ta), text::CENTER);
    }
}

const char* const WEEKDAYS[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"),
                                N_("Thursday"), N_("Friday"), N_("Saturday")};
const char* const MONTHS[] = {N_("January"), N_("February"), N_("March"),     N_("April"),
                              N_("May"),     N_("June"),     N_("July"),      N_("August"),
                              N_("September"), N_("October"), N_("November"), N_("December")};

std::string date_line() {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    // Each language puts the parts in its own order.
    std::string s = tr("{weekday}, {month} {day}");
    s = util::replace_all(s, "{weekday}", tr(WEEKDAYS[lt.tm_wday]));
    s = util::replace_all(s, "{month}", tr(MONTHS[lt.tm_mon]));
    return util::replace_all(s, "{day}", std::to_string(lt.tm_mday));
}

// Slow Lissajous drift so nothing sits still long enough to burn in.
void drift(float w, float h, float& x, float& y) {
    float s = g_saver_t;
    float mx = std::max(0.0f, (W - 120 - w) * 0.5f), my = std::max(0.0f, (H - 100 - h) * 0.5f);
    x = (W - w) * 0.5f + std::sin(s * 0.043f + 0.6f) * mx;
    y = (H - h) * 0.5f + std::sin(s * 0.031f) * my;
}

void draw_saver() {
    const Theme& t = theme();
    float a = anim::smoothstep(g_saver);
    gfx::push_alpha(a);
    gfx::fill_rect(Rect(0, 0, W, H), gfx::rgb(0x050407));

    float s = g_saver_t;
    bool music = player::active() && !player::has_video();
    player::Source src;
    std::string art;
    if (music) {
        src = player::source();
        art = player::artwork_key();
        if (art.empty()) art = src.artwork;
    }

    // Blurred artwork (or accent glows) washing slowly across the screen.
    const images::Image* blur = art.empty() ? nullptr : images::get(art, 0, 0, images::BLUR);
    if (blur && blur->ready) {
        float z = 1.25f + 0.08f * std::sin(s * 0.05f);
        Rect r(-W * (z - 1) * 0.5f + std::sin(s * 0.03f) * 60, -H * (z - 1) * 0.5f, W * z, H * z);
        gfx::image_cover(blur->tex, blur->w, blur->h, r, 0, Color(255, 255, 255, (uint8_t)(90 * images::fade(blur, 1.0f))));
    }
    gfx::glow(Rect(W * 0.5f - 700 + std::sin(s * 0.07f) * 300, -300 + std::cos(s * 0.05f) * 120, 1000, 900), t.accent.alpha(0.10f));
    gfx::glow(Rect(W * 0.5f - 200 + std::cos(s * 0.06f) * 320, H - 560 + std::sin(s * 0.08f) * 100, 1000, 900), t.accent2.alpha(0.08f));

    std::string clock = util::clock_hhmm();
    if (music) {
        // Now-playing card: artwork, title, artist and a live spectrum.
        float cw = 760, ch = 250, x, y;
        drift(cw, ch, x, y);
        Rect ar(x, y, ch, ch);
        const images::Image* img = art.empty() ? nullptr : images::get(art, 500, 500);
        gfx::shadow(ar.offset(0, 12), 40, Color(0, 0, 0, 180));
        if (img && img->ready) gfx::image_cover(img->tex, img->w, img->h, ar, 20);
        else {
            gfx::fill_rrect_vgrad(ar, 20, t.accent, t.accent2);
            text::icon(player::live() ? ic::RADIO : ic::MUSIC, 96, ar.cx(), ar.cy(), gfx::WHITE);
        }
        float tx = ar.r() + 36, tw = cw - ch - 36;
        std::string sub = player::stream_title();
        if (sub.empty()) sub = src.subtitle;
        text::draw(font::label, tx, y + 8, clock, t.text2);
        text::draw_fit(font::display, tx, y + 44, tw, src.title, t.text);
        if (!sub.empty()) text::draw_fit(font::title, tx, y + 104, tw, sub, t.text2);

        float lv[16];
        audio::levels(lv);
        float bw = tw / 16.0f;
        for (int i = 0; i < 16; i++) {
            float h = 6 + lv[i] * 64;
            Color c = gfx::lerp(t.accent, t.accent2, i / 15.0f).alpha(player::state() == player::PLAYING ? 0.9f : 0.35f);
            gfx::fill_rrect(Rect(tx + i * bw, y + ch - h, bw - 6, h), 3, c);
        }
        if (!player::live() && player::duration() > 0) {
            float p = (float)(player::position() / player::duration());
            gfx::fill_rrect(Rect(tx, y + ch + 22, tw, 4), 2, Color(255, 255, 255, 40));
            gfx::fill_rrect_hgrad(Rect(tx, y + ch + 22, tw * p, 4), 2, t.accent, t.accent2);
        }
    } else {
        // Big clock with the date.
        text::Font big = text::font(text::BOLD, 150);
        float cw = std::max(text::measure(big, clock), 420.0f), ch = 250, x, y;
        drift(cw, ch, x, y);
        float cx = x + cw * 0.5f;
        text::draw(big, cx, y, clock, t.text.alpha(0.92f), text::CENTER);
        text::draw(font::headline, cx, y + text::line_height(big) + 2, date_line(), t.text2, text::CENTER);
        float lx = cx - 70;
        logo(lx - 4, y - 36, 16, 0);
        text::draw(text::font(text::SEMIBOLD, 22), lx + 20, y - 50, "CoffeeFlix", t.text3);
    }
    gfx::pop_alpha();
}

}  // namespace

void begin(bool video_foreground) {
    Input& in = input();
    float dt = ui::dt();
    // Scripted desktop runs skip the intro unless they ask for it.
    if (!g_intro_done && platform::scripted() && !getenv("COFFEEFLIX_INTRO")) g_intro_done = true;
    if (!g_intro_done) {
        if (!g_intro_sound) {
            audio::play(audio::SFX_OPEN, 0.7f);
            g_intro_sound = true;
        }
        g_intro_t += dt;
        if (in.any() && g_intro_t > 0.15f && g_intro_t < OUT) g_intro_t = OUT;  // any button skips
        if (g_intro_t >= OUT + OUT_LEN) g_intro_done = true;
        swallow(in);
        return;
    }

    int after = (int)store::get_int("screensaver", 300);
    bool want = after > 0 && in.idle_time > after && !video_foreground && !screens::prompt_active();
    if (g_saver_on && (in.any() || in.idle_time < 0.5)) want = false;  // any input wakes
    if (want && !g_saver_on) g_saver_t = 0;
    g_saver_on = want;
    g_saver = anim::clamp01(g_saver + (want ? dt / 1.6f : -dt / 0.3f));
    if (g_saver_on) g_saver_t += dt;
    // Swallow input while it shows or fades so the wake-up press does nothing else.
    if (g_saver > 0.0f) swallow(in);
}

void draw() {
    if (!g_intro_done) draw_intro();
    else if (g_saver > 0.0f) draw_saver();
}

bool intro_running() { return !g_intro_done; }

}  // namespace ambient
