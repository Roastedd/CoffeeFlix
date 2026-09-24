#include "app/mini_player.hpp"

#include "app/app.hpp"
#include "gfx/images.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "ui/ui.hpp"

namespace mini_player {

using namespace ui;

void draw() {
    bool show = player::active() && player::audio_only() && !screens::now_playing_on_top();
    float a = tween(id("mini_a"), show ? 1.0f : 0.0f, 10.0f);
    if (a <= 0.01f) return;
    const Theme& t = theme();
    const player::Source& src = player::source();

    if (show && input().pressed_(BTN_PLUS)) {
        input().eat(BTN_PLUS);
        screens::open_now_playing();
        return;
    }

    Rect r(W - 400, H - 96 + (1 - a) * 40, 360, 68);
    gfx::push_alpha(a);
    Item it = show ? focusable(id("mini_player"), r, id("mini_group"), F_NO_MEMORY) : Item{};
    Rect br = r.scaled(1 + 0.04f * it.f);
    gfx::shadow(br, 22, Color(0, 0, 0, 170));
    gfx::fill_rrect(br, 18, gfx::lerp(Color(30, 26, 36, 245), t.surface_focus, it.f));
    Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
    Color fg2 = gfx::lerp(t.text2, Color(21, 18, 26, 170), it.f);

    std::string art = player::artwork_key();
    if (art.empty()) art = src.artwork;
    Rect ar(br.x + 10, br.y + 10, br.h - 20, br.h - 20);
    const images::Image* img = art.empty() ? nullptr : images::get(art, 120, 120);
    if (img && img->ready) gfx::image_cover(img->tex, img->w, img->h, ar, 10);
    else {
        gfx::fill_rrect_vgrad(ar, 10, t.accent, t.accent2);
        text::icon(player::live() ? ic::RADIO : ic::MUSIC, 26, ar.cx(), ar.cy(), gfx::WHITE);
    }
    float tx = ar.r() + 14, tw = br.r() - tx - 56;
    std::string sub = player::stream_title();
    if (sub.empty()) sub = src.subtitle;
    text::draw_fit(font::body_bold, tx, br.y + (sub.empty() ? 22 : 12), tw, src.title, fg);
    if (!sub.empty()) text::draw_fit(font::small, tx, br.y + 37, tw, sub, fg2);

    player::State st = player::state();
    float cx = br.r() - 32, cy = br.cy();
    if (st == player::OPENING || st == player::BUFFERING) spinner(cx, cy, 14, t.accent, 3);
    else text::icon(st == player::PAUSED ? ic::PLAY : ic::PAUSE, 30, cx, cy, fg);
    if (!player::live() && player::duration() > 0) {
        float p = (float)(player::position() / player::duration());
        gfx::fill_rect(Rect(br.x + 18, br.b() - 4, (br.w - 36) * p, 2.5f), t.accent);
    }
    gfx::pop_alpha();
    if (it.clicked) screens::open_now_playing();
    if (it.focused && input().pressed_(BTN_X)) {
        input().eat(BTN_X);
        player::toggle_pause();
    }
}

void update_hidden() {}
void shutdown() {}

}  // namespace mini_player
