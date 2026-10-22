// Fullscreen video player and the "Now Playing" audio screen.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>

#include "app/pad_views.hpp"
#include "app/sound_settings.hpp"
#include "app/updater.hpp"
#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/sleep_timer.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/youtube_common.hpp"
#include "services/ryd.hpp"
#include "services/yt_account.hpp"
#include "services/youtube.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

float view_button_width() { return std::max(168.0f, text::measure(font::body_bold, tr("View")) + 100); }

// Touch and controller focus open the same panel as the physical Minus button.
void view_button(Id iid, const Rect& r, Id group, bool enabled = true, bool show_focus = true) {
    if (!pad_views::available()) return;
    const Theme& t = theme();
    Item it{};
    const bool active = enabled && !menu_active() && !prompt_active();
    if (active) it = focusable(iid, r, group);
    const Input& in = input();
    const bool held = active && in.touching && r.contains(in.touch_start_x, in.touch_start_y) && r.contains(in.tx, in.ty);
    const float f = show_focus ? it.f : 0;
    gfx::fill_rrect(r, r.h * 0.5f, gfx::lerp(Color(255, 255, 255, held ? 70 : 34), t.surface_focus, f));
    const Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), f);
    text::icon(ic::GAMEPAD, 24, r.x + 28, r.cy(), fg);
    text::draw_fit(font::body_bold, r.x + 50, r.cy() - text::line_height(font::body_bold) * 0.5f,
                   r.w - 96, tr("View"), fg);
    button_glyph(r.r() - 26, r.cy(), "-", 24);
    if (it.clicked) pad_views::open_drawer();
}

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
    explicit VideoPlayerScreen(std::function<void()> choose_source = {}, std::function<void(const youtube::Video&)> play_related = {})
        : play_related_(std::move(play_related)), choose_source_(std::move(choose_source)) {}
    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }
    // The GamePad remote: the video is the TV's alone, and the controls are the GamePad's.
    bool tv_scene() const override { return true; }

    ~VideoPlayerScreen() override { player::close(); }

    void on_enter() override { show_overlay(); }

    bool on_back() override {
        if (menu_.open) return true;
        if (panel_open_) return true;  // channel_panel() takes B
        if (split_ && pad_tab_ != TAB_NOW) {  // on the GamePad: from the comments or what's next back to what's playing
            if (pad_tab_ == TAB_COMMENTS && comments_.back()) return true;
            pad_tab_ = TAB_NOW;
            return true;
        }
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
        // The GamePad remote: the TV gets the video and the subtitles, nothing else, and the controls are
        // drawn for the GamePad, over a backdrop, always up. Its buttons work as with the controls
        // hidden: A pauses, left and right skip, up and down change the channel.
        split_ = pad_views::pad_scene();
        if (split_) comments_open_ = false;  // the comments need the video's place on the screen

        // With the comments open, the video "zooms out" to the top left, like YouTube's watch page.
        float ce = tween(id("vp_comments"), comments_open_ ? 1.0f : 0.0f, 9.0f);
        if (ce < 0.002f || split_) ce = 0;
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
            float base = ce > 0.5f ? vr_.b() - 14 : overlay_a_ > 0.5f && !split_ ? H - 190 : H - 70;
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

        zap_frame();
        bool menu_open = false;
        if (split_) {
            // The GamePad's picture, from here: its own layout, with the panels opening over it.
            if (pad_views::pad_sleeping()) {  // dark: nothing is drawn for it, and the buttons still work
                handle_input(in, st);
            } else {
                pad_views::PadPass pad(true);
                draw_pad_backdrop();
                pad_covered_ = menu_.open || panel_open_ || menu_active() || prompt_active();
                draw_pad_remote(st);
                menu_open = menu_.frame() || channel_panel();
                // The comments and the list of what's next take the D-pad themselves.
                if (!menu_open && pad_tab_ == TAB_NOW) handle_input(in, st);
                if (updater::developer() && pad_stats_) draw_stats(st, ce);
            }
        } else {
            menu_open = menu_.frame() || channel_panel();
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
            if (menu_open) target = panel_open_ ? 0.0f : 0.4f;  // the channels cover where the title is
            if (comments_open_) target = 0;
            hud_up_ = target > 0.5f;
            overlay_a_ = tween(id("vp_overlay"), target, 9.0f);
            draw_overlay(st);
            draw_banner();
            if (!menu_open) touch_video(in);
        }

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
        if (updater::developer() && !split_) draw_stats(st, ce);
    }

