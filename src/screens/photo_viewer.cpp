// Fullscreen photo viewer: paging, zoom and pan, turning a picture (kept for next time), the details of a photo, and a slideshow
// that moves over each picture by where its people are (gfx/photo_motion), with music if wanted.
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <random>

#include "app/personal.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "gfx/jpeg_tools.hpp"
#include "gfx/photo_motion.hpp"
#include "gfx/thumb_cache.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/surprise.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using gfx::Color;

// The size a picture is decoded to for viewing (the UI's own, a 1080p TV is shown the 720p layout enlarged), and the larger
// one fetched when zooming in. The software renderer of the screenshot tests can't draw full-screen pictures of this size.
int fit_w() {
    static const int w = getenv("COFFEEFLIX_PHOTO_MAX") ? atoi(getenv("COFFEEFLIX_PHOTO_MAX")) : 1280;
    return w;
}
int fit_h() { return fit_w() * 9 / 16; }
int hi_w() { return fit_w() * 2; }
int hi_h() { return hi_w() * 9 / 16; }

constexpr float ZOOM_MAX = 8.0f;
constexpr double CROSS_SLIDE = 1.1;   // seconds a slideshow picture takes to fade into the next
constexpr double CROSS_PAGE = 0.35;   // paging by hand

// --- what the slideshow was set to
const int SPEEDS[] = {4, 6, 8, 12, 20, 30};
int slide_seconds() { return (int)std::clamp<int64_t>(store::get_int("slideshow_seconds", 6), 3, 60); }
enum Movement { MOVE_SMART = 0, MOVE_GENTLE = 1, MOVE_STILL = 2 };
int movement() { return (int)std::clamp<int64_t>(store::get_int("slideshow_move", MOVE_SMART), 0, 2); }
enum Music { MUSIC_OFF = 0, MUSIC_KEEP = 1, MUSIC_MINE = 2 };
int music_mode() { return (int)std::clamp<int64_t>(store::get_int("slideshow_music", MUSIC_KEEP), 0, 2); }
bool shuffled() { return store::get_bool("slideshow_shuffle", false); }

std::string rot_key(const std::string& path) {
    static const char* hex = "0123456789abcdef";
    const uint64_t h = util::hash64(path);
    std::string k = "photo_rot_";
    for (int i = 60; i >= 0; i -= 4) k += hex[(h >> i) & 15];
    return k;
}
int saved_turns(const std::string& path) { return (int)(store::get_int(rot_key(path).c_str(), 0) & 3); }
void save_turns(const std::string& path, int turns) { store::set_int(rot_key(path).c_str(), turns & 3); }

// A photo's details, read from the start of its file.
struct Details {
    bool read = false;
    int w = 0, h = 0;
    std::string taken, camera;
    long long bytes = 0;
};

// "2026:10:16 14:03:22" as "October 16, 2026  14:03".
std::string nice_date(const std::string& exif) {
    if (exif.size() < 16 || exif[4] != ':' || exif[7] != ':') return "";
    const int month = atoi(exif.substr(5, 2).c_str()) - 1;
    if (month < 0 || month > 11) return "";
    std::string s = tr("{month} {day}, {year}");
    s = util::replace_all(s, "{month}", i18n::month_name(month));
    s = util::replace_all(s, "{day}", std::to_string(atoi(exif.substr(8, 2).c_str())));
    s = util::replace_all(s, "{year}", exif.substr(0, 4));
    return s + "  " + exif.substr(11, 5);
}

focus::Focus turned(focus::Focus f, int turns) {
    for (int i = 0; i < (turns & 3); i++) {   // a quarter turn clockwise takes (x, y) to (1 - y, x)
        const focus::Focus o = f;
        f.cx = 1 - o.cy;
        f.cy = o.cx;
        f.x0 = 1 - o.y1;
        f.x1 = 1 - o.y0;
        f.y0 = o.x0;
        f.y1 = o.x1;
        f.main_w = o.main_h;
        f.main_h = o.main_w;
    }
    return f;
}

// One picture on its way in or out.
struct Slot {
    int idx = -1;
    int turns = 0;
    double born = -1;               // when it first showed (-1: not yet); a slideshow move counts from here
    photo_motion::Plan plan;
    bool moving = false;            // drawn by its plan (slideshow), not by hand
    float last_scale = 1, last_dx = 0, last_dy = 0;   // where it was drawn last, for it to leave from
};

