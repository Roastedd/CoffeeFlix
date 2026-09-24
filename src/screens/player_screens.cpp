// Fullscreen video player and the "Now Playing" audio screen.
#include <cmath>

#include "audio/mixer.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

void draw_time_row(float x, float y, float w, double pos, double dur, bool live) {
    const Theme& t = theme();
    if (live) {
        Rect badge(x, y, 58, 26);
        gfx::fill_rrect(badge, 6, t.bad);
        text::draw(font::caption, badge.cx(), badge.y + 4, "LIVE", gfx::WHITE, text::CENTER);
        return;
    }
    text::draw(font::small_bold, x, y, util::format_duration(pos), t.text);
    if (dur > 0) text::draw(font::small_bold, x + w, y, "-" + util::format_duration(std::max(0.0, dur - pos)), t.text2, text::RIGHT);
}

// Track picker (audio / subtitles) shown as a side panel.
struct TrackMenu {
    bool open = false;
    bool subtitles = false;
    std::vector<player::Track> tracks;
    int current = -1;
    double opened_at = 0;

    void show(bool subs) {
        subtitles = subs;
        tracks = subs ? player::subtitle_tracks() : player::audio_tracks();
        if (subs) tracks.insert(tracks.begin(), player::Track{-1, "Off"});
        current = subs ? player::subtitle_track() : player::audio_track();
        open = true;
        opened_at = ui::time();
        reset_focus();
        audio::play(audio::SFX_OPEN, 0.6f);
    }

    // Returns true while open (consumes input).
    bool frame() {
        if (!open) return false;
        const Theme& t = theme();
        float a = anim::ease_out_cubic((float)(ui::time() - opened_at) / 0.25f);
        push_layer();
        float pw = 400, x = W - pw * a;
        gfx::fill_rect_hgrad(Rect(x - 80, 0, 80, H), Color(8, 6, 12, 0), Color(8, 6, 12, 230));
        gfx::fill_rect(Rect(x, 0, pw, H), Color(12, 10, 16, 238));
        text::draw(font::title, x + 36, 60, subtitles ? "Subtitles" : "Audio", t.text);
        Id g = id("trackmenu");
        float y = 120;
        for (size_t i = 0; i < tracks.size(); i++) {
            Rect r(x + 24, y, pw - 48, 58);
            Id iid = id(g, (int64_t)i);
            Item it = focusable(iid, r, g, tracks[i].index == current ? F_DEFAULT : 0);
            gfx::fill_rrect(r, 14, gfx::lerp(Color(255, 255, 255, 0), t.surface_focus, it.f));
            Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
            if (tracks[i].index == current) text::icon(ic::CHECK, 24, r.x + 26, r.cy(), gfx::lerp(t.accent, fg, it.f));
            text::draw_fit(font::body_bold, r.x + 52, r.cy() - 12, r.w - 70, tracks[i].label, fg);
            if (it.clicked) {
                if (subtitles) player::set_subtitle_track(tracks[i].index);
                else player::set_audio_track(tracks[i].index);
                open = false;
                reset_focus();
            }
            y += 64;
        }
        pop_layer();
        if (input().pressed_(BTN_B)) {
            input().eat(BTN_B);
            open = false;
            reset_focus();
            audio::play(audio::SFX_BACK);
        }
        return true;
    }
};

// ============================================================================

class VideoPlayerScreen : public app::Screen {
public:
    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    ~VideoPlayerScreen() override { player::close(); }

    void on_enter() override { show_overlay(); }

    bool on_back() override {
        if (menu_.open) return true;
        return false;  // app pops us; destructor closes the player
    }

