#include "screens/end_card.hpp"

#include <algorithm>
#include <cmath>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/images.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {
using namespace ui;

namespace {
float button_w(const char* label, int icon, float least) { return std::max(least, measure_button(label, icon)); }
}  // namespace

EndAction draw_end_card(const EndCard& c) {
    const Theme& t = theme();
    const Id g = id("end_card");
    const bool episode = c.kind == EndKind::EPISODE;
    EndAction act = EndAction::NONE;
    gfx::fill_rect(Rect(0, 0, W, H), Color(0, 0, 0, 150));
    gfx::fill_rect_vgrad(Rect(0, H * 0.45f, W, H * 0.55f), Color(0, 0, 0, 0), Color(0, 0, 0, 170));
    const bool wide = c.has_next && !c.loading;
    const float cw = wide ? 900 : 640, ch = wide ? 350 : 250;
    const Rect card((W - cw) / 2, wide ? 140 : 190, cw, ch);
    gfx::fill_rrect(card, 24, Color(24, 20, 30, 245));
    if (c.loading) {
        loading_indicator(W / 2, card.y + ch / 2, tr("Loading"));
    } else if (c.has_next) {
        const float lx = card.x + 30, rx = card.x + 420, rw = cw - 450;
        const Rect thumb(lx, card.y + 30, 360, 203);
        const images::Image* img = c.next_image.empty() ? nullptr : images::get(c.next_image, 480, 0);
        gfx::fill_rrect(thumb, 14, Color(255, 255, 255, 18));
        if (img && img->ready) {
            if (img->w >= img->h * 1.5f) gfx::image_cover(img->tex, img->w, img->h, thumb, 14);
            else gfx::image_contain(img->tex, img->w, img->h, Rect(thumb.x, thumb.y + 6, thumb.w, thumb.h - 12));  // a poster
        } else text::icon(ic::MOVIE, 48, thumb.x + thumb.w / 2, thumb.y + thumb.h / 2, t.text3);
        std::string head;
        if (c.counting) {
            const int seconds = std::max(0, (int)std::ceil(END_COUNTDOWN - c.elapsed));
            head = episode ? util::fmt(tr("Next episode in %d seconds"), seconds) : util::fmt(tr("Next video in %d seconds"), seconds);
        } else head = episode ? tr("Next episode") : tr("Next video");
        text::draw_fit(font::small_bold, rx, card.y + 32, rw, head, t.accent);
        float y = card.y + 66;
        y += text::draw_wrapped(font::title, Rect(rx, y, rw, 70), c.next_title, t.text, 2) + 6;
        if (!c.next_subtitle.empty()) text::draw_fit(font::small, rx, y, rw, c.next_subtitle, t.text3);
        y += 34;
        if (!c.next_overview.empty()) text::draw_wrapped(font::small, Rect(rx, y, rw, 66), c.next_overview, t.text2, 3);
        const float by = card.y + 256;
        const char* play = episode ? tr("Play next episode") : tr("Play next video");
        if (button(id(g, "next"), Rect(lx, by, 360, 52), play, ic::SKIP_NEXT, BTN_PRIMARY, g, F_DEFAULT)) act = EndAction::PLAY_NEXT;
        if (c.counting && button(id(g, "cancel"), Rect(rx, by, std::min(rw, button_w(tr("Cancel autoplay"), ic::CLOSE, 240)), 52),
                                 tr("Cancel autoplay"), ic::CLOSE, BTN_NORMAL, g))
            act = EndAction::CANCEL;
        if (c.counting) {
            const Rect bar(lx, card.y + ch - 26, cw - 60, 6);
            gfx::fill_rrect(bar, 3, Color(255, 255, 255, 30));
            const float left = (float)std::clamp(1 - c.elapsed / END_COUNTDOWN, 0.0, 1.0);
            if (left > 0.01f) gfx::fill_rrect_hgrad(Rect(bar.x, bar.y, std::max(6.0f, bar.w * left), bar.h), 3, t.accent, t.accent2);
        }
    } else {
        const float x = card.x + 36, w = cw - 72;
        text::draw_fit(font::small_bold, x, card.y + 34, w, tr("Finished"), t.accent);
        const float y = card.y + 70;
        const float h = text::draw_wrapped(font::title, Rect(x, y, w, 70), c.title, t.text, 2);
        if (!c.subtitle.empty()) text::draw_fit(font::small, x, y + h + 6, w, c.subtitle, t.text3);
        const float by = card.y + ch - 90;
        float bx = x;
        if (button(id(g, "replay"), Rect(bx, by, 240, 52), tr("Replay"), ic::REFRESH, BTN_PRIMARY, g, F_DEFAULT)) act = EndAction::REPLAY;
        bx += 256;
        if (c.extra) {
            const float ew = button_w(c.extra, c.extra_icon, 220);
            if (button(id(g, "extra"), Rect(bx, by, ew, 52), c.extra, c.extra_icon, BTN_NORMAL, g)) act = EndAction::EXTRA;
            bx += ew + 16;
        }
        if (button(id(g, "back"), Rect(bx, by, 200, 52), tr("Back"), ic::ARROW_BACK, BTN_NORMAL, g)) act = EndAction::BACK;
    }
    if (wide || c.loading) {  // the quieter choices, under the card
        const std::string auto_label = std::string(tr("Autoplay")) + ": " + (c.autoplay ? tr("On") : tr("Off"));
        const float w1 = button_w(tr("Replay"), ic::REFRESH, 170), w2 = button_w(auto_label.c_str(), ic::SKIP_NEXT, 250),
                    w3 = c.extra ? button_w(c.extra, c.extra_icon, 200) : 0, w4 = button_w(tr("Back"), ic::ARROW_BACK, 150);
        const int n = c.extra ? 4 : 3;
        float x = (W - (w1 + w2 + w3 + w4 + 16 * (n - 1))) / 2;
        const float y = card.y + ch + 22;
        if (button(id(g, "replay"), Rect(x, y, w1, 48), tr("Replay"), ic::REFRESH, BTN_GHOST, g)) act = EndAction::REPLAY;
        x += w1 + 16;
        if (button(id(g, "autoplay"), Rect(x, y, w2, 48), auto_label.c_str(), ic::SKIP_NEXT, BTN_GHOST, g)) act = EndAction::TOGGLE_AUTOPLAY;
        x += w2 + 16;
        if (c.extra) {
            if (button(id(g, "extra"), Rect(x, y, w3, 48), c.extra, c.extra_icon, BTN_GHOST, g)) act = EndAction::EXTRA;
            x += w3 + 16;
        }
        if (button(id(g, "back"), Rect(x, y, w4, 48), tr("Back"), ic::ARROW_BACK, BTN_GHOST, g)) act = EndAction::BACK;
    }
    hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    return act;
}

}  // namespace screens