// The slideshow's own settings, and the way to start it.
class SlideshowScreen : public app::Screen {
public:
    explicit SlideshowScreen(bool* start) : start_(start) {}
    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    void frame() override {
        const Theme& t = theme();
        const float x0 = 120, w = 780;
        const Id g = id("slideshow_set");
        float y = 70;
        text::draw(font::display, x0, y, tr("Slideshow"), t.text);
        y += 90;
        auto row = [&](const char* key, const char* label, const std::string& value, int icon, bool first = false) {
            const bool hit = value_row(id(g, key), Rect(x0, y, w, 64), label, value.c_str(), icon, g, (first ? F_DEFAULT : 0) | F_WRAP);
            y += 74;
            return hit;
        };
        if (row("speed", tr("Time for each photo"), util::fmt(tr("%d seconds"), slide_seconds()), ic::SCHEDULE, true)) {
            int next = SPEEDS[0];
            for (int s : SPEEDS)
                if (s > slide_seconds()) {
                    next = s;
                    break;
                }
            store::set_int("slideshow_seconds", next);
        }
        static const char* MOVES[] = {N_("Follows the people"), N_("Gentle"), N_("None")};
        if (row("move", tr("Movement"), tr(MOVES[movement()]), ic::AUTO_AWESOME)) store::set_int("slideshow_move", (movement() + 1) % 3);
        static const char* MUSICS[] = {N_("Off"), N_("What is playing"), N_("My music")};
        if (row("music", tr("Music"), tr(MUSICS[music_mode()]), ic::MUSIC)) store::set_int("slideshow_music", (music_mode() + 1) % 3);
        bool sh = shuffled();
        if (toggle_row(id(g, "shuffle"), Rect(x0, y, w, 82), tr("Shuffle"), tr("In any order, not the order of the folder"), &sh, g, F_WRAP))
            store::set_bool("slideshow_shuffle", sh);
        y += 100;
        if (button(id(g, "start"), Rect(x0, y, 320, 56), tr("Start slideshow"), ic::PLAY, BTN_PRIMARY, g, F_WRAP)) {
            *start_ = true;
            app::pop();
            return;
        }
        text::draw_wrapped(font::small, Rect(x0, y + 84, w, 80),
                           tr("With \"Follows the people\", a picture of one person comes in close on them, and a picture of many stays wide."),
                           t.text3, 3);
        hint_bar({{"A", tr("Change")}, {"B", tr("Back")}});
    }

private:
    bool* start_;
};

class PhotoViewer : public app::Screen {
public:
    PhotoViewer(std::vector<std::string> paths, int index) : paths_(std::move(paths)) {
        const int n = (int)paths_.size();
        index = n ? std::clamp(index, 0, n - 1) : 0;
        order_.resize(n);
        touched_.assign(n, 0);
        for (int i = 0; i < n; i++) order_[i] = i;
        pos_ = index;
        begin_slot(cur_, n ? index : -1);
    }

    ~PhotoViewer() override {
        stop_music();
        for (size_t i = 0; i < touched_.size(); i++)
            if (touched_[i]) images::release(paths_[i], 800);
    }

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    bool on_back() override {
        if (info_) {
            info_ = false;
            return true;
        }
        if (slideshow_) {
            stop_slideshow();
            return true;
        }
        if (zoom_ > 1.01f || std::fabs(pan_x_) > 1 || std::fabs(pan_y_) > 1) {
            reset_view();
            return true;
        }
        return false;
    }

    void on_enter() override {
        if (start_requested_) {
            start_requested_ = false;
            start_slideshow();
        }
    }