    void frame() override {
        Input& in = input();
        const Theme& t = theme();
        player::State st = player::state();

        gfx::fill_rect(Rect(0, 0, W, H), gfx::BLACK);
        if (SDL_Texture* tex = player::video_texture()) {
            SDL_Rect fr = player::fit_rect((int)W, (int)H);
            gfx::image(tex, Rect((float)fr.x, (float)fr.y, (float)fr.w, (float)fr.h));
        } else if (st == player::OPENING || st == player::BUFFERING) {
            // Artwork placeholder while the first frame loads.
            std::string art = player::source().artwork;
            if (const images::Image* img = images::get(art, 0, 0, images::BLUR); img && img->ready)
                gfx::image_cover(img->tex, img->w, img->h, Rect(0, 0, W, H), 0, Color(255, 255, 255, 120));
        }

        // Subtitles
        std::string sub = player::subtitle_text();
        if (!sub.empty()) {
            float base = overlay_a_ > 0.5f ? H - 190 : H - 70;
            auto lines = text::wrap(font::title, sub, W * 0.8f, 3);
            float lh = text::line_height(font::title) * 1.1f;
            float y = base - lines.size() * lh;
            for (auto& l : lines) {
                float lw = text::measure(font::title, l);
                gfx::fill_rrect(Rect(W * 0.5f - lw * 0.5f - 12, y - 2, lw + 24, lh), 8, Color(0, 0, 0, 150));
                text::draw(font::title, W * 0.5f, y, l, gfx::WHITE, text::CENTER);
                y += lh;
            }
        }

        bool menu_open = menu_.frame();
        if (!menu_open) handle_input(in, st);

        bool force = st != player::PLAYING || focused_bar_;
        float target = (force || in.idle_time < 3.5 || ui::time() - shown_at_ < 3.5) ? 1.0f : 0.0f;
        if (menu_open) target = 0.4f;
        overlay_a_ = tween(id("vp_overlay"), target, 9.0f);
        draw_overlay(st);

        // Seek ripple (YouTube-style) when seeking with the overlay hidden.
        float ra = 1.0f - (float)(ui::time() - ripple_t_) / 0.7f;
        if (ra > 0) {
            float cx = ripple_dir_ > 0 ? W * 0.82f : W * 0.18f;
            float grow = anim::ease_out_cubic(1 - ra);
            gfx::glow(Rect(cx - 220 - grow * 60, H * 0.5f - 220 - grow * 60, 440 + grow * 120, 440 + grow * 120),
                      Color(255, 255, 255, (uint8_t)(50 * ra)));
            gfx::push_alpha(std::min(1.0f, ra * 2));
            text::icon(ripple_dir_ > 0 ? ic::FAST_FORWARD : ic::FAST_REWIND, 56, cx, H * 0.5f - 16, gfx::WHITE);
            text::draw(font::body_bold, cx, H * 0.5f + 22, util::fmt("%+d seconds", ripple_amount_), gfx::WHITE, text::CENTER);
            gfx::pop_alpha();
        }

        // Play/pause pulse
        float pa = 1.0f - (float)(ui::time() - pulse_t_) / 0.6f;
        if (pa > 0) {
            float s = 1 + (1 - pa) * 0.5f;
            gfx::push_alpha(pa);
            gfx::fill_circle(W * 0.5f, H * 0.5f, 56 * s, Color(0, 0, 0, 110));
            text::icon(pulse_play_ ? ic::PLAY : ic::PAUSE, 64 * s, W * 0.5f, H * 0.5f, gfx::WHITE);
            gfx::pop_alpha();
        }

        if (st == player::OPENING || st == player::BUFFERING) {
            spinner(W * 0.5f, H * 0.5f, 34, t.accent, 5);
        }
    }

private:
    void show_overlay() { shown_at_ = ui::time(); }

    void handle_input(Input& in, player::State st) {
        if (in.any()) show_overlay();
        if (st == player::FAILED || st == player::ENDED) return;
        bool overlay_visible = overlay_a_ > 0.6f;

        // Quick controls that work regardless of focus.
        if (in.pressed_(BTN_ZR) || in.pressed_(BTN_R)) seek_by(30);
        if (in.pressed_(BTN_ZL) || in.pressed_(BTN_L)) seek_by(-30);
        if (in.pressed_(BTN_Y) && !player::subtitle_tracks().empty()) menu_.show(true);
        if (in.pressed_(BTN_X) && player::audio_tracks().size() > 1) menu_.show(false);

        if (!overlay_visible) {
            suspend_nav();
            if (in.pressed_(BTN_A)) {
                in.eat(BTN_A);
                toggle();
            } else if (in.rep(BTN_RIGHT)) {
                seek_by(10);
            } else if (in.rep(BTN_LEFT)) {
                seek_by(-10);
            } else if (in.pressed_(BTN_UP) || in.pressed_(BTN_DOWN)) {
                show_overlay();
            }
        }
    }

    void toggle() {
        player::toggle_pause();
        pulse_t_ = ui::time();
        pulse_play_ = player::state() != player::PAUSED;
    }

