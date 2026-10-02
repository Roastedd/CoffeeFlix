// Fullscreen video player and the "Now Playing" audio screen.
#include <algorithm>
#include <cmath>

#include "app/updater.hpp"
#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/youtube_common.hpp"
#include "services/youtube.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

void draw_time_row(float x, float y, float w, double pos, double dur, bool live) {
    const Theme& t = theme();
    if (live) {
        Rect badge(x, y, std::max(58.0f, text::measure(font::caption, tr("LIVE")) + 20), 26);
        gfx::fill_rrect(badge, 6, t.bad);
        text::draw(font::caption, badge.cx(), badge.y + 4, tr("LIVE"), gfx::WHITE, text::CENTER);
        return;
    }
    text::draw(font::small_bold, x, y, util::format_duration(pos), t.text);
    if (dur > 0) text::draw(font::small_bold, x + w, y, "-" + util::format_duration(std::max(0.0, dur - pos)), t.text2, text::RIGHT);
}

// Picker for the audio track, subtitles, stream quality or playback speed, shown as a side panel.
struct TrackMenu {
    enum Kind { AUDIO, SUBTITLES, QUALITY, SPEED };
    bool open = false;
    Kind kind = AUDIO;
    std::vector<player::Track> tracks;
    int current = -1;
    double opened_at = 0;
    int focus_index = 0;  // keeps the focused row in view (YouTube can offer 18 languages)
    float scroll = 0;