    void frame() override {
        Input& in = input();
        suspend_nav();
        gfx::fill_rect(Rect(0, 0, W, H), gfx::BLACK);
        if (paths_.empty()) return;
        if (!menu_active()) handle_input(in);
        tick();

        const double now = ui::time();
        const double cross = slideshow_ ? CROSS_SLIDE : CROSS_PAGE;
        const float ft = cur_.born < 0 ? 0.0f : anim::smoothstep((float)((now - cur_.born) / cross));
        if (prev_.idx >= 0 && ft >= 1) {
            prev_ = Slot();
            trim_cache();
        }
        if (prev_.idx >= 0) draw_slot(prev_, 1.0f, false);
        const bool shown = draw_slot(cur_, prev_.idx >= 0 ? ft : 1.0f, true);
        if (!shown) wait_or_fail();
        preload();
        draw_overlay();
    }

private:
    std::vector<std::string> paths_;
    std::vector<int> order_;     // the order they come in (shuffled for a shuffled slideshow)
    int pos_ = 0;
    Slot cur_, prev_;
    std::vector<char> touched_;   // pictures asked for at their viewing size: let go of once they are away (trim_cache)
    std::map<int, Details> details_;
    // by hand
    float zoom_ = 1, pan_x_ = 0, pan_y_ = 0;
    // the slideshow, the overlay
    bool slideshow_ = false, start_requested_ = false, info_ = false;
    int show_seconds_ = 6;
    bool music_started_ = false;
    tasks::Scope music_scope_;
    double shown_at_ = 0, last_tap_ = -10, failed_since_ = -1;
    float last_tap_x_ = 0, last_tap_y_ = 0;

    const std::string& path_of(const Slot& s) const { return paths_[s.idx]; }

    void begin_slot(Slot& s, int idx) {
        s = Slot();
        s.idx = idx;
        if (idx >= 0) s.turns = saved_turns(paths_[idx]);
        if (idx >= 0 && idx < (int)touched_.size()) touched_[idx] = 1;
    }

    // Every picture looked at stays loaded (3-15 MB each) until the cache's budget evicts it: an evening of paging or a slideshow would
    // keep the console at that ceiling and push every other screen's pictures out. Only the ones about to be shown are kept.
    void trim_cache() {
        const int n = (int)paths_.size();
        if (n <= 5 || (int)touched_.size() != n) return;
        std::vector<char> keep(n, 0);
        auto k = [&](int i) {
            if (i >= 0 && i < n) keep[i] = 1;
        };
        k(cur_.idx);
        k(prev_.idx);
        for (int d : {0, 1, -1, 2}) k(order_[(pos_ + d + n * 2) % n]);
        for (int i = 0; i < n; i++)
            if (touched_[i] && !keep[i]) {
                images::release(paths_[i], 800);   // the viewing sizes; the grid's small ones stay
                touched_[i] = 0;
            }
    }

    // The picture to show now: the right size once it has loaded; until then whatever of it is loaded already (the small one of
    // My Files), so there is never nothing.
    const images::Image* pick(const std::string& path, bool zoomed, bool* final) {
        const images::Image* fit = images::get(path, fit_w(), fit_h(), images::FOCUS);
        if (zoomed) {
            const images::Image* hi = images::get(path, hi_w(), hi_h());
            if (hi && hi->ready && !hi->preview) {
                *final = true;
                return hi;
            }
        }
        if (fit && fit->ready && !fit->preview) {
            *final = true;
            return fit;
        }
        *final = false;
        if (fit && fit->ready) return fit;
        return images::best_loaded(path);
    }

    void preload() {
        const int n = (int)paths_.size();
        if (n <= 1) return;
        for (int d : {1, -1, 2}) {
            const int i = order_[(pos_ + d + n * 2) % n];
            images::get(paths_[i], fit_w(), fit_h(), images::FOCUS);
            if (i < (int)touched_.size()) touched_[i] = 1;
        }
    }