private:
    static constexpr double HUD_SECONDS = 3.5;
    bool split_ = false;  // the GamePad remote is on: the controls aren't on the TV's picture
    double waiting_since_ = -1;  // when it began buffering (-1: it isn't)

    // --- The GamePad remote's own layout -----------------------------------------------------------
    // Not the TV's controls made smaller: the GamePad's screen is small and touched, so it has big
    // buttons, the poster, the title and the seek bar. Buttons here are touch-only (no focus).

    bool pad_covered_ = false;  // a panel is open over the remote: its buttons wait
    bool pad_stats_ = false;    // developers: the network and decoder numbers over the remote (tap the TV icon)
    int skip_pending_ = 0;      // seconds of skips tapped on the remote, not made yet
    double skip_tapped_ = 0;    // when the last one was tapped

    // Skips tapped in a row are added up and made once the taps stop: each one is a seek, which on a
    // slow stream is a wait, and a few taps would be a queue of them.
    void pad_skip(int seconds) {
        skip_pending_ += seconds;
        skip_tapped_ = ui::time();
    }

    // Whether the D-pad (or a stick) was the last thing used here: the selector shows then, and not after a touch.
    bool pad_nav_ = false;

    // A press on `r`: a tap, or A while the D-pad has the selector on it. `held` tells whether a finger is on it now,
    // `focused` whether the selector should be drawn on it.
    bool pad_tap(const Rect& r, bool* held = nullptr, bool* focused = nullptr, int flags = 0) {
        Input& in = input();
        const bool down = !pad_covered_ && in.touching && r.contains(in.touch_start_x, in.touch_start_y) && r.contains(in.tx, in.ty);
        if (held) *held = down;
        if (focused) *focused = false;
        if (pad_covered_) return false;
        const Item it = focusable(id(id("pad_remote"), (int64_t)(r.x * 31 + r.y * 7919 + r.w * 3 + r.h)), r, id("pad_remote"), flags);
        if (focused) *focused = it.focused && pad_nav_;
        return it.clicked;
    }

    // A round button; `big` is the white play button. A little more than it looks is touchable.
    bool pad_circle(float cx, float cy, float rad, int icon, bool big = false, bool lit = false) {
        const Theme& t = theme();
        bool held = false, focused = false;
        const bool hit = pad_tap(Rect(cx - rad - 10, cy - rad - 10, rad * 2 + 20, rad * 2 + 20), &held, &focused, big ? F_DEFAULT : 0);
        const float r = held ? rad * 0.93f : rad;
        if (focused) focus_ring(Rect(cx - rad - 7, cy - rad - 7, rad * 2 + 14, rad * 2 + 14), rad + 7, 1.0f);
        if (big) gfx::fill_circle(cx, cy, r, held ? Color(225, 225, 230, 255) : gfx::WHITE);
        else gfx::fill_circle(cx, cy, r, lit ? t.accent : Color(255, 255, 255, held ? 70 : 34));
        text::icon(icon, rad * (big ? 1.15f : 1.0f), cx, cy, big || lit ? gfx::rgb(0x15121A) : t.text);
        return hit;
    }

    // A pill with an icon and a label; returns its width in `w`.
    bool pad_chip(float x, float y, const char* label, int icon, bool lit, float* w) {
        const Theme& t = theme();
        const bool bare = icon && !*label;  // just the icon: a round button
        const float width = bare ? 76.0f : text::measure(font::body_bold, label) + (icon ? 44.0f : 0.0f) + 44, h = 52;
        const Rect r(x, y, width, h);
        bool held = false, focused = false;
        const bool hit = pad_tap(r, &held, &focused);
        if (focused) focus_ring(Rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8), h * 0.5f + 4, 1.0f);
        if (lit) gfx::fill_rrect_hgrad(r, h * 0.5f, t.accent, t.accent2);
        else gfx::fill_rrect(r, h * 0.5f, Color(255, 255, 255, held ? 70 : 34));
        const Color fg = lit ? gfx::rgb(0x15121A) : t.text;
        float tx = x + 22;
        if (icon) {
            text::icon(icon, 24, bare ? x + width * 0.5f : x + 22 + 12, y + h * 0.5f, fg);
            tx += 36;
        }
        if (!bare) text::draw(font::body_bold, tx, y + h * 0.5f - text::line_height(font::body_bold) * 0.5f, label, fg);
        *w = width;
        return hit;
    }

    // The poster, blurred and dark, behind everything.
    void draw_pad_backdrop() {
        const Theme& t = theme();
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        if (const images::Image* img = images::get(player::source().artwork, 0, 0, images::BLUR); img && img->ready)
            gfx::image_cover(img->tex, img->w, img->h, Rect(0, 0, W, H), 0, Color(255, 255, 255, 120));
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), Color(8, 6, 12, 90), Color(8, 6, 12, 200));
    }

    // --- tabs: what's playing, the comments, what comes next -------------------------------------

    enum PadTab { TAB_NOW, TAB_COMMENTS, TAB_UPNEXT };
    PadTab pad_tab_ = TAB_NOW;
    std::string pad_video_;           // the video the tab state is about
    std::string comments_shown_;      // the video whose comments the tab was opened on
    youtube::Results related_;        // what comes next (YouTube), asked for when the tab is first opened
    std::string related_id_;
    bool related_loading_ = false;
    tasks::Scope related_scope_;
    Page pad_page_;
    std::function<void(const youtube::Video&)> play_related_;

    void load_related(const std::string& id) {
        if (id == related_id_) return;
        related_id_ = id;
        related_ = {};
        related_loading_ = true;
        related_scope_.reset();
        related_scope_.run<youtube::Results>([id] { return youtube::related(id); }, [this, id](youtube::Results r) {
            if (id != related_id_) return;
            related_loading_ = false;
            related_ = std::move(r);
        });
    }

    // Another video, from the list of what comes next.
    void play_other(const youtube::Video& v) {
        if (play_related_) play_related_(v);
        else player::open(youtube::make_source(v));
        pad_tab_ = TAB_NOW;
    }

    // A pill that switches tab.
    void pad_tab_chip(float& x, const char* label, PadTab tab) {
        float w = 0;
        if (pad_chip(x, 22, label, 0, pad_tab_ == tab, &w) && pad_tab_ != tab) {
            pad_tab_ = tab;
            if (tab == TAB_COMMENTS) comments_shown_.clear();  // loads the comments, and focuses the list
        }
        x += w + 10;
    }

    // Along the top: back, the tabs, the GamePad's battery and where the picture is.
    void draw_pad_top(bool comments, bool upnext) {
        const Theme& t = theme();
        gfx::fill_rect_vgrad(Rect(0, 0, W, 104), Color(8, 6, 12, 235), Color(8, 6, 12, 0));
        if (pad_circle(58, 48, 28, ic::ARROW_BACK)) {
            if (pad_tab_ != TAB_NOW) pad_tab_ = TAB_NOW;
            else app::pop();
        }
        float x = 112;
        if (comments || upnext) {
            pad_tab_chip(x, tr("Now playing"), TAB_NOW);
            if (comments) pad_tab_chip(x, tr("Comments"), TAB_COMMENTS);
            if (upnext) pad_tab_chip(x, tr("Up next"), TAB_UPNEXT);
        }
        view_button(id("pad_view_button"), Rect(x + 12, 22, view_button_width(), 52),
                    id("pad_remote"), !pad_covered_, pad_nav_);
        // The GamePad's battery: how full (the console gives 7 steps) or that it is charging.
        float rx = W - 64;
        // Keep the gauge, but omit its words when translated tabs need more room.
        const bool battery_words = x + 12 + view_button_width() <= W - 300;
        const float used = pad_views::draw_battery(rx, 48, 0.9f, battery_words);
        if (used > 0) rx -= used + 26;
        text::icon(ic::TV, 24, rx - 14, 48, pad_stats_ ? t.text : t.accent);
        if (updater::developer() && pad_tap(Rect(rx - 46, 18, 64, 60))) pad_stats_ = !pad_stats_;  // developers: the stats
        if (!comments && !upnext)  // room for words when there are no tabs
            text::draw(font::small_bold, rx - 34, 48 - text::line_height(font::small_bold) * 0.5f, tr("Playing on the TV"), t.text2, text::RIGHT);
    }

    // A strip along the bottom while another tab is open: pause, skip, and how far along it is.
    void draw_pad_mini(player::State st) {
        const Theme& t = theme();
        const player::Source src = player::source();
        const float top = H - 84;
        gfx::fill_rect(Rect(0, top, W, 84), Color(12, 10, 17, 248));
        gfx::fill_rect(Rect(0, top, W, 1), Color(255, 255, 255, 30));
        if (pad_tap(Rect(100, top, W - 460, 84))) pad_tab_ = TAB_NOW;
        const bool paused = st == player::PAUSED, ended = st == player::ENDED;
        if (pad_circle(56, top + 42, 28, paused || ended ? ic::PLAY : ic::PAUSE, true)) {
            if (ended) player::seek(0), player::set_paused(false);
            else toggle();
        }
        if (st == player::FAILED) text::draw_fit(font::body_bold, 108, top + 12, W - 460, tr("Playback failed"), t.bad);
        else text::draw_fit(font::body_bold, 108, top + 12, W - 460, src.title, t.text);
        const double dur = player::duration(), pos = player::position();
        if (!player::live() && dur > 0) {
            progress_bar(Rect(108, top + 54, W - 560, 6), (float)(pos / dur), (float)(player::buffered_until() / dur));
            text::draw(font::caption, W - 440, top + 46, util::format_duration(pos) + " / " + util::format_duration(dur), t.text2);
        }
        if (!player::live() && player::seekable()) {
            if (pad_circle(W - 160, top + 42, 26, ic::REPLAY_10)) pad_skip(-10);
            if (pad_circle(W - 80, top + 42, 26, ic::FORWARD_10)) pad_skip(10);
        }
    }

    void draw_pad_comments(const player::Source& src) {
        if (comments_shown_ != src.id) {
            comments_shown_ = src.id;
            comments_.show(src.id);
        }
        // A column, not the whole width: comments read badly in lines that long.
        comments_.frame(Rect(160, 96, W - 320, H - 96 - 84), 1.0f);
    }

    void draw_pad_upnext(const player::Source& src) {
        const Theme& t = theme();
        load_related(src.id);
        // A search for what to watch next, while this plays on the TV: the keyboard comes up on the GamePad.
        const float bar_y = 100;
        float sx = 77, sw = 0;
        if (pad_chip(sx, bar_y, up_query_.empty() ? tr("Search YouTube") : up_query_.c_str(), ic::SEARCH, !up_query_.empty(), &sw)) {
            prompt_text(tr("Search YouTube"), up_query_, tr("What to watch next"), [this](std::string q) { up_search(q); });
        }
        sx += sw + 14;
        if (!up_query_.empty() && pad_chip(sx, bar_y, tr("Back to suggestions"), ic::CLOSE, false, &sw)) up_search("");
        const float top = 96 + 68, view = H - top - 84;
        pad_page_.begin(id("pad_upnext"), top, view);
        const float y0 = pad_page_.y(top + 14);
        gfx::push_clip(Rect(0, top, W, view));
        float y = y0;
        const youtube::Results& list = up_query_.empty() ? related_ : up_results_;
        const bool loading = up_query_.empty() ? related_loading_ : up_loading_;
        if (!loading && list.items.empty()) {
            const bool failed = !list.error.empty();
            empty_state(Rect(80, top + 30, W - 160, 280), failed ? ic::WIFI_OFF : ic::SEARCH,
                        failed ? tr("Couldn't reach YouTube") : up_query_.empty() ? tr("No videos found") : tr("Nothing matched that"),
                        failed ? list.error.c_str() : "");
        } else {
            GridSpec gs;
            gs.count = (int)list.items.size();
            gs.cols = 4;
            gs.item_w = 262;
            gs.gap_x = 26;
            gs.loading = loading;
            gs.item = [&list](int i) { return yt::video_card(list.items[i]); };
            gs.on_click = [this](int i) { play_other((up_query_.empty() ? related_ : up_results_).items[i]); };
            y += grid(id(id("pad_upnext_grid"), up_query_), 77, y0, gs, &pad_page_);
        }
        gfx::pop_clip();
        pad_page_.end(y + pad_page_.scroll() + 20);
        (void)t;
    }

    void up_search(const std::string& q) {
        up_query_ = q;
        up_results_ = {};
        up_scope_.reset();
        if (q.empty()) return;
        up_loading_ = true;
        up_scope_.run<youtube::Results>([q] { return youtube::search(q); }, [this, q](youtube::Results r) {
            if (q != up_query_) return;
            up_loading_ = false;
            up_results_ = std::move(r);
        });
    }
    std::string up_query_;
    youtube::Results up_results_;
    bool up_loading_ = false;
    tasks::Scope up_scope_;

    // --- What's playing: the poster and title, the seek bar, the buttons -----------------------
    static std::string clock_after(double seconds) {
        const std::time_t at = (std::time_t)(std::time(nullptr) + std::max(0.0, seconds));
        std::tm local{};
        localtime_r(&at, &local);
        char text[16];
        std::strftime(text, sizeof(text), "%H:%M", &local);
        return text;
    }

    void draw_pad_now(player::State st, const player::Source& src) {
        const Theme& t = theme();
        const bool live = player::live();
        const double dur = player::duration();
        double pos = player::position();
        const bool loading = st == player::OPENING || st == player::BUFFERING;
        const bool paused = st == player::PAUSED, ended = st == player::ENDED, failed = st == player::FAILED;
        const bool can_seek = !live && player::seekable() && dur > 0 && !failed;
        const bool youtube = src.service == "youtube";

        // The poster, with the title beside it and, for YouTube, who and what to do about them
        const Rect art(64, 108, 400, 225);
        gfx::fill_rrect(art, 18, Color(0, 0, 0, 120));
        const images::Image* img = src.artwork.empty() ? nullptr : images::get(src.artwork, 480, 270);
        if (img && img->ready)
            gfx::image_cover(img->tex, img->w, img->h, art, 18, Color(255, 255, 255, (uint8_t)(255 * images::fade(img))));
        else text::icon(ic::MOVIE, 64, art.cx(), art.cy(), Color(255, 255, 255, 60));
        if (loading) {
            gfx::fill_rrect(art, 18, Color(0, 0, 0, 120));
            spinner(art.cx(), art.cy(), 28, gfx::WHITE, 5);
        }
        const float tx = art.r() + 36, tw = W - 64 - tx;
        float y = art.y;
        // The buttons sit along the poster's bottom edge; the title, the channel and the views and date line share what
        // is above them (a long title took the room of the line below it, and the buttons were drawn over that line).
        const bool chips = youtube && !store::kids_profile();
        const std::string facts = watch_facts(src);
        const float room = (chips ? art.b() - 52 - 12 : art.b()) - art.y;
        const float rest = (src.subtitle.empty() ? 0.0f : 34.0f) + (facts.empty() ? 0.0f : 28.0f) + 10;
        const float line = text::line_height(font::headline);
        const int title_lines = std::clamp((int)((room - rest) / line), 1, 3);
        y += text::draw_wrapped(font::headline, Rect(tx, y, tw, line * title_lines + 4), src.title, t.text, title_lines) + 10;
        if (!src.subtitle.empty()) {
            text::draw_fit(font::label, tx, y, tw, src.subtitle, t.text2);
            y += 34;
        }
        if (!facts.empty()) text::draw_fit(font::small, tx, y, tw, facts, t.text3);
        if (chips) {  // along the poster's bottom edge
            float x = tx, w = 0;
            if (!src.channel_id.empty()) {
                if (src.channel_id != sub_channel_ || ui::time() - sub_checked_ > 1) {
                    sub_channel_ = src.channel_id;
                    sub_on_ = yt::subscribed(src.channel_id);
                    sub_checked_ = ui::time();
                }
                if (pad_chip(x, art.b() - 52, sub_on_ ? tr("Subscribed") : tr("Subscribe"), sub_on_ ? ic::CHECK : ic::SUBSCRIPTIONS, !sub_on_, &w)) {
                    yt::set_subscribed(src.channel_id, src.subtitle, "", !sub_on_);
                    sub_checked_ = -1;
                }
                x += w + 14;
            }
            if (!live && !player::has_next()) {
                const bool autoplay = store::get_bool("yt_autoplay", true);
                if (pad_chip(x, art.b() - 52, tr("Autoplay"), ic::PLAY_CIRCLE, autoplay, &w)) {
                    store::set_bool("yt_autoplay", !autoplay);
                    toast(std::string(tr("Autoplay")) + ": " + (autoplay ? tr("Off") : tr("On")), ic::PLAY_CIRCLE);
                }
                x += w + 14;
            }
            // Two round buttons, only when both fit beside the others (long translations of the labels before them).
            if (can_rate(src)) {
                // With their counts when there's room beside the others, else just the thumbs, else not at all.
                const std::string lt = like_text(), dt = dislike_text();
                const auto chip_w = [](const std::string& s) { return text::measure(font::body_bold, s) + 88; };
                const bool counts = !lt.empty() && !dt.empty() && x + chip_w(lt) + chip_w(dt) + 14 <= W - 64;
                if (counts || x + 2 * 76 + 14 <= W - 64) {
                    if (pad_chip(x, art.b() - 52, counts ? lt.c_str() : "", ic::THUMB_UP, rating_ == youtube::Rating::LIKE, &w)) rate(src, youtube::Rating::LIKE);
                    x += w + 14;
                    if (pad_chip(x, art.b() - 52, counts ? dt.c_str() : "", ic::THUMB_DOWN, rating_ == youtube::Rating::DISLIKE, &w)) rate(src, youtube::Rating::DISLIKE);
                }
            }
        }

        // The seek bar, scrubbed by touch
        const float by = 388;
        touch_scrub(Rect(40, by - 50, W - 80, 104), Rect(64, by, W - 128, 0), dur, pos, can_seek && !pad_covered_);
        bool bar_focused = false;
        if (can_seek && !pad_covered_) {  // with the selector on the bar, left and right skip 10 seconds each
            const Item bar = focusable(id("pad_seek"), Rect(64, by - 26, W - 128, 52), id("pad_remote"));
            bar_focused = bar.focused && pad_nav_;
            if (bar.focused) {
                Input& in = input();
                if (in.rep(BTN_LEFT)) { in.eat(BTN_LEFT); pad_skip(-10); }
                if (in.rep(BTN_RIGHT)) { in.eat(BTN_RIGHT); pad_skip(10); }
            }
        }
        if (bar_focused) focus_ring(Rect(52, by - 22, W - 104, 44), 22, 1.0f);
        const float grow = tween(id("vp_scrub"), scrubbing_ ? 1.0f : 0.0f, 18.0f);
        const float prog = dur > 0 ? (float)(pos / dur) : 0, buf = dur > 0 ? (float)(player::buffered_until() / dur) : 0;
        const float bh = 10 + 6 * grow;
        if (!live) progress_bar(Rect(64, by - bh * 0.5f, W - 128, bh), prog, buf, true, 1.2f + grow * 0.5f);
        if (grow > 0.05f && dur > 0) {  // the time under the finger
            const std::string ts = util::format_duration(pos);
            const float w = text::measure(font::body_bold, ts) + 28;
            const Rect b(std::clamp(64 + (W - 128) * prog - w * 0.5f, 20.0f, W - 20 - w), by - 60, w, 38);
            gfx::push_alpha(grow);
            gfx::fill_rrect(b, 12, gfx::WHITE);
            text::draw(font::body_bold, b.cx(), b.y + 7, ts, gfx::rgb(0x15121A), text::CENTER);
            gfx::pop_alpha();
        }
        draw_time_row(64, by + 20, W - 128, pos, dur, live);
        if (skip_pending_ != 0) {
            text::draw(font::body_bold, W * 0.5f, by + 18, util::fmt(tr("%+d seconds"), skip_pending_), t.accent, text::CENTER);
        } else if (!live && dur > 0 && !ended && !failed) {  // when it'll be over, at the speed it plays
            const double left = (dur - pos) / std::max(0.25f, player::speed());
            text::draw(font::small, W * 0.5f, by + 20, util::fmt(tr("Ends at %s"), clock_after(left).c_str()), t.text3,
                       text::CENTER);
        }

        // Transport: play and pause, and skips back and ahead
        const float cx = W * 0.5f, cy = 488;
        if (pad_circle(cx, cy, 54, paused || ended ? ic::PLAY : ic::PAUSE, true)) {
            if (ended) player::seek(0), player::set_paused(false);
            else toggle();
        }
        if (!live && player::seekable()) {
            if (pad_circle(cx - 170, cy, 38, ic::REPLAY_10)) pad_skip(-10);
            if (pad_circle(cx + 170, cy, 38, ic::FORWARD_10)) pad_skip(10);
            if (pad_circle(cx - 300, cy, 32, ic::FAST_REWIND)) pad_skip(-30);
            if (pad_circle(cx + 300, cy, 32, ic::FAST_FORWARD)) pad_skip(30);
            text::draw(font::caption, cx - 300, cy + 40, "30", t.text3, text::CENTER);
            text::draw(font::caption, cx + 300, cy + 40, "30", t.text3, text::CENTER);
        }
        if (player::has_previous() && pad_circle(cx - 430, cy, 32, ic::SKIP_PREV)) player::previous();
        if (player::has_next() && pad_circle(cx + 430, cy, 32, ic::SKIP_NEXT)) player::next();

        // Two rows of pills: the picture and sound's choices, then where the sound goes and the screen
        float x = 64, w = 0;
        float row = 568;
        if (player::started() && (player::audio_tracks().size() > 1 || (src.on_track_choice && !player::audio_tracks().empty()))) {
            if (pad_chip(x, row, tr("Audio"), ic::AUDIOTRACK, false, &w)) menu_.show(TrackMenu::AUDIO);
            x += w + 14;
        }
        if (player::started() && !player::subtitle_tracks().empty()) {
            if (pad_chip(x, row, tr("Subtitles"), ic::SUBTITLES, player::subtitle_track() >= 0, &w)) menu_.show(TrackMenu::SUBTITLES);
            x += w + 14;
        }
        if (!src.qualities.empty()) {
            if (pad_chip(x, row, tr("Quality"), ic::HD, false, &w)) menu_.show(TrackMenu::QUALITY);
            x += w + 14;
        }
        if (!live) {
            if (pad_chip(x, row, tr("Playback speed"), ic::SPEED, player::speed() != 1.0f, &w)) menu_.show(TrackMenu::SPEED);
            x += w + 14;
        }
        if (pad_chip(x, row, sleep_timer_label().c_str(), ic::SCHEDULE, sleep_timer::active(), &w)) show_sleep_timer_menu();
        x += w + 14;
        if (lineup()) {  // TV channels: the list, and one up or down
            float lx = W - 64 - 28;
            if (pad_circle(lx, row + 26, 28, ic::LIST)) open_panel();
            lx -= 70;
            if (pad_circle(lx, row + 26, 28, ic::EXPAND_LESS)) zap(1);
            lx -= 70;
            if (pad_circle(lx, row + 26, 28, ic::EXPAND_MORE)) zap(-1);
        }
        x = 64;
        row = 636;
        {   // where the sound goes: the TV, or the GamePad's speakers and headphones too
            const int rs = pad_views::remote_sound();
            const char* label = rs == 1 ? tr("Sound: GamePad only") : rs == 2 ? tr("Sound: TV and GamePad") : tr("Sound: TV only");
            if (pad_chip(x, row, label, rs == 0 ? ic::VOLUME_UP : ic::HEADPHONES, rs != 0, &w)) pad_views::cycle_remote_sound();
            x += w + 14;
        }
        if (pad_chip(x, row, tr("Screen off"), ic::VISIBILITY_OFF, false, &w)) pad_views::sleep_pad();
        x += w + 14;
        if (choose_source_) {
            if (pad_chip(x, row, tr("Choose source"), ic::LINK, false, &w)) choose_source_();
            x += w + 14;
        }
        {   // the picture's sound level, at the right of the same row: a finger drags it, left and right on the D-pad move it
            const float sw = 300, sx = W - 64 - sw;
            const Rect track(sx, row + 12, sw, 28);
            float vol = sound_settings::media_volume();
            const Id sid = id("pad_volume");
            if (slider(sid, track, &vol, 0.05f, id("pad_remote"), 0, false)) sound_settings::set_media_volume(vol);
            const bool on = focused() == sid && pad_nav_;
            if (on) focus_ring(Rect(sx - 54, row - 2, sw + 130, 56), 28, 1.0f);
            text::icon(vol <= 0.001f ? ic::VOLUME_OFF : ic::VOLUME_UP, 28, sx - 28, row + 26, vol <= 0.001f ? theme().text3 : theme().text2);
            text::draw(font::caption, sx + sw + 14, row + 26 - text::line_height(font::caption) * 0.5f, util::fmt("%d%%", (int)std::lround(vol * 100)), theme().text2);
        }
    }

    void draw_pad_remote(player::State st) {
        {   // the selector shows after the D-pad or a stick, and goes after a touch
            const Input& in = input();
            const uint32_t dpad = bit(BTN_UP) | bit(BTN_DOWN) | bit(BTN_LEFT) | bit(BTN_RIGHT) | bit(BTN_A);
            const bool stick = std::fabs(in.lx) > 0.5f || std::fabs(in.ly) > 0.5f;
            if (in.pressed & dpad || stick) pad_nav_ = true;
            else if (in.touch_began || in.tap) pad_nav_ = false;
        }
        const player::Source src = player::source();
        const bool live = player::live();
        const bool yt_video = src.service == "youtube" && !src.id.empty();
        const bool kids = store::kids_profile();  // no comments or suggestions for a kids profile
        const bool comments = yt_video && !live && !kids, upnext = yt_video && !kids;
        if (src.id != pad_video_) {  // another video: back to what's playing
            pad_video_ = src.id;
            pad_tab_ = TAB_NOW;
            up_search("");
        }
        if ((pad_tab_ == TAB_COMMENTS && !comments) || (pad_tab_ == TAB_UPNEXT && !upnext)) pad_tab_ = TAB_NOW;
        if (skip_pending_ != 0 && ui::time() - skip_tapped_ > 0.6) {
            seek_by(skip_pending_);
            skip_pending_ = 0;
        }

        // The tab first, then the bar and strip that stay over it
        switch (pad_tab_) {
        case TAB_NOW: draw_pad_now(st, src); break;
        case TAB_COMMENTS: draw_pad_comments(src); break;
        case TAB_UPNEXT: draw_pad_upnext(src); break;
        }
        draw_pad_top(comments, upnext);
        if (pad_tab_ != TAB_NOW) draw_pad_mini(st);

        if (pad_tab_ == TAB_NOW) {
            if (st == player::FAILED) draw_message(ic::ERROR_OUTLINE, tr("Playback failed"), player::error().c_str(), true);
            else if (st == player::ENDED) draw_message(ic::REFRESH, tr("Finished"), "", false);
        }
    }

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
        if (screens::menu_active() || screens::prompt_active()) return;  // a panel (the GamePad's, say) has the D-pad
        bool stick = std::fabs(in.lx) > 0.3f || std::fabs(in.ly) > 0.3f || std::fabs(in.rx) > 0.3f || std::fabs(in.ry) > 0.3f;
        if (in.pressed || in.pointer_moved || stick) show_overlay();
        if (st == player::FAILED || st == player::ENDED) return;
        bool overlay_visible = overlay_a_ > 0.6f && !split_;  // on the GamePad remote, as with them hidden

        // Quick controls that work regardless of focus. On a TV channel, L goes back to the last one.
        const bool channels = lineup() && on_air();
        if (in.pressed_(BTN_ZR) || in.pressed_(BTN_R)) seek_by(30);
        if (in.pressed_(BTN_ZL) || in.pressed_(BTN_L)) {
            if (channels) last_channel();
            else seek_by(-30);
        }
        if (in.pressed_(BTN_Y) && player::started() && !player::subtitle_tracks().empty()) menu_.show(TrackMenu::SUBTITLES);
        if (in.pressed_(BTN_X) && player::started() && (player::audio_tracks().size() > 1 || (player::source().on_track_choice && !player::audio_tracks().empty()))) menu_.show(TrackMenu::AUDIO);
        if (comments_open_) return;  // the D-pad moves around the comments

        // On the GamePad remote the D-pad moves the selector over the buttons, as on any screen (a dark GamePad has
        // no selector to move: the D-pad skips, as before).
        if (!overlay_visible && (!split_ || pad_views::pad_sleeping())) {
            suspend_nav();
            if (in.pressed_(BTN_A)) {
                in.eat(BTN_A);
                if (pending_ >= 0) tune(pending_);
                else if (channels) open_panel();
                else toggle();
            } else if (in.rep(BTN_RIGHT)) {
                seek_by(10);
            } else if (in.rep(BTN_LEFT)) {
                seek_by(-10);
            } else if (lineup() && (in.rep(BTN_UP) || in.rep(BTN_DOWN))) {
                // Up and down change the channel, like a TV remote's.
                zap(in.rep(BTN_UP) ? 1 : -1);
                hud_ = false;
            } else if (in.pressed_(BTN_UP) || in.pressed_(BTN_DOWN)) {
                show_overlay();
            }
        }
    }

    // Live channels belong to the full app; here a video is never one of a list.
    player::Lineup* lineup() const { return nullptr; }
    int channel() const { return -1; }
    bool on_air() const { return true; }
    void zap(int) {}
    void zap_frame() {}
    void tune(int) {}
    void last_channel() {}
    void draw_banner() {}
    void open_panel() {}
    bool channel_panel() { return false; }

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

    // Like and dislike buttons: for a YouTube video while signed in (and not turned off in Settings).
    bool can_rate(const player::Source& src) const {
        return src.service == "youtube" && !src.live && !store::kids_profile() && yt_account::signed_in() &&
               store::get_bool("yt_rate_buttons", true);
    }

    // The counts on the buttons: what YouTube and Return YouTube Dislike said, moved by what was pressed here.
    std::string like_text() const {
        int64_t n = facts_.like_count >= 0 ? facts_.like_count : like_ryd_;
        return n < 0 ? "" : util::format_count(std::max<int64_t>(0, n + like_delta_));
    }
    std::string dislike_text() const {
        return dislike_ryd_ < 0 ? "" : "~" + util::format_count(std::max<int64_t>(0, dislike_ryd_ + dislike_delta_));
    }

    // Pressing the one already set takes the rating back.
    void rate(const player::Source& src, youtube::Rating wanted) {
        if (rating_busy_) return;
        const youtube::Rating target = wanted == rating_ ? youtube::Rating::NONE : wanted;
        const youtube::Rating before = rating_;
        const int dl = like_delta_, dd = dislike_delta_;
        // At once, so the press shows: the button lights and the count moves; both go back if YouTube says no.
        rating_ = target;
        like_delta_ += (target == youtube::Rating::LIKE) - (before == youtube::Rating::LIKE);
        dislike_delta_ += (target == youtube::Rating::DISLIKE) - (before == youtube::Rating::DISLIKE);
        rating_busy_ = true;
        rate_scope_.run<std::string>(
            [id = src.id, target] {
                std::string err;
                return youtube::account_rate(id, target, err) ? std::string() : (err.empty() ? std::string(tr("YouTube didn't accept that")) : err);
            },
            [this, target, before, dl, dd](std::string err) {
                rating_busy_ = false;
                if (!err.empty()) {
                    rating_ = before;
                    like_delta_ = dl;
                    dislike_delta_ = dd;
                    toast(util::fmt(tr("Couldn't change your rating: %s"), err.c_str()), ic::ERROR_OUTLINE);
                    return;
                }
                toast(target == youtube::Rating::LIKE ? tr("Liked") : target == youtube::Rating::DISLIKE ? tr("Disliked") : tr("Rating removed"),
                      target == youtube::Rating::DISLIKE ? ic::THUMB_DOWN : ic::THUMB_UP);
            });
    }

    // "1.2M views  ·  19M likes  ·  September 14, 2026" under a YouTube title: what the feed or
    // the stream's resolving had, and the likes and the exact day once they've been asked for.
    std::string watch_facts(const player::Source& src) {
        if (src.service != "youtube") return "";
        if (src.id != facts_id_) {
            facts_id_ = src.id;
            facts_ = {};
            facts_dislikes_.clear();
            like_ryd_ = dislike_ryd_ = -1;
            like_delta_ = dislike_delta_ = 0;
            facts_scope_.reset();
            ryd_scope_.reset();
            rating_ = youtube::my_rating(src.id);
            if (!src.live && !store::kids_profile()) {
                facts_scope_.run<youtube::Details>(
                    [id = src.id] {
                        std::string err;
                        return youtube::details(id, err);
                    },
                    [this](youtube::Details d) { facts_ = std::move(d); });
                if (ryd::enabled() && !store::kids_profile())
                    ryd_scope_.run<ryd::Votes>(
                        [id = src.id] {
                            std::string err;
                            return ryd::fetch(id, err);
                        },
                        [this](ryd::Votes v) {
                            if (v.ok && v.dislikes > 0) {
                                facts_dislikes_ = util::fmt(tr("~%s dislikes"), util::format_count(v.dislikes).c_str());
                                dislike_ryd_ = v.dislikes;
                                like_ryd_ = v.likes;
                            }
                        });
            }
        }
        std::string s;
        for (const std::string* part : std::initializer_list<const std::string*>{
                 facts_.views.empty() ? &src.views : &facts_.views, &facts_.likes, &facts_dislikes_,
                 facts_.posted.empty() ? &src.posted : &facts_.posted}) {
            // With the like and dislike buttons showing, the counts are on them.
            if (can_rate(src) && (part == &facts_.likes || part == &facts_dislikes_)) continue;
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
        if (can_rate(src)) {  // like and dislike under the title, with their counts: the lit one is yours
            Id tg = id("vp_top");
            const float cy0 = (src.subtitle.empty() ? 118 : 148) - slide;
            float cx0 = 108;
            struct Chip { youtube::Rating r; int icon; const char* key; std::string text; };
            const Chip chips[] = {{youtube::Rating::LIKE, ic::THUMB_UP, "like", like_text()},
                                  {youtube::Rating::DISLIKE, ic::THUMB_DOWN, "dislike", dislike_text()}};
            for (const Chip& c : chips) {
                const bool on = rating_ == c.r;
                const float bw = std::max(c.text.empty() ? 64.0f : 0.0f, measure_button(c.text.c_str(), c.icon));
                gfx::push_alpha(rating_busy_ ? 0.6f : 1.0f);  // while YouTube is being asked
                if (button(id(tg, c.key), Rect(cx0, cy0, bw, 44), c.text.c_str(), c.icon, on ? BTN_PRIMARY : BTN_NORMAL, tg)) rate(src, c.r);
                gfx::pop_alpha();
                cx0 += bw + 12;
            }
        }
        player::Lineup* channels = lineup();
        std::string codec = player::codec_info(updater::developer());  // the frame rate for the developer
        if (!codec.empty()) {
            float cw = text::measure(font::caption, codec) + 24;
            Rect cr(W - 64 - cw, 50 - slide, cw, 28);
            gfx::fill_rrect(cr, 8, Color(255, 255, 255, 30));
            text::draw(font::caption, cr.cx(), cr.y + 6, codec, t.text2, text::CENTER);
        }
        view_button(id("vp_view_button"), Rect(W - 64 - view_button_width(), 98 - slide, view_button_width(), 52),
                    id("vp_top"), !menu_.open && !panel_open_);

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
        if (choose_source_) {
            if (button(id(g, "source"), Rect(90, cy - 22, 260, 44), tr("Choose source"), ic::LINK, BTN_NORMAL, g)) choose_source_();
        }
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
        if (icon_button(id(g, "sleep"), rx, cy, 26, ic::SCHEDULE, g, 0, sleep_timer::active())) show_sleep_timer_menu();  // lit up while it runs
        rx -= 70;
        if (player::started() && !player::subtitle_tracks().empty()) {
            if (icon_button(id(g, "subs"), rx, cy, 26, ic::SUBTITLES, g, 0, player::subtitle_track() >= 0))
                menu_.show(TrackMenu::SUBTITLES);
            rx -= 70;
        }
        if (player::started() && (player::audio_tracks().size() > 1 || (src.on_track_choice && !player::audio_tracks().empty()))) {
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

        // TV channels: the list, the one before and after, and back to live from a recording.
        if (channels) {
            float lx = 90;
            if (icon_button(id(g, "channels"), lx, cy, 26, ic::LIST, g)) open_panel();
            lx += 70;
            if (icon_button(id(g, "ch_down"), lx, cy, 26, ic::EXPAND_MORE, g)) zap(-1);
            lx += 70;
            if (icon_button(id(g, "ch_up"), lx, cy, 26, ic::EXPAND_LESS, g)) zap(1);
            lx += 70;
            if (!on_air() && icon_button(id(g, "golive"), lx, cy, 26, ic::LIVE_TV, g)) tune(channel());
        }

        // YouTube: subscribe to the uploader without leaving the video, the comments, and whether
        // the next video follows by itself (lit up while it does; not in a playlist, which goes on
        // anyway, nor for a live stream).
        if (src.service == "youtube") {
            const bool kids = store::kids_profile();  // a kids profile: no subscribing, comments or autoplay
            float lx = 64;
            if (!kids && !src.channel_id.empty()) lx += subscribe_button(id(g, "subscribe"), src, lx, cy, g) + 20;
            lx += 26;
            if (!live && !kids) {
                if (icon_button(id(g, "comments"), lx, cy, 26, ic::COMMENT, g)) open_comments();
                lx += 70;
            }
            if (!live && !kids && !player::has_next()) {
                const bool autoplay = store::get_bool("yt_autoplay", true);
                if (icon_button(id(g, "autoplay"), lx, cy, 26, ic::PLAY_CIRCLE, g, 0, autoplay)) {
                    store::set_bool("yt_autoplay", !autoplay);
                    audio::play(autoplay ? audio::SFX_TOGGLE_OFF : audio::SFX_TOGGLE, 0.6f);  // (it is now the other way)
                    toast(std::string(tr("Autoplay")) + ": " + (autoplay ? tr("Off") : tr("On")), ic::PLAY_CIRCLE);
                }
                lx += 70;
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

    std::function<void()> choose_source_;
    static constexpr float SMALL_W = 780, PANEL_W = 440;

    TrackMenu menu_;
    int pending_ = -1;        // the channel about to be tuned (-1: none)
    bool panel_open_ = false;
    yt::CommentsPanel comments_;
    youtube::Details facts_;   // asked for when a video starts
    std::string facts_id_, facts_dislikes_;
    int64_t like_ryd_ = -1, dislike_ryd_ = -1;  // Return YouTube Dislike's counts, -1 until known
    int like_delta_ = 0, dislike_delta_ = 0;    // pressed here since the video began
    tasks::Scope facts_scope_, ryd_scope_, rate_scope_;
    youtube::Rating rating_ = youtube::Rating::NONE;  // what this console last set
    bool rating_busy_ = false;
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
    bool tv_scene() const override { return true; }
    bool draws_background() const override { return true; }
    bool on_back() override { return menu_.open; }

    void frame() override {
        const Theme& t = theme();
        const player::Source& src = player::source();
        player::State st = player::state();

        std::string art = player::artwork_key();
        if (art.empty()) art = src.artwork;
        float lv[16], bars[16];
        audio::levels(lv);
        const float beat = spring(id("np_beat"), (lv[0] + lv[1] + lv[2]) / 3.0f, 400, 18);
        for (int i = 0; i < 16; ++i) bars[i] = spring(id(id("np_bar"), i), lv[i], 500, 22);
        const Rect ar(110, 150, 380, 380);
        const Rect sr = ar.scaled(1.0f + 0.025f * beat * (st == player::PLAYING ? 1 : 0));
        const float x = 560, w = W - x - 90;
        // Draw the same artwork/track information on both screens, but process controls
        // only once, on the GamePad in a split view. Animation state also advances once.
        auto draw_body = [&] {

            // Blurred cover as the whole backdrop.
            gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
            if (const images::Image* bg = art.empty() ? nullptr : images::get(art, 0, 0, images::BLUR); bg && bg->ready) {
                gfx::image_cover(bg->tex, bg->w, bg->h, Rect(-60, -60, W + 120, H + 120), 0,
                                 Color(255, 255, 255, (uint8_t)(210 * images::fade(bg, 0.6f))));
            }
            gfx::fill_rect_vgrad(Rect(0, 0, W, H), Color(0, 0, 0, 90), Color(0, 0, 0, 200));


            // Artwork
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
                float v = bars[i];
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

        };
        draw_body();
        const bool split = pad_views::pad_scene();
        pad_views::PadPass pad(split);
        if (split) draw_body();
        view_button(id("np_view_button"), Rect(W - 64 - view_button_width(), 28, view_button_width(), 52),
                    id("np_controls"), !menu_.open);

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
            cx += 84;
        }
        if (icon_button(id(g, "sleep"), cx, cy, 28, ic::SCHEDULE, g, 0, sleep_timer::active())) show_sleep_timer_menu();
        if (split) {
            const int sound = pad_views::remote_sound();
            const char* label = sound == 1 ? tr("Sound: GamePad only") : sound == 2 ? tr("Sound: TV and GamePad") : tr("Sound: TV only");
            if (button(id(g, "sound"), Rect(110, 606, 380, 48), label, ic::VOLUME_UP, BTN_NORMAL, g))
                pad_views::cycle_remote_sound();
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

std::unique_ptr<app::Screen> make_video_player(std::function<void()> choose_source,
                                               std::function<void(const youtube::Video&)> play_related) {
    return std::make_unique<VideoPlayerScreen>(std::move(choose_source), std::move(play_related));
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