    void show(Kind k) {
        kind = k;
        tracks.clear();
        if (k == QUALITY) {
            const player::Source& src = player::source();
            // Auto first, with what it picked: 0 as the value, as in the setting.
            if (src.auto_quality) {
                std::string label = tr("Auto");
                int h = std::min(player::video_width(), player::video_height());  // an upright 1080×1920 is 1080p
                if (src.quality == 0 && h > 0)
                    label += util::fmt(" (%dp%s)", h, player::video_fps() > 31 ? "60" : "");
                tracks.push_back(player::Track{0, label});
            }
            for (auto it = src.qualities.rbegin(); it != src.qualities.rend(); ++it)
                tracks.push_back(player::Track{*it, player::quality_label(*it, src.hfr)});
            current = src.quality;
        } else if (k == SPEED) {  // in hundredths
            for (int v : {50, 75, 100, 125, 150, 175, 200})
                tracks.push_back(player::Track{v, v == 100 ? tr("Normal") : util::fmt("%g\xC3\x97", v / 100.0)});
            current = (int)std::lround(player::speed() * 100);
        } else {
            bool subs = k == SUBTITLES;
            tracks = subs ? player::subtitle_tracks() : player::audio_tracks();
            if (subs) tracks.insert(tracks.begin(), player::Track{-1, tr("Off")});
            current = subs ? player::subtitle_track() : player::audio_track();
        }
        open = true;
        opened_at = ui::time();
        focus_index = 0;
        for (size_t i = 0; i < tracks.size(); i++)
            if (tracks[i].index == current) focus_index = (int)i;
        scroll = -1;  // jump to the current row
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
        const char* title = kind == SUBTITLES ? tr("Subtitles")
                            : kind == QUALITY ? tr("Quality")
                            : kind == SPEED   ? tr("Playback speed")
                                              : tr("Audio");
        text::draw(font::title, x + 36, 60, title, t.text);
        Id g = id("trackmenu");
        const float top = 120, row = 64, view = H - 30 - top;
        float max_scroll = std::max(0.0f, tracks.size() * row - 6 - view);
        float target = scroll < 0 ? focus_index * row - view * 0.5f : scroll;
        float fy = focus_index * row;
        if (fy < target) target = fy;
        if (fy + row > target + view) target = fy + row - view;
        Input& in = input();
        if (in.touching && in.dragging) target -= in.tdy;
        scroll = std::clamp(target, 0.0f, max_scroll);
        float sy = tween(id(g, "scroll"), scroll, 16);
        gfx::push_clip(Rect(x, top - 4, pw, view + 8));
        float y = top - sy;
        for (size_t i = 0; i < tracks.size(); i++) {
            Rect r(x + 24, y, pw - 48, 58);
            Id iid = id(g, (int64_t)i);
            Item it = focusable(iid, r, g, tracks[i].index == current ? F_DEFAULT : 0);
            if (it.focused) focus_index = (int)i;
            gfx::fill_rrect(r, 14, gfx::lerp(Color(255, 255, 255, 0), t.surface_focus, it.f));
            Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
            if (tracks[i].index == current) text::icon(ic::CHECK, 24, r.x + 26, r.cy(), gfx::lerp(t.accent, fg, it.f));
            text::draw_fit(font::body_bold, r.x + 52, r.cy() - 12, r.w - 70, tracks[i].label, fg);
            if (it.clicked) {
                if (kind == SUBTITLES) {
                    player::set_subtitle_track(tracks[i].index);
                } else if (kind == AUDIO) {
                    player::set_audio_track(tracks[i].index);
                } else if (kind == SPEED) {
                    player::set_speed(tracks[i].index / 100.0f);
                } else {
                    // Kept as the default for this service too (the same setting as in Settings).
                    const player::Source& src = player::source();
                    if (!src.quality_setting.empty()) store::set_int(src.quality_setting.c_str(), tracks[i].index);
                    player::set_quality(tracks[i].index);
                }
                open = false;
                reset_focus();
            }
            y += row;
        }
        gfx::pop_clip();
        pop_layer();
        bool tap_outside = in.tap && in.tx < x;  // tapping the video closes the panel
        if (tap_outside) in.tap = false;
        if (input().pressed_(BTN_B) || tap_outside) {
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
        if (comments_open_) {
            if (!comments_.back()) close_comments();
            return true;
        }
        return false;  // app pops us; destructor closes the player
    }

    void frame() override {
        Input& in = input();
        const Theme& t = theme();
        player::State st = player::state();

        // With the comments open, the video "zooms out" to the top left, like YouTube's watch page.
        float ce = tween(id("vp_comments"), comments_open_ ? 1.0f : 0.0f, 9.0f);
        if (ce < 0.002f) ce = 0;
        SDL_Rect fr = player::fit_rect((int)W, (int)H);
        Rect vfull((float)fr.x, (float)fr.y, (float)fr.w, (float)fr.h);
        if (fr.w <= 0 || fr.h <= 0) vfull = Rect(0, 0, W, H);
        Rect area(36, 36, SMALL_W, SMALL_W * 9 / 16);
        float aspect = vfull.w / vfull.h;
        Rect vsmall = aspect > area.w / area.h ? Rect(area.x, area.cy() - area.w / aspect * 0.5f, area.w, area.w / aspect)
                                               : Rect(area.cx() - area.h * aspect * 0.5f, area.y, area.h * aspect, area.h);
        vr_ = Rect(anim::lerp(vfull.x, vsmall.x, ce), anim::lerp(vfull.y, vsmall.y, ce), anim::lerp(vfull.w, vsmall.w, ce),
                   anim::lerp(vfull.h, vsmall.h, ce));

        gfx::fill_rect(Rect(0, 0, W, H), gfx::BLACK);
        if (ce > 0) {
            gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top.alpha(ce), t.bg_bottom.alpha(ce));
            gfx::fill_rrect(Rect(area.x, area.y, area.w, area.h), 14 * ce, Color(0, 0, 0, (uint8_t)(255 * ce)));
        }
        if (SDL_Texture* tex = player::video_texture()) {
            gfx::image_rotated(tex, vr_, player::video_rotation(), gfx::WHITE, 14 * ce);
        } else if (st == player::OPENING || st == player::BUFFERING) {
            // Artwork placeholder while the first frame loads.
            std::string art = player::source().artwork;
            if (const images::Image* img = images::get(art, 0, 0, images::BLUR); img && img->ready)
                gfx::image_cover(img->tex, img->w, img->h, ce > 0 ? area : Rect(0, 0, W, H), 14 * ce, Color(255, 255, 255, 120));
        }

        // Subtitles
        std::string sub = player::subtitle_text();
        if (!sub.empty()) {
            text::Font sf = ce > 0.5f ? font::body_bold : font::title;
            float base = ce > 0.5f ? vr_.b() - 14 : overlay_a_ > 0.5f ? H - 190 : H - 70;
            auto lines = text::wrap(sf, sub, vr_.w * 0.8f, 3);
            float lh = text::line_height(sf) * 1.1f;
            float y = base - lines.size() * lh;
            for (auto& l : lines) {
                float lw = text::measure(sf, l);
                gfx::fill_rrect(Rect(vr_.cx() - lw * 0.5f - 12, y - 2, lw + 24, lh), 8, Color(0, 0, 0, 150));
                text::draw(sf, vr_.cx(), y, l, gfx::WHITE, text::CENTER);
                y += lh;
            }
        }

        bool menu_open = menu_.frame();
        if (!menu_open) handle_input(in, st);
        if (ce > 0) {
            draw_watch_info(st, ce);
            comments_.frame(Rect(W - PANEL_W * ce, 0, PANEL_W, H), ce);
        }

        // The controls hide after a few seconds without input, wherever the focus is. They stay
        // up while paused (unless tapped away), while scrubbing, and on the error/finished card.
        if (hud_ && ui::time() - shown_at_ > HUD_SECONDS) hud_ = false;
        bool force = st == player::FAILED || st == player::ENDED || scrubbing_ || (st == player::PAUSED && !user_hid_);
        float target = force || hud_ ? 1.0f : 0.0f;
        if (menu_open) target = 0.4f;
        if (comments_open_) target = 0;
        hud_up_ = target > 0.5f;
        overlay_a_ = tween(id("vp_overlay"), target, 9.0f);
        draw_overlay(st);
        if (!menu_open) touch_video(in);

        // Seek ripple (YouTube-style) when seeking with the overlay hidden.
        float ra = 1.0f - (float)(ui::time() - ripple_t_) / 0.7f;
        float vs = vr_.w / W;  // the ripple and pulse shrink with the video
        if (ra > 0) {
            float cx = vr_.x + vr_.w * (ripple_dir_ > 0 ? 0.82f : 0.18f), cy = vr_.cy();
            float grow = anim::ease_out_cubic(1 - ra), gr = (220 + grow * 60) * vs;
            gfx::push_clip(vr_);
            gfx::glow(Rect(cx - gr, cy - gr, gr * 2, gr * 2), Color(255, 255, 255, (uint8_t)(50 * ra)));
            gfx::pop_clip();
            gfx::push_alpha(std::min(1.0f, ra * 2));
            text::icon(ripple_dir_ > 0 ? ic::FAST_FORWARD : ic::FAST_REWIND, 56 * std::max(vs, 0.7f), cx, cy - 16, gfx::WHITE);
            text::draw(font::body_bold, cx, cy + 22, util::fmt(tr("%+d seconds"), ripple_amount_), gfx::WHITE, text::CENTER);
            gfx::pop_alpha();
        }

        // Play/pause pulse
        float pa = 1.0f - (float)(ui::time() - pulse_t_) / 0.6f;
        if (pa > 0) {
            float s = (1 + (1 - pa) * 0.5f) * std::max(vs, 0.7f);
            gfx::push_alpha(pa);
            gfx::fill_circle(vr_.cx(), vr_.cy(), 56 * s, Color(0, 0, 0, 110));
            text::icon(pulse_play_ ? ic::PLAY : ic::PAUSE, 64 * s, vr_.cx(), vr_.cy(), gfx::WHITE);
            gfx::pop_alpha();
        }

        if (st == player::OPENING || st == player::BUFFERING) {
            const float r = 34 * std::max(vs, 0.7f);
            spinner(vr_.cx(), vr_.cy(), r, t.accent, 5);
            // A wait of more than a moment (after the picture ran out it can be a long one, for the
            // rest to play through): how far along it is.
            const double at = util::now_seconds();
            if (st != player::BUFFERING) waiting_since_ = -1;
            else if (waiting_since_ < 0) waiting_since_ = at;
            else if (at - waiting_since_ > 2) {
                std::string pct = util::fmt("%d%%", (int)(player::buffering_progress() * 100));
                float y = vr_.cy() + r + 18;
                gfx::fill_rrect(Rect(vr_.cx() - 34, y - 5, 68, 32), 16, Color(0, 0, 0, 120));
                text::draw(font::small_bold, vr_.cx(), y, pct, gfx::WHITE, text::CENTER);
            }
        } else {
            waiting_since_ = -1;
        }
        if (updater::developer()) draw_stats(st, ce);
    }

private:
    static constexpr double HUD_SECONDS = 3.5;
    double waiting_since_ = -1;  // when it began buffering (-1: it isn't)