    // --- drawing
    // Draws a slot; false when there is nothing of it to show yet.
    bool draw_slot(Slot& s, float alpha, bool is_cur) {
        if (s.idx < 0) return false;
        const std::string& path = path_of(s);
        const bool zoomed = is_cur && !slideshow_ && zoom_ > 1.5f;
        bool final = false;
        const images::Image* img = pick(path, zoomed, &final);
        if (!img || !img->ready) return false;
        const double now = ui::time();
        // A slideshow picture's move begins when its own size has loaded (that is when the people in it are known); by hand,
        // it shows at once, even as the small stand-in.
        if (s.born < 0 && is_cur && (final || !slideshow_)) {
            s.born = now;
            if (slideshow_) plan_slot(s, img);
        }
        const bool odd = s.turns & 1;
        const float rw = odd ? (float)img->h : (float)img->w, rh = odd ? (float)img->w : (float)img->h;
        const float sc = std::min(W / rw, H / rh);
        const float dw = rw * sc, dh = rh * sc;

        float scale = 1, dx = 0, dy = 0;
        if (s.moving && s.born >= 0) {
            // Along its own move; a picture fading out keeps going, it doesn't spring back.
            const float span = (float)(show_seconds_ + CROSS_SLIDE);
            photo_motion::View v = photo_motion::at(s.plan, (float)(now - s.born) / span);
            v = photo_motion::clamp(v, rw / rh, W / H);
            const photo_motion::Transform t = photo_motion::transform(v, dw, dh);
            scale = t.scale;
            dx = t.dx;
            dy = t.dy;
        } else if (is_cur) {
            scale = spring(id("pv_zoom"), zoom_, 200, 24);
            // Not past the edge of the picture.
            const float mx = std::max(0.0f, (dw * scale - W) * 0.5f) / scale, my = std::max(0.0f, (dh * scale - H) * 0.5f) / scale;
            pan_x_ = std::clamp(pan_x_, -mx, mx);
            pan_y_ = std::clamp(pan_y_, -my, my);
            dx = spring(id("pv_px"), pan_x_, 200, 26) * scale;
            dy = spring(id("pv_py"), pan_y_, 200, 26) * scale;
        } else {
            scale = s.last_scale;   // leaves from where it was
            dx = s.last_dx;
            dy = s.last_dy;
        }
        s.last_scale = scale;
        s.last_dx = dx;
        s.last_dy = dy;

        // The blur of the picture behind it where it doesn't fill the screen.
        if (s.turns == 0 && (dw < W - 2 || dh < H - 2)) {
            const images::Image* blur = images::get(path, 0, 0, images::BLUR);
            if (blur && blur->ready)
                gfx::image_cover(blur->tex, blur->w, blur->h, Rect(-W * 0.15f, -H * 0.15f, W * 1.3f, H * 1.3f), 0,
                                 Color(255, 255, 255, (uint8_t)(120 * alpha * images::fade(blur, 0.4f))));
        }
        gfx::push_alpha(alpha);
        gfx::push_transform(scale, W * 0.5f, H * 0.5f, dx, dy);
        gfx::image_rotated(img->tex, Rect((W - dw) * 0.5f, (H - dh) * 0.5f, dw, dh), s.turns,
                           Color(255, 255, 255, (uint8_t)(255 * images::fade(img, 0.3f))));
        gfx::pop_transform();
        gfx::pop_alpha();
        if (!final && is_cur) spinner(W - 60, 60, 14, Color(255, 255, 255, 190), 3);   // the sharp one is on its way
        return true;
    }

    void wait_or_fail() {
        if (cur_.idx < 0) return;
        const images::Image* img = images::get(path_of(cur_), fit_w(), fit_h(), images::FOCUS);
        if (img && img->failed) {
            empty_state(Rect(W * 0.5f - 300, H * 0.5f - 150, 600, 300), ic::ERROR_OUTLINE, tr("Can't open this image"), "");
            if (failed_since_ < 0) failed_since_ = ui::time();
        } else {
            spinner(W * 0.5f, H * 0.5f, 28, theme().accent, 4);
        }
    }

    void plan_slot(Slot& s, const images::Image* img) {
        focus::Focus f;
        const bool known = images::focus_of(path_of(s), f);
        if (movement() == MOVE_GENTLE || !known) f = focus::Focus();
        f = turned(f, s.turns);
        const bool odd = s.turns & 1;
        const float aspect = odd ? (float)img->h / img->w : (float)img->w / img->h;
        const bool still = movement() == MOVE_STILL || personal::reduce_motion();
        s.plan = photo_motion::plan(f, aspect, W / H, util::hash64(path_of(s)) ^ (uint64_t)s.idx, still);
        s.moving = true;
    }