    void seek_by(int s) {
        if (!player::seekable()) return;
        player::seek_relative(s);
        if (ui::time() - ripple_t_ < 0.7 && (s > 0) == (ripple_dir_ > 0)) ripple_amount_ += s;
        else ripple_amount_ = s;
        ripple_dir_ = s > 0 ? 1 : -1;
        ripple_t_ = ui::time();
    }

    void draw_overlay(player::State st) {
        const Theme& t = theme();
        float a = overlay_a_;
        if (a <= 0.01f) {
            focused_bar_ = false;
            return;
        }
        gfx::push_alpha(a);
        float slide = (1 - a) * 30;

        // Top: title
        gfx::fill_rect_vgrad(Rect(0, 0, W, 200), Color(0, 0, 0, 200), Color(0, 0, 0, 0));
        const player::Source& src = player::source();
        text::draw_fit(font::headline, 64, 40 - slide, W - 360, src.title, t.text);
        if (!src.subtitle.empty()) text::draw_fit(font::body, 64, 88 - slide, W - 360, src.subtitle, t.text2);
        std::string codec = player::codec_info();
        if (!codec.empty()) {
            float cw = text::measure(font::caption, codec) + 24;
            Rect cr(W - 64 - cw, 50 - slide, cw, 28);
            gfx::fill_rrect(cr, 8, Color(255, 255, 255, 30));
            text::draw(font::caption, cr.cx(), cr.y + 6, codec, t.text2, text::CENTER);
        }

        // Bottom: seek bar and controls
        gfx::fill_rect_vgrad(Rect(0, H - 250, W, 250), Color(0, 0, 0, 0), Color(0, 0, 0, 225));
        float by = H - 150 + slide;
        double pos = player::position(), dur = player::duration();

        Id bar_id = id("vp_bar");
        Rect bar_hit(64, by - 20, W - 128, 40);
        bool live = player::live();
        Item bar{};
        if (!live && player::seekable()) bar = focusable(bar_id, bar_hit, id("vp_controls"), F_SILENT);
        focused_bar_ = bar.focused;
        if (bar.focused) {
            Input& in = input();
            if (in.rep(BTN_LEFT)) { seek_by(-10); in.eat(BTN_LEFT); }
            if (in.rep(BTN_RIGHT)) { seek_by(10); in.eat(BTN_RIGHT); }
            if (bar.clicked) toggle();
        }
        float prog = dur > 0 ? (float)(pos / dur) : 0;
        float buf = dur > 0 ? (float)(player::buffered_until() / dur) : 0;
        float bh = 6 + 4 * bar.f;
        if (!live) progress_bar(Rect(64, by - bh * 0.5f, W - 128, bh), prog, buf, true, 1 + bar.f * 0.5f);
        if (bar.f > 0.05f && dur > 0) {
            // Time bubble above the knob
            float kx = 64 + (W - 128) * prog;
            std::string ts = util::format_duration(pos);
            float tw = text::measure(font::small_bold, ts) + 20;
            Rect b(kx - tw * 0.5f, by - 52, tw, 30);
            gfx::push_alpha(bar.f);
            gfx::fill_rrect(b, 10, gfx::WHITE);
            text::draw(font::small_bold, b.cx(), b.y + 5, ts, gfx::rgb(0x15121A), text::CENTER);
            gfx::pop_alpha();
        }
        draw_time_row(64, by + 16, W - 128, pos, dur, live);

        // Control row
        Id g = id("vp_controls");
        float cy = H - 64 + slide;
        float cx = W * 0.5f;
        bool paused = st == player::PAUSED;
        if (icon_button(id(g, "back10"), cx - 90, cy, 26, ic::REPLAY_10, g) && !live) seek_by(-10);
        if (icon_button(id(g, "play"), cx, cy, 34, paused || st == player::ENDED ? ic::PLAY : ic::PAUSE, g, F_DEFAULT, true)) toggle();
        if (icon_button(id(g, "fwd10"), cx + 90, cy, 26, ic::FORWARD_10, g) && !live) seek_by(10);
        float rx = W - 90;
        if (player::has_next()) {
            if (icon_button(id(g, "next"), rx, cy, 26, ic::SKIP_NEXT, g)) player::next();
            rx -= 70;
        }
        if (!player::subtitle_tracks().empty()) {
            if (icon_button(id(g, "subs"), rx, cy, 26, ic::SUBTITLES, g, 0, player::subtitle_track() >= 0)) menu_.show(true);
            rx -= 70;
        }
        if (player::audio_tracks().size() > 1) {
            if (icon_button(id(g, "audio"), rx, cy, 26, ic::AUDIOTRACK, g)) menu_.show(false);
        }
        hint_bar({{"B", "Back"}}, cy);

        if (st == player::FAILED) draw_message(ic::ERROR_OUTLINE, "Playback failed", player::error().c_str(), true);
        else if (st == player::ENDED) draw_message(ic::REFRESH, "Finished", "", false);
        gfx::pop_alpha();
    }