    // Developers: what the network and the decoder are doing, under the codec badge with the
    // controls (not over a menu) and while it loads. Not translated.
    void draw_stats(player::State st, float ce) {
        const bool loading = st == player::OPENING || st == player::BUFFERING;
        const float a = tween(id("vp_stats"), menu_.open ? 0.0f : loading ? 1.0f : overlay_a_, 9.0f) * (1 - ce);
        player::Stats p;
        if (a <= 0.01f || !player::stats(p)) return;
        const Theme& t = theme();
        struct Line {
            const char* label;
            std::string value;
            bool warn;
        };
        auto kbs = [](float v) { return v > 0 ? util::fmt("%.0f KB/s", v) : std::string("-"); };
        std::vector<Line> lines = {
            {"Download", kbs(p.download) + (p.average > 0 ? util::fmt(" (avg %.0f)", p.average) : ""),
             p.stream > 0 && p.average > 0 && p.average < p.stream},
            {"Stream needs", kbs(p.stream), false},
            {"Video in", util::fmt("%.1f s", p.video_ahead), p.video_ahead < 3},
            {"Sound in", util::fmt("%.1f s", p.audio_ahead), p.audio_ahead < 2},
            {"Downloads", util::fmt("%d at once, %d requests", p.connections, p.requests), false},
            {"Redone", util::fmt("%d quiet, %d failed, %d reconnects", p.gone_quiet, p.failed, p.new_connections),
             p.failed > 0},
            {"Waited", util::fmt("%d times, %.1f s", p.waits, p.waited), p.waits > 0},
            {"Shown", util::fmt("%.0f fps, %d left out, %d dropped, %d late", p.shown_fps, p.left_out, p.dropped, p.late),
             p.dropped + p.late > 5},
            {"Decoding", p.decode_ms > 0 ? util::fmt("%.1f ms a picture", p.decode_ms) : std::string("-"), false},
        };
        const float lh = 22, w = 400, x = W - 64 - w, y0 = 90;
        gfx::push_alpha(a);
        gfx::fill_rrect(Rect(x, y0, w, lines.size() * lh + 20), 12, Color(0, 0, 0, 170));
        float y = y0 + 10;
        for (const Line& l : lines) {
            text::draw(font::caption, x + 14, y + 2, l.label, t.text3);
            text::draw_fit(font::caption, x + 118, y + 2, w - 132, l.value, l.warn ? t.warn : t.text);
            y += lh;
        }
        gfx::pop_alpha();
    }