    // --- input
    void handle_input(Input& in) {
        if (in.any()) shown_at_ = ui::time();
        const int n = (int)paths_.size();
        const bool at_rest = zoom_ <= 1.01f;
        if (in.pressed_(BTN_R) || (at_rest && in.rep(BTN_RIGHT))) go(1);
        if (in.pressed_(BTN_L) || (at_rest && in.rep(BTN_LEFT))) go(-1);
        if (n > 1 && at_rest && in.touch_ended && in.dragging && std::fabs(in.tx - in.touch_start_x) > 80 &&
            std::fabs(in.tx - in.touch_start_x) > std::fabs(in.ty - in.touch_start_y))
            go(in.tx < in.touch_start_x ? 1 : -1);
        if (!at_rest && !slideshow_) {
            const float sp = 900 * ui::dt() / zoom_;
            if (in.down(BTN_RIGHT)) pan_x_ -= sp;
            if (in.down(BTN_LEFT)) pan_x_ += sp;
            if (in.down(BTN_DOWN)) pan_y_ -= sp;
            if (in.down(BTN_UP)) pan_y_ += sp;
            pan_x_ -= in.lx * sp * 1.4f;
            pan_y_ += in.ly * sp * 1.4f;
            if (in.touching && in.dragging) {
                pan_x_ += in.tdx / zoom_;
                pan_y_ += in.tdy / zoom_;
            }
        }
        if (!slideshow_) {
            if (in.pressed_(BTN_ZR) || in.pressed_(BTN_A)) {
                if (!(in.pointer && toolbar_tap(in.px, in.py))) zoom_in();
            }
            if (in.pressed_(BTN_ZL)) zoom_ = std::max(zoom_ / 1.6f, 1.0f);
            if (in.pressed_(BTN_X)) rotate();
            // Double tap: close in where the finger is, or back out.
            if (in.tap && !toolbar_tap(in.tx, in.ty)) {
                if (ui::time() - last_tap_ < 0.35 && std::fabs(in.tx - last_tap_x_) < 60 && std::fabs(in.ty - last_tap_y_) < 60) {
                    if (zoom_ > 1.05f) reset_view();
                    else zoom_at(2.5f, in.tx, in.ty);
                    last_tap_ = -10;
                } else {
                    last_tap_ = ui::time();
                    last_tap_x_ = in.tx;
                    last_tap_y_ = in.ty;
                }
            }
        }
        if (in.pressed_(BTN_Y)) {
            if (slideshow_) stop_slideshow();
            else start_slideshow();
        }
        // + and - are the viewer's own here: eaten, so the mini player and the GamePad views drawer don't take them too.
        if (in.pressed_(BTN_PLUS)) {
            in.eat(BTN_PLUS);
            app::push(std::make_unique<SlideshowScreen>(&start_requested_));
        }
        if (in.pressed_(BTN_MINUS)) {
            in.eat(BTN_MINUS);
            info_ = !info_;
        }
        if (zoom_ <= 1.0f) pan_x_ = pan_y_ = 0;
    }

    void zoom_in() { zoom_ = zoom_ >= ZOOM_MAX - 0.01f ? 1.0f : std::min(zoom_ * 1.6f, ZOOM_MAX); }

    void zoom_at(float z, float sx, float sy) {
        // The point under the finger stays under it: pan = (point - middle) * (1/z - 1).
        zoom_ = z;
        pan_x_ = (sx - W * 0.5f) * (1.0f / z - 1);
        pan_y_ = (sy - H * 0.5f) * (1.0f / z - 1);
    }

    void reset_view() {
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
        set_value(id("pv_zoom"), 1);
        set_value(id("pv_px"), 0);
        set_value(id("pv_py"), 0);
    }

    void rotate() {
        if (cur_.idx < 0) return;
        cur_.turns = (cur_.turns + 1) & 3;
        save_turns(path_of(cur_), cur_.turns);
        reset_view();
        toast(tr("Turned. It stays this way."), ic::ROTATE_RIGHT);
    }

    // The little buttons for touch and the pointer: turn, zoom, slideshow, details.
    Rect tool_rect(int i) const { return Rect(60 + i * 64.0f, H - 150, 52, 52); }
    bool toolbar_tap(float x, float y) {
        if (slideshow_ || (ui::time() - shown_at_ > 2.5 && !info_)) return false;   // not showing
        for (int i = 0; i < 4; i++) {
            if (!tool_rect(i).contains(x, y)) continue;
            if (i == 0) rotate();
            else if (i == 1) zoom_in();
            else if (i == 2) start_slideshow();
            else info_ = !info_;
            return true;
        }
        return false;
    }

    // --- paging and the slideshow
    void go(int d) {
        const int n = (int)paths_.size();
        if (n <= 1) return;
        prev_ = cur_;   // what leaves keeps going from where it was
        pos_ = (pos_ + d + n) % n;
        begin_slot(cur_, order_[pos_]);
        reset_view();
        failed_since_ = -1;
    }