    void draw_message(int icon, const char* title, const char* detail, bool error) {
        const Theme& t = theme();
        Rect card(W * 0.5f - 260, H * 0.5f - 130, 520, 230);
        gfx::shadow(card, 30, Color(0, 0, 0, 160));
        gfx::fill_rrect(card, 24, Color(24, 20, 30, 240));
        text::icon(icon, 44, card.cx(), card.y + 50, error ? t.bad : t.accent);
        text::draw(font::title, card.cx(), card.y + 82, title, t.text, text::CENTER);
        if (detail && *detail) text::draw_wrapped(font::small, Rect(card.x + 30, card.y + 118, card.w - 60, 40), detail, t.text2, 2, text::CENTER);
        Id g = id("vp_msg");
        push_layer();
        if (button(id(g, "retry"), Rect(card.cx() - 180, card.b() - 64, 170, 46), error ? "Retry" : "Replay", ic::REFRESH,
                   BTN_PRIMARY, g, F_DEFAULT)) {
            if (error) player::retry();
            else player::seek(0), player::set_paused(false);
        }
        if (button(id(g, "back"), Rect(card.cx() + 10, card.b() - 64, 170, 46), "Back", ic::ARROW_BACK, BTN_NORMAL, g))
            app::pop();
        pop_layer();
    }

    TrackMenu menu_;
    float overlay_a_ = 1;
    double shown_at_ = 0;
    bool focused_bar_ = false;
    double ripple_t_ = -10;
    int ripple_dir_ = 1, ripple_amount_ = 0;
    double pulse_t_ = -10;
    bool pulse_play_ = true;
};

// ============================================================================