    void show_overlay() {
        shown_at_ = ui::time();
        hud_ = true;
        user_hid_ = false;
    }

    void hide_overlay() {
        hud_ = false;
        user_hid_ = true;
    }

    // Taps on the video itself (not on a control): a tap shows or hides the controls, and a
    // double tap on the left or right side skips back or ahead 10 seconds, like YouTube's app.
    void touch_video(Input& in) {
        double now = ui::time();
        if (comments_open_) {
            // Tapping the small video brings it back to full screen.
            bool on_video = in.tap && vr_.contains(in.tx, in.ty);
            in.tap = false;
            if (on_video) close_comments();
            return;
        }
        if (in.tap) {
            in.tap = false;
            int side = in.tx < W * 0.35f ? -1 : in.tx > W * 0.65f ? 1 : 0;
            bool can_skip = player::seekable() && !player::live();
            if (side && can_skip && side == tap_side_ && now - tap_t_ < 0.4) {
                seek_by(side * 10);
                pending_tap_ = false;
                tap_t_ = now;  // a third tap adds another 10
                return;
            }
            tap_side_ = side;
            tap_t_ = now;
            // On the sides, wait a moment to see if it's a double tap before toggling.
            if (side && can_skip) pending_tap_ = true;
            else toggle_overlay();
        }
        if (pending_tap_ && now - tap_t_ >= 0.3) {
            pending_tap_ = false;
            if (!in.touching) toggle_overlay();
        }
        // A finger working the controls keeps them up.
        if (in.touching && hud_up_) shown_at_ = now;
    }

    void toggle_overlay() {
        if (hud_up_) hide_overlay();
        else show_overlay();
    }