    void tick() {
        if (!slideshow_) return;
        const double now = ui::time();
        const int n = (int)paths_.size();
        if (n <= 1) return;
        if (cur_.born >= 0 && now - cur_.born >= show_seconds_) {
            // Wait for the next to have loaded, so there is no dark between.
            const images::Image* nx = images::get(paths_[order_[(pos_ + 1) % n]], fit_w(), fit_h(), images::FOCUS);
            const bool ready = nx && ((nx->ready && !nx->preview) || nx->failed);
            if (ready || now - cur_.born > show_seconds_ + 8) go(1);
        } else if (failed_since_ >= 0 && now - failed_since_ > 1.5) {
            go(1);   // one that would not open is passed
        }
    }

    void start_slideshow() {
        if (paths_.empty()) return;
        show_seconds_ = slide_seconds();
        slideshow_ = true;
        info_ = false;
        // Shuffled: the order from here is made afresh, this picture first.
        const int n = (int)paths_.size();
        const int current = order_[pos_];
        if (shuffled() && n > 2) {
            std::vector<int> rest;
            for (int i = 0; i < n; i++)
                if (i != current) rest.push_back(i);
            std::mt19937_64 rng((uint64_t)(util::now_seconds() * 1000));
            std::shuffle(rest.begin(), rest.end(), rng);
            order_.assign(1, current);
            order_.insert(order_.end(), rest.begin(), rest.end());
            pos_ = 0;
        }
        reset_view();
        cur_.born = -1;     // its move begins now
        cur_.moving = false;
        prev_ = Slot();
        toast(tr("Slideshow on"), ic::SLIDESHOW);
        start_music();
    }

    void stop_slideshow() {
        if (!slideshow_) return;
        // Where the move had got to is where it stays (it doesn't jump back).
        if (cur_.moving && cur_.born >= 0 && cur_.last_scale > 1.01f) {
            zoom_ = cur_.last_scale;
            pan_x_ = cur_.last_dx / zoom_;
            pan_y_ = cur_.last_dy / zoom_;
            set_value(id("pv_zoom"), zoom_);
            set_value(id("pv_px"), pan_x_);
            set_value(id("pv_py"), pan_y_);
        }
        cur_.moving = false;
        prev_.moving = false;
        slideshow_ = false;
        stop_music();
        toast(tr("Slideshow off"), ic::SLIDESHOW);
    }

    // --- the music
    void start_music() {
        if (music_mode() != MUSIC_MINE) return;
        if (player::active() && !player::has_video()) return;   // there's music on already
        media::SurpriseOptions o;
        o.roots.push_back(util::join_path(platform::media_root(), "Music"));
        o.roots.push_back(platform::media_root());
        platform::Volume vols[8];
        const int nv = platform::volumes(vols, 8);
        for (int i = 0; i < nv; i++)
            if (vols[i].path != platform::media_root()) o.roots.push_back(vols[i].path);
        o.skip_top = {"wiiu", "System Volume Information", "$RECYCLE.BIN"};
        o.scope = media::SURPRISE_MUSIC;
        o.seconds = 3;
        o.seed = (uint64_t)(util::now_seconds() * 1e6);
        music_scope_.run<media::Surprise>([o] { return media::pick_surprise(o); }, [this](media::Surprise p) {
            if (!slideshow_) return;
            if (!p.ok) {
                toast(tr("No music found on the card"), ic::MUSIC);
                return;
            }
            play_audio_queue(media::music_queue_from(p.path, (uint64_t)(util::now_seconds() * 1e3)), 0, false);
            music_started_ = true;
        });
    }

    void stop_music() {
        music_scope_.reset();
        if (music_started_ && player::active() && !player::has_video()) player::close();
        music_started_ = false;
    }

    // --- the overlay
    // A photo's details, read on a worker: reading the start of a file on the card while the picture loaders read whole photos
    // took the main thread 250 ms on the console. Empty until read.
    static Details read_details(const std::string& path) {
        Details d;
        d.read = true;
        if (path.find("://") != std::string::npos) return d;   // only files on the card
        struct stat st;
        if (stat(path.c_str(), &st) == 0) d.bytes = (long long)st.st_size;
        jpeg_tools::Info info;
        const std::string head = thumb_cache::read_prefix(path, 70000);
        if (jpeg_tools::read_info(head, info)) {
            const bool sideways = info.orientation >= 5 && info.orientation <= 8;
            d.w = sideways ? info.height : info.width;
            d.h = sideways ? info.width : info.height;
            d.taken = info.taken;
            d.camera = info.camera;
        } else {
            int64_t w = 0, h = 0;
            if (images::stated_size(head, w, h)) {
                d.w = (int)w;
                d.h = (int)h;
            }
        }
        return d;
    }