class NowPlayingScreen : public app::Screen {
public:
    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    void frame() override {
        const Theme& t = theme();
        const player::Source& src = player::source();
        player::State st = player::state();

        std::string art = player::artwork_key();
        if (art.empty()) art = src.artwork;

        // Blurred cover as the whole backdrop.
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        if (const images::Image* bg = art.empty() ? nullptr : images::get(art, 0, 0, images::BLUR); bg && bg->ready) {
            gfx::image_cover(bg->tex, bg->w, bg->h, Rect(-60, -60, W + 120, H + 120), 0,
                             Color(255, 255, 255, (uint8_t)(210 * images::fade(bg, 0.6f))));
        }
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), Color(0, 0, 0, 90), Color(0, 0, 0, 200));

        float lv[16];
        audio::levels(lv);
        float bass = (lv[0] + lv[1] + lv[2]) / 3.0f;
        float beat = spring(id("np_beat"), bass, 400, 18);

        // Artwork
        Rect ar(110, 150, 380, 380);
        float s = 1.0f + 0.025f * beat * (st == player::PLAYING ? 1 : 0);
        Rect sr = ar.scaled(s);
        gfx::glow(sr.inset(-120), t.accent.alpha(0.18f + 0.25f * beat));
        gfx::shadow(sr, 40, Color(0, 0, 0, 200));
        const images::Image* img = art.empty() ? nullptr : images::get(art, 600, 600);
        if (img && img->ready) {
            gfx::image_cover(img->tex, img->w, img->h, sr, 22);
        } else {
            gfx::fill_rrect_vgrad(sr, 22, t.accent, t.accent2);
            text::icon(player::live() ? ic::RADIO : ic::MUSIC, 150, sr.cx(), sr.cy(), Color(255, 255, 255, 200));
        }

        // Text block
        float x = 560, w = W - x - 90;
        float y = 170;
        if (player::live()) {
            Rect badge(x, y, 64, 28);
            gfx::fill_rrect(badge, 7, t.bad);
            gfx::fill_circle(badge.x + 14, badge.cy(), 4 + 1.5f * (0.5f + 0.5f * std::sin((float)ui::time() * 4)), gfx::WHITE);
            text::draw(font::caption, badge.x + 24, badge.y + 5, "LIVE", gfx::WHITE);
            y += 44;
        }
        auto lines = text::wrap(font::display, src.title.empty() ? "Unknown" : src.title, w, 2);
        for (auto& l : lines) {
            text::draw(font::display, x, y, l, t.text);
            y += text::line_height(font::display);
        }
        if (!src.subtitle.empty()) {
            text::draw_fit(font::title, x, y + 6, w, src.subtitle, t.text2);
            y += 44;
        }
        std::string icy = player::stream_title();
        if (!icy.empty()) {
            y += 14;
            text::icon(ic::GRAPHIC_EQ, 22, x + 11, y + 13, t.accent);
            text::draw_fit(font::body_bold, x + 32, y, w - 32, icy, t.text);
            y += 34;
        }

        // Visualizer
        float vx = x, vy = 470, bw = (w - 15 * 8) / 16.0f;
        for (int i = 0; i < 16; i++) {
            float v = spring(id(id("np_bar"), i), lv[i], 500, 22);
            float bh = 6 + std::max(0.0f, v) * 70;
            Color c = gfx::lerp(t.accent, t.accent2, i / 15.0f);
            gfx::fill_rrect(Rect(vx + i * (bw + 8), vy - bh, bw, bh), bw * 0.4f, c.alpha(0.85f));
            gfx::fill_rrect(Rect(vx + i * (bw + 8), vy + 6, bw, bh * 0.35f), bw * 0.4f, c.alpha(0.18f));
        }

        // Progress
        double pos = player::position(), dur = player::duration();
        if (!player::live()) {
            progress_bar(Rect(x, 530, w, 6), dur > 0 ? (float)(pos / dur) : 0, dur > 0 ? (float)(player::buffered_until() / dur) : 0);
            draw_time_row(x, 546, w, pos, dur, false);
        }

        // Controls
        Id g = id("np_controls");
        float cy = 630, cx = x + 36;
        bool paused = st == player::PAUSED;
        if (player::has_previous() || player::seekable()) {
            if (icon_button(id(g, "prev"), cx, cy, 28, ic::SKIP_PREV, g)) player::previous();
            cx += 84;
        }
        if (icon_button(id(g, "play"), cx + 8, cy, 38, paused ? ic::PLAY : ic::PAUSE, g, F_DEFAULT, true)) player::toggle_pause();
        cx += 100;
        if (player::has_next()) {
            if (icon_button(id(g, "next"), cx, cy, 28, ic::SKIP_NEXT, g)) player::next();
            cx += 84;
        }
        if (icon_button(id(g, "stop"), cx, cy, 28, ic::STOP, g)) {
            player::close();
            app::pop();
            return;
        }
        if (st == player::OPENING || st == player::BUFFERING) spinner(sr.cx(), sr.cy(), 30, gfx::WHITE, 5);
        if (st == player::FAILED) {
            text::icon(ic::ERROR_OUTLINE, 22, x + 11, 600 - 60, t.bad);
            text::draw_fit(font::body_bold, x + 30, 600 - 72, w, player::error(), t.bad);
        }

        Input& in = input();
        if (in.pressed_(BTN_ZR) || in.pressed_(BTN_R)) player::seek_relative(30);
        if (in.pressed_(BTN_ZL) || in.pressed_(BTN_L)) player::seek_relative(-15);
        hint_bar({{"B", "Back"}, {"L", "-15s"}, {"R", "+30s"}});
    }
};

}  // namespace

void play_video(const player::Source& src) {
    player::open(src);
    app::push(std::make_unique<VideoPlayerScreen>());
    audio::play(audio::SFX_OPEN, 0.7f);
}

void play_audio(const player::Source& src, bool show) {
    player::open(src);
    if (show) open_now_playing();
}

void play_audio_queue(std::vector<player::Source> queue, int index, bool show) {
    player::open_queue(std::move(queue), index);
    if (show) open_now_playing();
}

bool now_playing_on_top() { return dynamic_cast<NowPlayingScreen*>(app::top()) != nullptr; }

void open_now_playing() {
    if (now_playing_on_top()) return;
    app::push(std::make_unique<NowPlayingScreen>());
    audio::play(audio::SFX_OPEN, 0.7f);
}

}  // namespace screens
