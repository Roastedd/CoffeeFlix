#include "app/photo_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "app/personal.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/photo_motion.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/surprise.hpp"
#include "ui/ui.hpp"

namespace photo_frame {

namespace {

using gfx::Color;
using ui::Rect;
using namespace screens::media;

constexpr float CROSSFADE = 1.8f;
constexpr size_t RECENT = 40;

struct Slide {
    std::string path;
    float age = 0;  // seconds on screen
    photo_motion::Plan plan;
    bool planned = false;
    float last_scale = 1, last_dx = 0, last_dy = 0;
};

struct State {
    bool on = false;
    Slide cur, prev;
    std::string next;              // picked, being loaded
    bool asking = false;           // a pick is on its way
    bool none = false;             // looked, found nothing
    double retry_at = 0;           // when to look again after finding nothing
    std::deque<std::string> recent;
    float clock_t = 0;
    std::string generation;        // told apart from an old run's reply
    uint64_t epoch = 1;
    int shown = 0;
};
State st;

std::string photos_folder() { return util::join_path(platform::media_root(), "Photos"); }

void ask() {
    if (st.asking) return;
    st.asking = true;
    SurpriseOptions o;
    const bool card = source() == FROM_CARD;
    o.roots.push_back(photos_folder());
    if (card || true) o.roots.push_back(platform::media_root());  // a folder without Photos: what is in the CoffeeFlix folder
    if (card) {
        platform::Volume vols[8];
        const int n = platform::volumes(vols, 8);
        for (int i = 0; i < n; i++)
            if (vols[i].path != platform::media_root()) o.roots.push_back(vols[i].path);
    }
    o.skip_top = {"wiiu", "System Volume Information", "$RECYCLE.BIN"};
    o.scope = SURPRISE_PHOTOS;
    o.seconds = 2.5;
    o.seed = (uint64_t)(util::now_seconds() * 1e6) ^ (st.epoch * 0x9E3779B97F4A7C15ull);
    o.avoid.assign(st.recent.begin(), st.recent.end());
    const uint64_t epoch = st.epoch;
    tasks::submit(tasks::API, [o, epoch] {
        const Surprise p = pick_surprise(o);
        return [p, epoch] {
            if (epoch != st.epoch) return;  // from before the frame was last stopped
            st.asking = false;
            if (!p.ok) {
                st.none = true;
                st.retry_at = util::now_seconds() + 60;
                return;
            }
            st.none = false;
            st.next = p.path;
            st.recent.push_back(p.path);
            if (st.recent.size() > RECENT) st.recent.pop_front();
        };
    });
}

// The size a picture is decoded to: the UI's own (a 1080p TV is shown the 720p layout enlarged). The software renderer the
// screenshot tests use draws no full-screen picture of this size, so they ask for a smaller one.
int max_width() {
    static const int w = getenv("COFFEEFLIX_PHOTO_MAX") ? atoi(getenv("COFFEEFLIX_PHOTO_MAX")) : 1280;
    return w;
}
const images::Image* load(const std::string& path) { return images::get(path, max_width(), max_width() * 9 / 16, images::FOCUS); }

// A picture, laid in the blur of itself, at `alpha`. Its move is planned once, from where the people in it are, and goes on
// along the same line while it fades out.
void draw_photo(Slide& slide, float duration, float alpha, bool still) {
    const std::string& path = slide.path;
    const images::Image* blur = images::get(path, 0, 0, images::BLUR);
    const images::Image* img = load(path);
    if (!img || !img->ready) return;
    const float aspect = (float)img->w / img->h;
    if (!slide.planned) {
        focus::Focus f;
        images::focus_of(path, f);
        slide.plan = photo_motion::soften(photo_motion::plan(f, aspect, ui::W / ui::H, util::hash64(path), still), 0.6f);
        slide.planned = true;
    }
    const float dw = aspect >= ui::W / ui::H ? ui::W : ui::H * aspect, dh = aspect >= ui::W / ui::H ? ui::W / aspect : ui::H;
    photo_motion::View v = photo_motion::at(slide.plan, slide.age / std::max(1.0f, duration + CROSSFADE));
    v = photo_motion::clamp(v, aspect, ui::W / ui::H);
    const photo_motion::Transform t = photo_motion::transform(v, dw, dh);
    if (blur && blur->ready) {
        const float z = 1.3f;
        gfx::image_cover(blur->tex, blur->w, blur->h, Rect(-ui::W * (z - 1) * 0.5f, -ui::H * (z - 1) * 0.5f, ui::W * z, ui::H * z), 0,
                         Color(255, 255, 255, (uint8_t)(150 * alpha)));
    }
    gfx::push_alpha(alpha);
    gfx::push_transform(t.scale, ui::W * 0.5f, ui::H * 0.5f, t.dx, t.dy);
    gfx::image_contain(img->tex, img->w, img->h, Rect(0, 0, ui::W, ui::H), Color(255, 255, 255, 255), 0);
    gfx::pop_transform();
    gfx::pop_alpha();
}

}  // namespace

bool wanted() { return store::get_int("screensaver_style", 0) == 1; }
void set_wanted(bool on) { store::set_int("screensaver_style", on ? 1 : 0); }
Source source() { return store::get_int("frame_source", 0) == 1 ? FROM_CARD : FROM_PHOTOS_FOLDER; }
void set_source(Source s) { store::set_int("frame_source", s == FROM_CARD ? 1 : 0); }
int seconds_per_photo() { return (int)std::clamp<int64_t>(store::get_int("frame_seconds", 12), 5, 60); }
void set_seconds_per_photo(int s) { store::set_int("frame_seconds", std::clamp(s, 5, 60)); }
bool show_clock() { return store::get_bool("frame_clock", true); }
void set_show_clock(bool on) { store::set_bool("frame_clock", on); }

Drift drift_for(const std::string& path, float t, float duration, bool still) {
    // A picture with nothing known about it: a slow drift of its own, in or out a little with a little shift.
    const float sa = 16.0f / 9;
    const photo_motion::Plan plan = photo_motion::plan(focus::Focus(), sa, sa, util::hash64(path), still);
    const photo_motion::View v = photo_motion::at(plan, t / std::max(1.0f, duration + CROSSFADE));
    const photo_motion::Transform tr_ = photo_motion::transform(v, 1.0f, 1.0f);
    return {tr_.scale, tr_.dx, tr_.dy};
}

void begin() {
    st.on = true;
    st.epoch++;
    st.cur = Slide();
    st.prev = Slide();
    st.next.clear();
    st.asking = false;
    st.none = false;
    st.retry_at = 0;
    st.clock_t = 0;
    st.shown = 0;
}

void end() {
    images::release(st.cur.path);
    images::release(st.prev.path);
    images::release(st.next);
    st.on = false;
    st.epoch++;
    st.asking = false;
    st.next.clear();
    st.cur = Slide();
    st.prev = Slide();
}

bool draw(float seconds, float dt) {
    (void)seconds;
    if (!st.on) return false;
    const bool still = personal::reduce_motion();
    const float duration = (float)seconds_per_photo();
    const double now = util::now_seconds();

    if (st.cur.path.empty() && st.next.empty() && !st.asking && (!st.none || now >= st.retry_at)) ask();
    if (st.cur.path.empty()) {
        if (!st.next.empty()) {  // the first picture: shown when it has loaded
            const images::Image* img = load(st.next);
            if (img && img->ready) {
                st.cur.path = st.next;
                st.cur.age = 0;
                st.next.clear();
                st.shown++;
                log_message(LOG_DEBUG, "PhotoFrame", "Picture %d: %s (%dx%d shown); pictures held %zu KB", st.shown,
                            util::file_name(st.cur.path).c_str(), img->w, img->h, images::bytes_used() / 1024);
                ask();
            }
        }
        if (st.cur.path.empty()) return !st.none;  // still looking (the dark shows); nothing found: the clock instead
    }

    st.cur.age += dt;
    if (!st.prev.path.empty()) {
        st.prev.age += dt;
        if (st.prev.age > CROSSFADE + 0.1f) {  // gone for good: its pictures need not be kept (the cache would only grow)
            if (st.prev.path != st.cur.path) images::release(st.prev.path);
            st.prev = Slide();
        }
    }
    // The next picture, once the time is up and it has loaded.
    if (st.cur.age >= duration && !st.next.empty()) {
        const images::Image* img = load(st.next);
        if (img && img->ready) {
            st.prev = st.cur;
            st.cur.path = st.next;
            st.cur.age = 0;
            st.next.clear();
            st.shown++;
            log_message(LOG_DEBUG, "PhotoFrame", "Picture %d: %s (%dx%d shown); pictures held %zu KB", st.shown,
                        util::file_name(st.cur.path).c_str(), img->w, img->h, images::bytes_used() / 1024);
            ask();
        } else if (img && img->failed) {  // a picture that would not load: another
            st.next.clear();
            ask();
        }
    }
    if (!st.next.empty()) load(st.next);  // loading ahead
    if (st.next.empty() && !st.asking && (!st.none || now >= st.retry_at)) ask();

    const float fade_in = std::min(1.0f, st.cur.age / CROSSFADE);
    const float in = fade_in * fade_in * (3 - 2 * fade_in);
    if (!st.prev.path.empty()) draw_photo(st.prev, duration, 1.0f, still);
    draw_photo(st.cur, duration, st.prev.path.empty() ? 1.0f : in, still);
    // A veil at the bottom so the time reads over any picture.
    gfx::fill_rect_vgrad(Rect(0, ui::H - 220, ui::W, 220), Color(0, 0, 0, 0), Color(0, 0, 0, 150));
    st.clock_t += dt;

    const ui::Theme& t = ui::theme();
    if (show_clock()) {
        // Moved a few pixels now and then (slowly), so nothing sits in one place.
        const float ox = std::sin(st.clock_t * 0.05f) * 14, oy = std::sin(st.clock_t * 0.037f + 1.0f) * 8;
        const std::string clock = util::clock_hhmm();
        text::Font big = text::font(text::BOLD, 84);
        text::draw(big, 72 + ox, ui::H - 150 + oy, clock, Color(255, 255, 255, 235));
        std::tm lt;
        util::local_tm(&lt);
        static const char* WEEKDAYS[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
        std::string date = tr("{weekday}, {month} {day}");
        date = util::replace_all(date, "{weekday}", tr(WEEKDAYS[lt.tm_wday]));
        date = util::replace_all(date, "{month}", i18n::month_name(lt.tm_mon));
        date = util::replace_all(date, "{day}", std::to_string(lt.tm_mday));
        text::draw(ui::font::title, 76 + ox, ui::H - 150 + oy + text::line_height(big) + 2, date, Color(255, 255, 255, 190));
    }
    // What is playing, small, at the other corner.
    if (player::active() && !player::has_video()) {
        std::string line = player::source().title;
        std::string sub = player::stream_title();
        if (sub.empty()) sub = player::source().subtitle;
        if (!sub.empty()) line += "  \xC2\xB7  " + sub;
        text::icon(ic::MUSIC, 26, ui::W - 92, ui::H - 60, Color(255, 255, 255, 200));
        text::draw_fit(ui::font::body, ui::W - 130, ui::H - 74, 520, line, Color(255, 255, 255, 210), text::RIGHT);
    }
    (void)t;
    return true;
}

}  // namespace photo_frame