    const Details& details_of(int idx) {
        Details& d = details_[idx];
        if (!d.read && !details_asked_.count(idx)) {
            details_asked_.insert(idx);
            details_scope_.run<Details>([path = paths_[idx]] { return read_details(path); },
                                        [this, idx](Details got) { details_[idx] = std::move(got); });
        }
        return d;
    }
    std::set<int> details_asked_;
    tasks::Scope details_scope_;

    void draw_overlay() {
        const Theme& t = theme();
        const bool recent = ui::time() - shown_at_ < 2.5;
        const float a = tween(id("pv_info"), recent || info_ ? 1.0f : 0.0f, 8);
        if (a <= 0.01f || cur_.idx < 0) return;
        gfx::push_alpha(a);
        gfx::fill_rect_vgrad(Rect(0, H - 170, W, 170), Color(0, 0, 0, 0), Color(0, 0, 0, 200));
        const Details& d = details_of(cur_.idx);
        text::draw_fit(font::title, 60, H - 84, W - 560, util::file_name(path_of(cur_)), t.text);
        std::string sub = util::fmt(tr("%d of %zu"), pos_ + 1, paths_.size());
        if (d.w > 0) sub += util::fmt("  \xC2\xB7  %d \xC3\x97 %d", d.w, d.h);
        const std::string when = nice_date(d.taken);
        if (!when.empty()) sub += "  \xC2\xB7  " + when;
        text::draw_fit(font::small_bold, 60, H - 46, W - 560, sub, t.text2);
        if (slideshow_) hint_bar({{"B", tr("Stop")}, {"+", tr("Options")}, {"L/R", tr("Skip")}}, H - 60);
        else hint_bar({{"A", tr("Zoom")}, {"X", tr("Turn")}, {"Y", tr("Slideshow")}, {"-", tr("Details")}}, H - 60);
        if (!slideshow_) {
            const int icons[4] = {ic::ROTATE_RIGHT, ic::ZOOM_IN, ic::SLIDESHOW, ic::INFO};
            for (int i = 0; i < 4; i++) {
                const Rect r = tool_rect(i);
                gfx::fill_rrect(r, 26, Color(255, 255, 255, 34));
                text::icon(icons[i], 30, r.cx(), r.cy(), Color(255, 255, 255, 230));
            }
        }
        gfx::pop_alpha();
        if (info_) draw_details(d);
    }

    void draw_details(const Details& d) {
        const Theme& t = theme();
        const float w = 440, x = W - w - 40, y = 60;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({tr("Name"), util::file_name(path_of(cur_))});
        rows.push_back({tr("Folder"), util::file_name(util::parent_dir(path_of(cur_)))});
        if (d.w > 0) rows.push_back({tr("Size"), util::fmt("%d \xC3\x97 %d", d.w, d.h)});
        if (d.bytes > 0) rows.push_back({tr("File"), util::format_bytes((uint64_t)d.bytes)});
        const std::string when = nice_date(d.taken);
        if (!when.empty()) rows.push_back({tr("Taken"), when});
        if (!d.camera.empty()) rows.push_back({tr("Camera"), d.camera});
        focus::Focus f;
        if (images::focus_of(path_of(cur_), f) && f.found && f.people > 0)
            rows.push_back({tr("People"), f.people >= 4 ? std::string(tr("Several")) : std::to_string(f.people)});
        const float h = 40 + rows.size() * 50.0f;
        gfx::fill_rrect(Rect(x, y, w, h), 18, Color(10, 8, 14, 215));
        float yy = y + 22;
        for (auto& [k, v] : rows) {
            text::draw_fit(font::small, x + 24, yy + 4, 110, k, t.text3);
            text::draw_fit(font::body, x + 140, yy, w - 164, v, t.text);
            yy += 50;
        }
    }
};

}  // namespace

std::unique_ptr<app::Screen> make_photo_viewer(std::vector<std::string> paths, int index) {
    return std::make_unique<PhotoViewer>(std::move(paths), index);
}

}  // namespace screens