    void handle_input(Input& in, player::State st) {
        bool stick = std::fabs(in.lx) > 0.3f || std::fabs(in.ly) > 0.3f || std::fabs(in.rx) > 0.3f || std::fabs(in.ry) > 0.3f;
        if (in.pressed || in.pointer_moved || stick) show_overlay();
        if (st == player::FAILED || st == player::ENDED) return;
        bool overlay_visible = overlay_a_ > 0.6f;

        // Quick controls that work regardless of focus.
        if (in.pressed_(BTN_ZR) || in.pressed_(BTN_R)) seek_by(30);
        if (in.pressed_(BTN_ZL) || in.pressed_(BTN_L)) seek_by(-30);
        if (in.pressed_(BTN_Y) && player::started() && !player::subtitle_tracks().empty()) menu_.show(TrackMenu::SUBTITLES);
        if (in.pressed_(BTN_X) && player::started() && player::audio_tracks().size() > 1) menu_.show(TrackMenu::AUDIO);
        if (comments_open_) return;  // the D-pad moves around the comments

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
        // Not while the video is still loading: a stray press then (like the one that opened it)
        // would have it start paused, with nothing on screen saying so.
        if (!player::started()) {
            show_overlay();
            return;
        }
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

    // "1.2M views  ·  19M likes  ·  September 14, 2026" under a YouTube title: what the feed or
    // the stream's resolving had, and the likes and the exact day once they've been asked for.
    std::string watch_facts(const player::Source& src) {
        if (src.service != "youtube") return "";
        if (src.id != facts_id_) {
            facts_id_ = src.id;
            facts_ = {};
            facts_scope_.reset();
            if (!src.live)
                facts_scope_.run<youtube::Details>(
                    [id = src.id] {
                        std::string err;
                        return youtube::details(id, err);
                    },
                    [this](youtube::Details d) { facts_ = std::move(d); });
        }
        std::string s;
        for (const std::string* part : std::initializer_list<const std::string*>{
                 facts_.views.empty() ? &src.views : &facts_.views, &facts_.likes,
                 facts_.posted.empty() ? &src.posted : &facts_.posted}) {
            if (!part->empty()) s += (s.empty() ? "" : "  \xC2\xB7  ") + *part;
        }
        return s;
    }

    void draw_overlay(player::State st) {
        const Theme& t = theme();
        float a = overlay_a_;
        if (a <= 0.01f) return;
        gfx::push_alpha(a);
        float slide = (1 - a) * 30;

        // Top: back (for the touch screen) and title
        gfx::fill_rect_vgrad(Rect(0, 0, W, 200), Color(0, 0, 0, 200), Color(0, 0, 0, 0));
        const player::Source& src = player::source();
        if (icon_button(id(id("vp_top"), "back"), 64, 62 - slide, 24, ic::ARROW_BACK, id("vp_top"))) app::pop();
        text::draw_fit(font::headline, 108, 40 - slide, W - 400, src.title, t.text);
        if (!src.subtitle.empty()) text::draw_fit(font::body, 108, 88 - slide, W - 400, src.subtitle, t.text2);
        const std::string facts = watch_facts(src);
        if (!facts.empty()) text::draw_fit(font::small, 108, (src.subtitle.empty() ? 90 : 120) - slide, W - 400, facts, t.text2);
        std::string codec = player::codec_info(updater::developer());  // the frame rate for the developer
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
        bool can_seek = !live && player::seekable() && dur > 0;

        Input& in = input();
        if (!comments_open_) touch_scrub(Rect(40, by - 46, W - 80, 84), Rect(64, by, W - 128, 0), dur, pos, can_seek && a > 0.6f);

        Item bar{};
        if (can_seek) bar = focusable(bar_id, bar_hit, id("vp_controls"), F_SILENT);
        if (bar.focused) {
            if (in.rep(BTN_LEFT)) { seek_by(-10); in.eat(BTN_LEFT); }
            if (in.rep(BTN_RIGHT)) { seek_by(10); in.eat(BTN_RIGHT); }
            if (bar.clicked) toggle();
        }
        float grow = std::max(bar.f, tween(id("vp_scrub"), scrubbing_ ? 1.0f : 0.0f, 18.0f));
        float prog = dur > 0 ? (float)(pos / dur) : 0;
        float buf = dur > 0 ? (float)(player::buffered_until() / dur) : 0;
        float bh = 6 + 4 * grow;
        if (!live) progress_bar(Rect(64, by - bh * 0.5f, W - 128, bh), prog, buf, true, 1 + grow * 0.5f);
        if (grow > 0.05f && dur > 0) {
            // Time bubble above the knob
            float kx = 64 + (W - 128) * prog;
            std::string ts = util::format_duration(pos);
            float tw = text::measure(font::small_bold, ts) + 20;
            Rect b(std::clamp(kx - tw * 0.5f, 20.0f, W - 20 - tw), by - 52, tw, 30);
            gfx::push_alpha(grow);
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
        if (!live && icon_button(id(g, "back10"), cx - 90, cy, 26, ic::REPLAY_10, g)) seek_by(-10);
        if (icon_button(id(g, "play"), cx, cy, 34, paused || st == player::ENDED ? ic::PLAY : ic::PAUSE, g, F_DEFAULT, true)) toggle();
        if (!live && icon_button(id(g, "fwd10"), cx + 90, cy, 26, ic::FORWARD_10, g)) seek_by(10);
        float rx = W - 90;
        if (player::has_next()) {
            if (icon_button(id(g, "next"), rx, cy, 26, ic::SKIP_NEXT, g)) player::next();
            rx -= 70;
        }
        if (!src.qualities.empty()) {
            if (icon_button(id(g, "quality"), rx, cy, 26, ic::HD, g)) menu_.show(TrackMenu::QUALITY);
            rx -= 70;
        }
        if (!live) {  // lit up while not at normal speed
            if (icon_button(id(g, "speed"), rx, cy, 26, ic::SPEED, g, 0, player::speed() != 1.0f)) menu_.show(TrackMenu::SPEED);
            rx -= 70;
        }
        if (player::started() && !player::subtitle_tracks().empty()) {
            if (icon_button(id(g, "subs"), rx, cy, 26, ic::SUBTITLES, g, 0, player::subtitle_track() >= 0))
                menu_.show(TrackMenu::SUBTITLES);
            rx -= 70;
        }
        if (player::started() && player::audio_tracks().size() > 1) {
            if (icon_button(id(g, "audio"), rx, cy, 26, ic::AUDIOTRACK, g)) menu_.show(TrackMenu::AUDIO);
            rx -= 70;
        }
        // Developers: bicubic video scaling on/off (Settings > Playback too), lit up while on. Not translated.
        if (updater::developer() && platform::is_wiiu()) {
            bool on = store::get_bool("dev_bicubic", false);
            if (icon_button(id(g, "bicubic"), rx, cy, 26, ic::AUTO_AWESOME, g, 0, on)) {
                store::set_bool("dev_bicubic", !on);
                toast(!on ? "Sharper video scaling: bicubic" : "Sharper video scaling off: bilinear", ic::AUTO_AWESOME);
            }
        }

        // YouTube: subscribe to the uploader without leaving the video, the comments, and whether
        // the next video follows by itself (lit up while it does; not in a playlist, which goes on
        // anyway, nor for a live stream).
        if (src.service == "youtube") {
            float lx = 64;
            if (!src.channel_id.empty()) lx += subscribe_button(id(g, "subscribe"), src, lx, cy, g) + 20;
            lx += 26;
            if (!live) {
                if (icon_button(id(g, "comments"), lx, cy, 26, ic::COMMENT, g)) open_comments();
                lx += 70;
            }
            if (!live && !player::has_next()) {
                const bool autoplay = store::get_bool("yt_autoplay", true);
                if (icon_button(id(g, "autoplay"), lx, cy, 26, ic::PLAY_CIRCLE, g, 0, autoplay)) {
                    store::set_bool("yt_autoplay", !autoplay);
                    audio::play(audio::SFX_TOGGLE, 0.6f);
                    toast(std::string(tr("Autoplay")) + ": " + (autoplay ? tr("Off") : tr("On")), ic::PLAY_CIRCLE);
                }
            }
        }

        if (st == player::FAILED) draw_message(ic::ERROR_OUTLINE, tr("Playback failed"), player::error().c_str(), true);
        else if (st == player::ENDED) draw_message(ic::REFRESH, tr("Finished"), "", false);
        gfx::pop_alpha();
    }

    // Subscribe/Subscribed; returns its width. The lists are looked through about once a
    // second, not every frame: signed in, they can hold hundreds of channels.
    float subscribe_button(Id bid, const player::Source& src, float x, float cy, Id g) {
        if (src.channel_id != sub_channel_ || ui::time() - sub_checked_ > 1) {
            sub_channel_ = src.channel_id;
            sub_on_ = yt::subscribed(src.channel_id);
            sub_checked_ = ui::time();
        }
        bool on = sub_on_;
        const char* label = on ? tr("Subscribed") : tr("Subscribe");
        int icon = on ? ic::CHECK : ic::SUBSCRIPTIONS;
        float w = std::max(on ? 190.0f : 170.0f, measure_button(label, icon));
        if (button(bid, Rect(x, cy - 23, w, 46), label, icon, on ? BTN_NORMAL : BTN_PRIMARY, g)) {
            yt::set_subscribed(src.channel_id, src.subtitle, "", !on);
            sub_checked_ = -1;
        }
        return w;
    }

    // Touch scrubbing on a seek bar: put a finger anywhere on it and slide; the video jumps
    // there when it's lifted. Meanwhile `pos` is the time under the finger.
    void touch_scrub(const Rect& touch_area, const Rect& bar, double dur, double& pos, bool enabled) {
        Input& in = input();
        if (enabled && in.touch_began && touch_area.contains(in.tx, in.ty)) scrubbing_ = true;
        if (!scrubbing_) return;
        scrub_to_ = std::clamp((in.tx - bar.x) / bar.w, 0.0f, 1.0f) * dur;
        shown_at_ = ui::time();
        if (!in.touching) {
            scrubbing_ = false;
            in.tap = false;  // not a tap on the bar or the video
            player::seek(scrub_to_);
            audio::play(audio::SFX_MOVE, 0.6f);
        } else {
            pos = scrub_to_;
        }
    }

    void open_comments() {
        comments_open_ = true;
        comments_video_ = player::source().id;
        comments_.show(comments_video_);
        audio::play(audio::SFX_OPEN, 0.6f);
    }

    void close_comments() {
        comments_open_ = false;
        show_overlay();
        set_focus(id(id("vp_controls"), "comments"));
        audio::play(audio::SFX_BACK);
    }

    // Under the small video: title, channel, playback controls and the seek bar.
    void draw_watch_info(player::State st, float ce) {
        const Theme& t = theme();
        const player::Source src = player::source();
        if (comments_open_ && src.id != comments_video_) {
            comments_video_ = src.id;  // the next video started
            comments_.show(src.id);
        }
        gfx::push_alpha(ce);
        float x = 36, w = SMALL_W;
        float y = 36 + SMALL_W * 9 / 16 + 20 + (1 - ce) * 40;
        text::draw_wrapped(font::title, Rect(x, y, w, 80), src.title, t.text, 2);
        y += text::measure_wrapped(font::title, w, src.title, 2) + 6;
        const std::string facts = watch_facts(src);
        if (facts.empty()) {
            y += 8;
        } else {
            text::draw_fit(font::small, x, y, w, facts, t.text2);
            y += text::line_height(font::small) + 8;
        }

        Id g = id("vp_watch");
        float cy = y + 23;
        float nw = std::min(text::measure(font::body_bold, src.subtitle), 250.0f);
        text::draw_fit(font::body_bold, x, cy - text::line_height(font::body_bold) * 0.5f, nw, src.subtitle, t.text2);
        if (src.service == "youtube" && !src.channel_id.empty()) subscribe_button(id(g, "subscribe"), src, x + nw + 18, cy, g);
        bool live = player::live();
        float rx = x + w - 24;
        if (icon_button(id(g, "full"), rx, cy, 23, ic::FULLSCREEN, g)) close_comments();
        rx -= 62;
        if (!live && icon_button(id(g, "fwd10"), rx, cy, 23, ic::FORWARD_10, g)) seek_by(10);
        if (!live) rx -= 62;
        bool paused = st == player::PAUSED;
        if (icon_button(id(g, "play"), rx, cy, 26, paused || st == player::ENDED ? ic::PLAY : ic::PAUSE, g, 0, true)) toggle();
        rx -= 62;
        if (!live && icon_button(id(g, "back10"), rx, cy, 23, ic::REPLAY_10, g)) seek_by(-10);

        // Seek bar
        y += 66;
        double pos = player::position(), dur = player::duration();
        bool can_seek = !live && player::seekable() && dur > 0;
        if (comments_open_) touch_scrub(Rect(x - 10, y - 26, w + 20, 52), Rect(x, y, w, 0), dur, pos, can_seek);
        float grow = tween(id("vp_watch_scrub"), scrubbing_ ? 1.0f : 0.0f, 18.0f);
        float bh = 5 + 4 * grow;
        if (!live) {
            float prog = dur > 0 ? (float)(pos / dur) : 0, buf = dur > 0 ? (float)(player::buffered_until() / dur) : 0;
            progress_bar(Rect(x, y - bh * 0.5f, w, bh), prog, buf, true, 0.8f + grow * 0.6f);
        }
        draw_time_row(x, y + 12, w, pos, dur, live);
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
        const char* again = error ? tr("Retry") : tr("Replay");
        float aw = std::max(170.0f, measure_button(again, ic::REFRESH)), bw = std::max(170.0f, measure_button(tr("Back"), ic::ARROW_BACK));
        float bx = card.cx() - (aw + 20 + bw) * 0.5f;
        if (button(id(g, "retry"), Rect(bx, card.b() - 64, aw, 46), again, ic::REFRESH, BTN_PRIMARY, g, F_DEFAULT)) {
            if (error) player::retry();
            else player::seek(0), player::set_paused(false);
        }
        if (button(id(g, "back"), Rect(bx + aw + 20, card.b() - 64, bw, 46), tr("Back"), ic::ARROW_BACK, BTN_NORMAL, g))
            app::pop();
        pop_layer();
    }

    static constexpr float SMALL_W = 780, PANEL_W = 440;

    TrackMenu menu_;
    yt::CommentsPanel comments_;
    youtube::Details facts_;   // asked for when a video starts
    std::string facts_id_;
    tasks::Scope facts_scope_;
    bool comments_open_ = false;
    std::string comments_video_;
    Rect vr_{0, 0, W, H};  // where the video is drawn
    float overlay_a_ = 1;
    double shown_at_ = 0;
    bool hud_ = true;        // controls wanted (until HUD_SECONDS pass without input)
    bool hud_up_ = true;     // controls showing this frame
    bool user_hid_ = false;  // tapped away, so they stay hidden even while paused
    std::string sub_channel_;  // subscribe_button's last look
    bool sub_on_ = false;
    double sub_checked_ = -1;
    bool scrubbing_ = false;
    double scrub_to_ = 0;
    double tap_t_ = -10;
    int tap_side_ = 0;
    bool pending_tap_ = false;
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
    bool on_back() override { return menu_.open; }

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
            Rect badge(x, y, std::max(64.0f, text::measure(font::caption, tr("LIVE")) + 36), 28);
            gfx::fill_rrect(badge, 7, t.bad);
            gfx::fill_circle(badge.x + 14, badge.cy(), 4 + 1.5f * (0.5f + 0.5f * std::sin((float)ui::time() * 4)), gfx::WHITE);
            text::draw(font::caption, badge.x + 24, badge.y + 5, tr("LIVE"), gfx::WHITE);
            y += 44;
        }
        auto lines = text::wrap(font::display, src.title.empty() ? tr("Unknown") : src.title, w, 2);
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
        if (icon_button(id(g, "play"), cx + 8, cy, 38, paused ? ic::PLAY : ic::PAUSE, g, F_DEFAULT, true) && player::started())
            player::toggle_pause();
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
        cx += 84;
        if (player::seekable()) {  // podcasts, audiobooks
            if (icon_button(id(g, "speed"), cx, cy, 28, ic::SPEED, g, 0, player::speed() != 1.0f))
                menu_.show(TrackMenu::SPEED);
        }
        if (st == player::OPENING || st == player::BUFFERING) spinner(sr.cx(), sr.cy(), 30, gfx::WHITE, 5);
        if (st == player::FAILED) {
            text::icon(ic::ERROR_OUTLINE, 22, x + 11, 600 - 60, t.bad);
            text::draw_fit(font::body_bold, x + 30, 600 - 72, w, player::error(), t.bad);
        }

        hint_bar({{"B", tr("Back")}, {"L", "-15s"}, {"R", "+30s"}});
        Input& in = input();
        if (!menu_.frame()) {  // on top of everything
            if (in.pressed_(BTN_ZR) || in.pressed_(BTN_R)) player::seek_relative(30);
            if (in.pressed_(BTN_ZL) || in.pressed_(BTN_L)) player::seek_relative(-15);
        }
    }

private:
    TrackMenu menu_;
};

}  // namespace

std::unique_ptr<app::Screen> make_video_player() {
    return std::make_unique<VideoPlayerScreen>();
}
void play_video(const player::Source& src) {
    player::open(src);
    app::push(std::make_unique<VideoPlayerScreen>());
    audio::play(audio::SFX_OPEN, 0.7f);
}

void play_video_queue(std::vector<player::Source> queue, int index) {
    player::open_queue(std::move(queue), index);
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
