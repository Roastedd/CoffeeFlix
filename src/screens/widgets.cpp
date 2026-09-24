#include "screens/widgets.hpp"

#include <cmath>

#include "audio/mixer.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/text_input.hpp"

namespace screens {

using namespace ui;

namespace {

Id g_marquee_id = 0;
double g_marquee_start = 0;

struct Prompt {
    bool active = false;
    std::string title;
    std::function<void(std::string)> done;
    double opened = 0;
    bool password = false;
} g_prompt;

float image_height(CardShape shape, float w) {
    switch (shape) {
        case CARD_WIDE: return w * 9.0f / 16.0f;
        case CARD_POSTER: return w * 1.5f;
        default: return w;
    }
}

float radius_for(CardShape shape, const Rect& r) {
    switch (shape) {
        case CARD_CIRCLE: return r.w * 0.5f;
        case CARD_SQUARE: return 16;
        default: return 14;
    }
}

void draw_marquee(text::Font f, float x, float y, float w, const std::string& s, Color c, Id id, bool focused) {
    float tw = text::measure(f, s);
    if (!focused || tw <= w) {
        text::draw_fit(f, x, y, w, s, c);
        return;
    }
    if (g_marquee_id != id) {
        g_marquee_id = id;
        g_marquee_start = ui::time();
    }
    float over = tw - w + 12;
    float speed = 45.0f;
    float travel = over / speed;
    float cycle = 1.2f + travel + 1.2f + travel;
    float t = std::fmod((float)(ui::time() - g_marquee_start), cycle);
    float off;
    if (t < 1.2f) off = 0;
    else if (t < 1.2f + travel) off = (t - 1.2f) * speed;
    else if (t < 2.4f + travel) off = over;
    else off = over - (t - 2.4f - travel) * speed;
    gfx::push_clip(Rect(x, y - 4, w, text::line_height(f) + 8));
    text::draw(f, x - off, y, s, c);
    gfx::pop_clip();
}

}  // namespace

float card_text_height(CardShape shape) { return shape == CARD_CIRCLE ? 46 : 62; }

bool card(Id id, const Rect& ir, CardShape shape, const CardInfo& c, Id group, int flags) {
    const Theme& t = theme();
    Item it = focusable(id, ir, group, flags);
    float s = 1.0f + 0.075f * it.f - 0.05f * it.press;
    float lift = -5.0f * it.f;
    Rect r = ir.scaled(s).offset(bump_x(id), bump_y(id) + lift);
    float rad = radius_for(shape, r);

    focus_glow(r, it.f);
    gfx::shadow(r, 12, Color(0, 0, 0, 80));

    const images::Image* img = c.image.empty() ? nullptr : images::get(c.image, c.image_w, 0);
    if (img && img->ready) {
        gfx::fill_rrect(r, rad, Color(20, 18, 24, 255));
        gfx::image_cover(img->tex, img->w, img->h, r, rad, Color(255, 255, 255, (uint8_t)(255 * images::fade(img))));
    } else if (img && !img->failed) {
        skeleton(r, rad);
    } else {
        uint64_t h = id * 2654435761u;
        float k = (h % 1000) / 1000.0f;
        Color top = c.tint.a ? c.tint : gfx::lerp(t.accent, t.accent2, k).alpha(0.55f);
        gfx::fill_rrect_vgrad(r, rad, top, Color(40, 30, 50, 230));
        text::icon(c.icon ? c.icon : ic::MOVIE, std::min(r.w, r.h) * 0.36f, r.cx(), r.cy(), Color(255, 255, 255, 210));
    }

    // Overlays on the image
    if (c.live) {
        Rect b(r.x + 10, r.y + 10, 52, 24);
        gfx::fill_rrect(b, 6, t.bad);
        text::draw(font::caption, b.cx(), b.y + 4, "LIVE", gfx::WHITE, text::CENTER);
    }
    if (!c.badge.empty()) {
        float bw = text::measure(font::caption, c.badge) + 14;
        Rect b(r.r() - bw - 8, r.b() - 32, bw, 24);
        gfx::fill_rrect(b, 6, Color(0, 0, 0, 185));
        text::draw(font::caption, b.cx(), b.y + 4, c.badge, gfx::WHITE, text::CENTER);
    }
    if (c.favorite) {
        gfx::fill_circle(r.r() - 22, r.y + 22, 15, Color(0, 0, 0, 150));
        text::icon(ic::FAVORITE, 18, r.r() - 22, r.y + 22, t.accent);
    }
    if (c.progress >= 0) {
        Rect pb(r.x + 10, r.b() - 10, r.w - 20, 4);
        gfx::fill_rrect(pb, 2, Color(0, 0, 0, 150));
        gfx::fill_rrect(Rect(pb.x, pb.y, std::max(4.0f, pb.w * std::clamp(c.progress, 0.0f, 1.0f)), 4), 2, t.accent);
    }
    focus_ring(r, rad, it.f);

    // Text below (anchored to the unscaled rect so it doesn't wobble)
    float ty = ir.b() + 12 + (s - 1) * ir.h * 0.5f + lift * 0.5f;
    Color fg = gfx::lerp(t.text2, t.text, 0.5f + 0.5f * it.f);
    if (shape == CARD_CIRCLE) {
        float tw = std::min(text::measure(font::body_bold, c.title), ir.w + 30);
        text::draw_fit(font::body_bold, ir.cx() - tw * 0.5f, ty, ir.w + 30, c.title, fg);
    } else {
        draw_marquee(font::body_bold, ir.x + 2, ty, ir.w - 4, c.title, fg, id, it.focused);
        if (!c.subtitle.empty())
            text::draw_fit(font::small, ir.x + 2, ty + 26, ir.w - 4, c.subtitle, gfx::lerp(t.text3, t.text2, it.f));
    }
    return it.clicked;
}

// --- Page ------------------------------------------------------------------

float Page::begin(Id id, float view_top, float view_h) {
    id_ = id;
    view_top_ = view_top;
    view_h_ = view_h;
    fa_ = next_fa_;
    fb_ = next_fb_;
    has_focus_ = next_has_focus_;
    next_has_focus_ = false;
    float content = std::max(view_h, content_h_ - view_top);
    scroll_drag(id, Rect(0, view_top, ui::W, view_h), true, content, view_h);
    scroll_ = scroll_follow(id, fa_ - view_top, fb_ - view_top, view_h, content, 70, has_focus_);
    return scroll_;
}

void Page::focus_range(float top, float bottom) {
    next_fa_ = top;
    next_fb_ = bottom;
    next_has_focus_ = true;
}

void Page::end(float content_bottom) { content_h_ = content_bottom + 40; }

// --- Shelf -------------------------------------------------------------------

float shelf(Id id, float x, float y, const ShelfSpec& s, Page* page) {
    const Theme& t = theme();
    std::string title = s.title ? s.title : s.title_str;
    float h0 = y;
    if (!title.empty()) {
        text::draw(font::title, x, y, title, t.text);
        y += 46;
    }
    float iw = s.item_w, ih = image_height(s.shape, iw);
    float gap = s.shape == CARD_CIRCLE ? 30 : 22;
    float total_h = ih + card_text_height(s.shape) + 18;

    if (s.loading && s.count == 0) {
        for (int i = 0; i < 6 && x + i * (iw + gap) < ui::W; i++) {
            Rect r(x + i * (iw + gap), y, iw, ih);
            skeleton(r, radius_for(s.shape, r));
            skeleton(Rect(r.x, r.b() + 14, iw * 0.7f, 16), 6);
        }
        return y + total_h - h0;
    }

    Id group = ui::id(id, "items");
    bool active = focus_in_group(group);
    float view_w = ui::W - x - 30;
    float content_w = s.count * (iw + gap);
    // Horizontal scroll follows the focused item (with a leading margin).
    Id sid = ui::id(id, "hscroll");
    float focus_x0 = 0, focus_x1 = 0;
    Id fid = focused();
    for (int i = 0; i < s.count; i++) {
        if (ui::id(group, (int64_t)i) == fid) {
            focus_x0 = i * (iw + gap);
            focus_x1 = focus_x0 + iw;
        }
    }
    scroll_drag(sid, Rect(x, y, view_w, ih), false, content_w, view_w);
    float sx = scroll_follow(sid, focus_x0, focus_x1, view_w, content_w, iw * 0.6f, active);

    for (int i = 0; i < s.count; i++) {
        float rx = x + i * (iw + gap) - sx;
        Rect r(rx, y, iw, ih);
        Id iid = ui::id(group, (int64_t)i);
        bool visible = rx < ui::W + 40 && rx + iw > -40;
        bool clicked;
        bool focused_now;
        if (visible) {
            CardInfo c = s.item(i);
            clicked = card(iid, r, s.shape, c, group);
            focused_now = ui::focused() == iid;
        } else {
            Item it = focusable(iid, r, group);
            clicked = it.clicked;
            focused_now = it.focused;
        }
        if (focused_now) {
            if (page) page->focus_range(h0 + page->scroll(), y + total_h + page->scroll());
            if (s.on_focus) s.on_focus(i);
            if (s.on_x && input().pressed_(BTN_X)) {
                input().eat(BTN_X);
                s.on_x(i);
            }
        }
        if (clicked && s.on_click) s.on_click(i);
    }
    return y + total_h - h0;
}

// --- Grid ---------------------------------------------------------------------------

float grid(Id id, float x, float y, const GridSpec& s, Page* page) {
    float iw = s.item_w, ih = image_height(s.shape, iw);
    float row_h = ih + card_text_height(s.shape) + 20;
    Id group = ui::id(id, "cells");
    if (s.loading && s.count == 0) {
        for (int i = 0; i < s.cols * 2; i++) {
            Rect r(x + (i % s.cols) * (iw + s.gap_x), y + (i / s.cols) * row_h, iw, ih);
            skeleton(r, radius_for(s.shape, r));
            skeleton(Rect(r.x, r.b() + 14, iw * 0.7f, 16), 6);
        }
        return 2 * row_h;
    }
    for (int i = 0; i < s.count; i++) {
        int row = i / s.cols, col = i % s.cols;
        Rect r(x + col * (iw + s.gap_x), y + row * row_h, iw, ih);
        Id iid = ui::id(group, (int64_t)i);
        bool visible = r.y < ui::H + 20 && r.b() + 80 > 0;
        bool clicked, focused_now;
        if (visible) {
            clicked = card(iid, r, s.shape, s.item(i), group, i == 0 ? F_DEFAULT : 0);
            focused_now = ui::focused() == iid;
        } else {
            Item it = focusable(iid, r, group, i == 0 ? F_DEFAULT : 0);
            clicked = it.clicked;
            focused_now = it.focused;
        }
        if (focused_now) {
            if (page) page->focus_range(r.y + page->scroll() - 20, r.y + row_h + page->scroll());
            if (s.on_focus) s.on_focus(i);
            if (s.on_x && input().pressed_(BTN_X)) {
                input().eat(BTN_X);
                s.on_x(i);
            }
            int rows = (s.count + s.cols - 1) / s.cols;
            if (s.on_reach_end && row >= rows - 2) s.on_reach_end();
        }
        if (clicked && s.on_click) s.on_click(i);
    }
    int rows = (s.count + s.cols - 1) / s.cols;
    float h = rows * row_h;
    if (s.loading) {
        loading_indicator(x + (s.cols * (iw + s.gap_x)) * 0.5f, y + h + 30);
        h += 70;
    }
    return h;
}

// --- misc -----------------------------------------------------------------------------

void empty_state(const Rect& r, int icon, const char* title, const char* desc) {
    const Theme& t = theme();
    float cx = r.cx(), y = r.y + 20;
    float bob = std::sin((float)ui::time() * 2.0f) * 4;
    gfx::glow(Rect(cx - 110, y - 50 + bob, 220, 220), t.accent.alpha(0.25f));
    gfx::fill_circle(cx, y + 60 + bob, 52, Color(255, 255, 255, 18));
    text::icon(icon, 54, cx, y + 60 + bob, t.accent);
    text::draw(font::title, cx, y + 132, title, t.text, text::CENTER);
    if (desc && *desc) text::draw_wrapped(font::body, Rect(r.x + 40, y + 174, r.w - 80, 80), desc, t.text2, 3, text::CENTER);
}

bool empty_state_action(Id id, const Rect& r, int icon, const char* title, const char* desc, const char* action,
                        int action_icon) {
    empty_state(r, icon, title, desc);
    float w = measure_button(action, action_icon);
    float y = r.y + 20 + 174 + (desc && *desc ? 64 : 0);
    return button(id, Rect(r.cx() - w * 0.5f, y, w, 50), action, action_icon, BTN_PRIMARY, 0, F_DEFAULT);
}

void section_title(float x, float y, const char* title, const char* subtitle) {
    const Theme& t = theme();
    text::draw(font::display, x, y, title, t.text);
    if (subtitle) text::draw(font::body, x + 2, y + 62, subtitle, t.text2);
}

bool search_bar(Id id, const Rect& r, const std::string& query, const char* placeholder, Id group, int flags) {
    const Theme& t = theme();
    Item it = focusable(id, r, group, flags);
    Rect br = r.scaled(1 + 0.03f * it.f - 0.02f * it.press).offset(bump_x(id), bump_y(id));
    if (it.f > 0.01f) gfx::shadow(br, 18, Color(0, 0, 0, (uint8_t)(110 * it.f)));
    gfx::fill_rrect(br, br.h * 0.5f, gfx::lerp(t.surface_hi, t.surface_focus, it.f));
    Color fg = gfx::lerp(t.text2, gfx::rgb(0x15121A), it.f);
    text::icon(ic::SEARCH, 28, br.x + 34, br.cy(), gfx::lerp(t.accent, gfx::rgb(0x15121A), it.f));
    bool empty = query.empty();
    text::draw_fit(empty ? font::body : font::body_bold, br.x + 62, br.cy() - 12, br.w - 90, empty ? placeholder : query,
                   empty ? fg : gfx::lerp(t.text, gfx::rgb(0x15121A), it.f));
    return it.clicked;
}

void loading_indicator(float cx, float cy, const char* label) {
    const Theme& t = theme();
    spinner(cx, cy, 18, t.accent, 4);
    if (label) text::draw(font::body, cx, cy + 32, label, t.text2, text::CENTER);
}

// --- keyboard prompt --------------------------------------------------------------

void prompt_text(const std::string& title, const std::string& initial, const std::string& hint,
                 std::function<void(std::string)> done, bool password, bool url) {
    platform::TextInputOptions o;
    o.initial = initial;
    o.hint = hint.empty() ? title : hint;
    o.password = password;
    o.url = url;
    o.ok_label = "OK";
    g_prompt.active = true;
    g_prompt.title = title;
    g_prompt.done = std::move(done);
    g_prompt.opened = ui::time();
    g_prompt.password = password;
    platform::text_input_start(o);
}

bool prompt_active() { return g_prompt.active; }

void draw_prompt() {
    if (!g_prompt.active) return;
    platform::TextInputState st = platform::text_input_state();
    if (st == platform::TEXT_DONE) {
        std::string v = platform::text_input_value();
        platform::text_input_reset();
        g_prompt.active = false;
        auto done = std::move(g_prompt.done);
        g_prompt.done = nullptr;
        audio::play(audio::SFX_SELECT);
        if (done) done(util::trim(v));
        return;
    }
    if (st == platform::TEXT_CANCELLED || st == platform::TEXT_IDLE) {
        platform::text_input_reset();
        g_prompt.active = false;
        g_prompt.done = nullptr;
        audio::play(audio::SFX_BACK);
        return;
    }
    const Theme& t = theme();
    float a = anim::ease_out_cubic((float)(ui::time() - g_prompt.opened) / 0.25f);
    gfx::fill_rect(Rect(0, 0, W, H), Color(4, 3, 6, (uint8_t)(200 * a)));
    if (platform::text_input_native()) return;  // the system keyboard draws itself

    Rect card(W * 0.5f - 380, 200 + (1 - a) * 30, 760, 210);
    gfx::shadow(card, 34, Color(0, 0, 0, 180));
    gfx::fill_rrect(card, 26, Color(26, 22, 32, 250));
    text::draw(font::title, card.x + 36, card.y + 28, g_prompt.title, t.text);
    Rect field(card.x + 36, card.y + 80, card.w - 72, 60);
    gfx::fill_rrect(field, 16, Color(255, 255, 255, 22));
    gfx::stroke_rrect(field, 16, 2, t.accent);
    std::string v = platform::text_input_value();
    if (g_prompt.password) v = std::string(v.size(), '*');
    float tw = text::measure(font::label, v);
    float maxw = field.w - 40;
    gfx::push_clip(field.inset(12, 4));
    float tx = field.x + 20 - std::max(0.0f, tw - maxw);
    text::draw(font::label, tx, field.cy() - 13, v, t.text);
    if (std::fmod((float)ui::time(), 1.0f) < 0.55f) gfx::fill_rect(Rect(tx + tw + 3, field.cy() - 14, 2.5f, 28), t.accent);
    gfx::pop_clip();
    text::draw(font::small, card.x + 36, card.b() - 46, "Type on your keyboard \xC2\xB7 Enter to confirm \xC2\xB7 Esc to cancel", t.text3);
}

}  // namespace screens
