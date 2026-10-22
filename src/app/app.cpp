#include "app/app.hpp"

#include <dirent.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <typeinfo>
#include <vector>

#include "audio/mixer.hpp"
#include "core/blackbox.hpp"
#include "core/cpu.hpp"
#include "core/heartbeat.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/input.hpp"
#include "core/screentime.hpp"
#include "core/sleep_timer.hpp"
#include "core/stats.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/youtube_common.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"
#include "app/ambient.hpp"
#include "app/dev_log.hpp"
#include "app/crash_report.hpp"
#include "app/exit_watchdog.hpp"
#include "app/mini_player.hpp"
#include "app/pad_views.hpp"
#include "screens/osk.hpp"
#include "app/birthday.hpp"
#include "app/celebrate.hpp"
#include "app/greeting.hpp"
#include "app/idle_mug.hpp"
#include "app/personal.hpp"
#include "app/sound_settings.hpp"
#include "app/updater.hpp"
#include "core/lock.hpp"
#include "core/settings_import.hpp"
#include "player/http_io.hpp"
#include "player/player.hpp"
#include "services/jellyfin.hpp"
#include "services/yt_account.hpp"

namespace app {

namespace {

constexpr float RAIL_W = 84.0f;
constexpr float RAIL_OPEN_W = 272.0f;

std::vector<std::unique_ptr<Screen>> g_stack;
std::vector<ui::Id> g_saved_focus;  // focus of each covered screen, restored on pop
std::vector<std::unique_ptr<Screen>> g_dead;  // destroyed at end of frame
Section g_section = SEC_HOME;
double g_trans_start = -10;
int g_trans_dir = 1;
bool g_quit = false;
bool g_focus_rail_request = false;
bool g_mini_visible = false;
ui::Id g_rail_group = 0;

struct RailEntry { Section s; int icon; const char* label; };
const RailEntry RAIL[] = {
    {SEC_HOME, ic::HOME, N_("Home")},
    {SEC_SEARCH, ic::SEARCH, N_("Search")},
    {SEC_YOUTUBE, ic::SMART_DISPLAY, "YouTube"},
    {SEC_JELLYFIN, ic::VIDEO_LIBRARY, "Jellyfin"},
    {SEC_NAVIDROME, ic::LIBRARY_MUSIC, "Navidrome"},
    {SEC_TWITCH, ic::LIVE_TV, "Twitch"},
    {SEC_RADIO, ic::RADIO, N_("Radio")},
    {SEC_PODCASTS, ic::PODCASTS, N_("Podcasts")},
    {SEC_MEDIA, ic::FOLDER, N_("My Media")},
    {SEC_SETTINGS, ic::SETTINGS, N_("Settings")},
};

ui::Id rail_id(Section s) { return ui::id(g_rail_group, (int64_t)s); }

void start_transition(int dir) {
    g_trans_start = ui::time();
    g_trans_dir = dir;
}

void draw_rail() {
    using namespace ui;
    const Theme& t = theme();
    bool rail_has_focus = focus_in_group(g_rail_group);
    float open = spring(id("rail_open"), rail_has_focus ? 1.0f : 0.0f, 260, 26);
    float w = RAIL_W + (RAIL_OPEN_W - RAIL_W) * open;

    // Scrim over the content while the rail is open.
    if (open > 0.01f) gfx::fill_rect_hgrad(Rect(0, 0, W, H), Color(5, 4, 8, (uint8_t)(200 * open)), Color(5, 4, 8, (uint8_t)(90 * open)));
    gfx::fill_rect_hgrad(Rect(0, 0, w + 60, H), Color(10, 8, 14, (uint8_t)(170 + 70 * open)), Color(10, 8, 14, 0));

    // Logo. Inside a page it turns into a back button for the GamePad's touch screen.
    float lx = 42, ly = 58;
    float back = tween(id("rail_back"), g_stack.size() > 1 ? 1.0f : 0.0f, 12.0f);
    gfx::fill_circle(lx, ly, 22, t.accent);
    gfx::fill_rrect_hgrad(Rect(lx - 22, ly - 22, 44, 44), 22, t.accent, t.accent2);
    if (back < 0.99f) text::icon(ic::LOCAL_CAFE, 26 * (1 - back), lx, ly, gfx::rgb(0x1A1016));
    if (back > 0.01f) text::icon(ic::ARROW_BACK, 26 * back, lx, ly, gfx::rgb(0x1A1016));
    if (open > 0.02f) {
        gfx::push_alpha(anim::smoothstep((open - 0.3f) / 0.7f));
        text::draw(text::font(text::BOLD, 24), lx + 36, ly - 15, "CoffeeFlix", t.text);
        gfx::pop_alpha();
    }

    set_group_entry(g_rail_group, rail_id(g_section));
    // A kids profile sees only what it may open, and "Grown-ups" where Settings is.
    const bool kids = store::kids_profile();
    std::vector<const RailEntry*> shown;
    for (const RailEntry& e : RAIL)
        if (section_visible(e.s) || e.s == SEC_SETTINGS) shown.push_back(&e);
    int n = (int)shown.size();
    // A phone is too short for the whole column at a size a finger can hit: the entries scroll under the logo
    // (drag them, or move the focus), with Settings the last of them.
    const bool phone = platform::is_phone();
    constexpr float PHONE_TOP = 100, PHONE_STEP = 58;
    static float phone_fa = 0, phone_fb = 0, phone_scroll = 0;
    static bool phone_focus = false;
    bool focus_now = false;
    if (phone) {
        const float content = 12 + n * PHONE_STEP, view = H - PHONE_TOP;
        scroll_drag(id("rail_scroll"), Rect(0, PHONE_TOP, w + 40, view), true, content, view);
        phone_scroll = scroll_follow(id("rail_scroll"), phone_fa, phone_fb, view, content, 30, phone_focus);
        gfx::push_clip(Rect(0, PHONE_TOP, w + 60, view));
    }
    float y = phone ? PHONE_TOP + 6 - phone_scroll : 124;
    const float step = phone ? PHONE_STEP : std::min(54.0f, (H - 124 - 84 - 24 - 50) / std::max(1, n - 2));
    for (int i = 0; i < n; i++) {
        const RailEntry& e = *shown[i];
        const bool grownups = kids && e.s == SEC_SETTINGS;
        const int icon = grownups ? ic::LOCK : e.icon;
        const char* label = grownups ? tr("Grown-ups") : tr(e.label);
        if (e.s == SEC_SETTINGS && !phone) y = H - 84;
        Rect r(14, y, w - 28, 50);
        Item it = focusable(rail_id(e.s), r, g_rail_group, F_NO_MEMORY | F_SIDE_ENTRY | F_WRAP);
        if (phone && it.focused) { focus_now = true; phone_fa = y - 6 + phone_scroll - PHONE_TOP; phone_fb = y + 56 + phone_scroll - PHONE_TOP; }
        if (it.focused) g_focus_rail_request = false;
        if (it.clicked) {
            if (grownups) request_grownups();
            else open_section(e.s);
        }
        bool active = e.s == g_section;
        float f = it.f;
        Rect br = r.offset(bump_x(rail_id(e.s)), bump_y(rail_id(e.s)));
        if (f > 0.01f) gfx::fill_rrect(br, 25, t.surface_focus.alpha(f));
        if (active && f < 0.99f) {
            float a = 1 - f;
            gfx::fill_rrect_hgrad(Rect(4, br.cy() - 14, 5, 28), 2.5f, t.accent.alpha(a), t.accent2.alpha(a));
            if (open > 0.01f) gfx::fill_rrect(br, 25, t.surface.alpha(open * a));
        }
        Color fg = active ? t.accent : t.text2;
        fg = gfx::lerp(fg, gfx::rgb(0x15121A), f);
        text::icon(icon, 28, 42 + bump_x(rail_id(e.s)), br.cy(), fg);
        if (open > 0.02f) {
            gfx::push_alpha(anim::smoothstep((open - 0.25f) / 0.75f));
            Color lc = gfx::lerp(active ? t.text : t.text2, gfx::rgb(0x15121A), f);
            text::draw(active ? font::body_bold : font::body, 76 + bump_x(rail_id(e.s)),
                       br.cy() - text::line_height(font::body) * 0.5f, label, lc);
            gfx::pop_alpha();
        }
        y += step;
    }
    if (phone) {
        gfx::pop_clip();
        phone_focus = focus_now;
    }
}

// The top right corner: who is watching, the GamePad's battery and the time, in one dark capsule so they stay
// readable over a picture, with the same gap between each. Nothing about the network unless it is gone.
// Screen time of a kids profile: once a second, the warnings at ten minutes and at one, and "that's all for today" (or
// quiet time) when it is up. -1 minutes left means nothing to show.
int g_minutes_left = -1;

void check_screen_time() {
    static double next = 0;
    static std::string warned_ten, warned_one;
    const double now = util::now_seconds();
    if (now < next) return;
    next = now + 1;
    g_minutes_left = -1;
    if (!store::kids_profile()) return;
    const screentime::Status st = screentime::status();
    const screentime::Today td = screentime::today();
    if (td.limit_minutes > 0 && st.state != screentime::QUIET) g_minutes_left = (int)std::ceil(st.remaining / 60.0);
    const std::string key = store::active_profile() + util::fmt(":%lld", (long long)util::unix_time() / 86400);
    switch (st.state) {
        case screentime::TEN_LEFT:
            if (warned_ten != key) {
                warned_ten = key;
                ui::toast(tr("10 minutes left today"), ic::SCHEDULE);
            }
            break;
        case screentime::ONE_LEFT:
            if (warned_one != key) {
                warned_one = key;
                ui::toast(tr("1 minute left today"), ic::SCHEDULE);
            }
            break;
        case screentime::TIME_UP:
        case screentime::QUIET:
            if (!screens::time_up_active()) push(screens::make_time_up(st.state == screentime::QUIET, st.quiet_ends));
            break;
        default: break;
    }
}

void draw_status() {
    using namespace ui;
    const Theme& t = theme();
    constexpr float TOP = 12, HEIGHT = 38, PAD = 20, GAP = 22, AVATAR_R = 15;
    const float RIGHT = W - 36;
    const std::string clock = util::clock_hhmm();
    const float clock_w = text::measure(font::label, clock);
    const bool profile = store::has_other_profiles();
    const bool offline = !platform::network_connected();
    const bool timer = g_minutes_left >= 0 && g_minutes_left <= 30;  // a kids profile's time left, when it is getting short
    const std::string timer_text = timer ? util::fmt(tr("%d min left"), std::max(1, g_minutes_left)) : std::string();
    const float timer_w = timer ? 24 + 8 + text::measure(font::small_bold, timer_text) : 0;
    const bool sleeping = sleep_timer::active();  // the sleep timer, running
    const std::string sleep_text = sleeping ? screens::sleep_timer_label() : std::string();
    const float sleep_w = sleeping ? 24 + 8 + text::measure(font::small_bold, sleep_text) : 0;
    const float battery_w = pad_views::battery_width();
    const bool pad_icon = pad_views::available() && battery_w <= 0;  // no reading: the GamePad's own icon opens the panel
    float width = PAD * 2 + clock_w;
    if (offline) width += 24 + GAP;
    if (timer) width += timer_w + GAP;
    if (sleeping) width += sleep_w + GAP;
    if (battery_w > 0) width += battery_w + GAP;
    if (pad_icon) width += 24 + GAP;
    if (profile) width += AVATAR_R * 2 + GAP;

    const Rect capsule(RIGHT - width, TOP, width, HEIGHT);
    const float cy = capsule.cy();
    gfx::fill_rrect(capsule, HEIGHT * 0.5f, Color(10, 8, 14, 150));
    gfx::stroke_rrect(capsule, HEIGHT * 0.5f, 1, Color(255, 255, 255, 26));

    float x = RIGHT - PAD;  // going left from the clock
    text::draw(font::label, x, cy - text::line_height(font::label) * 0.5f, clock, t.text, text::RIGHT);
    x -= clock_w + GAP;
    Input& in = input();
    if (battery_w > 0) {  // a tap on it opens the GamePad panel, as MINUS does
        pad_views::draw_status_battery(x, cy);
        if (in.tap && Rect(x - battery_w - 10, TOP, battery_w + 20, HEIGHT).contains(in.tx, in.ty)) {
            in.tap = false;
            pad_views::open_drawer();
        }
        x -= battery_w + GAP;
    }
    if (sleeping) {
        text::icon(ic::SCHEDULE, 24, x - sleep_w + 12, cy, t.text2);
        text::draw(font::small_bold, x - sleep_w + 24 + 8, cy - text::line_height(font::small_bold) * 0.5f, sleep_text, t.text2);
        x -= sleep_w + GAP;
    }
    if (timer) {
        const Color tc = g_minutes_left <= 5 ? t.warn : t.text2;
        text::icon(ic::SCHEDULE, 24, x - timer_w + 12, cy, tc);
        text::draw(font::small_bold, x - timer_w + 24 + 8, cy - text::line_height(font::small_bold) * 0.5f, timer_text, tc);
        x -= timer_w + GAP;
    }
    if (pad_icon) {
        text::icon(ic::GAMEPAD, 24, x - 12, cy, pad_views::view() != pad_views::VIEW_MIRROR ? t.accent : t.text2);
        if (in.tap && Rect(x - 36, TOP, 48, HEIGHT).contains(in.tx, in.ty)) {
            in.tap = false;
            pad_views::open_drawer();
        }
        x -= 24 + GAP;
    }
    if (offline) {
        text::icon(ic::WIFI_OFF, 24, x - 12, cy, t.bad);
        x -= 24 + GAP;
    }
    if (profile) {  // who is watching; a tap on it changes that (a kids profile asks for the code)
        const float px = x - AVATAR_R;
        screens::draw_profile_avatar(store::active_profile(), px, cy, AVATAR_R);
        if (in.tap && Rect(px - AVATAR_R - 8, TOP, AVATAR_R * 2 + 16, HEIGHT).contains(in.tx, in.ty)) {
            in.tap = false;
            if (store::kids_profile()) request_grownups();
            else push(screens::make_profile_picker(false));
        }
    }
}

void handle_back() {
    Input& in = ui::input();
    bool touch_back = in.tap && g_stack.size() > 1 && !top()->fullscreen() &&
                      ui::Rect(0, 0, RAIL_W + 10, 116).contains(in.tx, in.ty);
    if (touch_back) in.tap = false;
    else if (!in.pressed_(BTN_B)) return;
    in.eat(BTN_B);
    Screen* s = top();
    if (s && s->on_back()) return;
    audio::play(audio::SFX_BACK);
    if (g_stack.size() > 1) {
        pop();
    } else if (!ui::focus_in_group(g_rail_group)) {
        focus_rail();
    } else if (g_section != SEC_HOME) {
        open_section(SEC_HOME);
        focus_rail();
    } else if (platform::back_exits_app()) {
        g_quit = true;
    }
}

void frame(float dt) {
    using namespace ui;
    Screen* s = top();
    bool full = s && s->fullscreen();
    player::State ps = player::state();
    bool video_playing = player::has_video() && (ps == player::PLAYING || ps == player::BUFFERING || ps == player::OPENING);
    ambient::begin(full && video_playing);

    // The GamePad remote: the window is the TV's, black unless the screen on top draws a video there
    // (tv_scene); everything else is drawn into the GamePad's own picture.
    pad_views::begin_frame(s && s->tv_scene());
    const bool pad_scene = pad_views::pad_scene();
    const bool own_tv = pad_scene && s && s->tv_scene();
    pad_views::set_tv_scene(own_tv);
    if (pad_scene) pad_views::clear_pad_scene();
    pad_views::PadPass pad_all(pad_scene && !own_tv);

    if (!s || !s->draws_background()) draw_background();
    bool prompting = screens::prompt_active();
    bool menu = screens::menu_active();
    if (menu && !prompting && ui::input().pressed_(BTN_B)) {
        ui::input().eat(BTN_B);  // B closes the menu, not the screen under it
        screens::close_menu();
    }
    if (prompting) {
        screens::osk::input(ui::input());  // the app's own keyboard takes what it uses, then nothing else sees it
        ui::input().eat_all();
        ui::input().eat(BTN_B);
        suspend_nav();
    }

    // Screen content with a slide/fade transition.
    const bool still = personal::reduce_motion();  // a quick fade, nothing sliding
    float tt = (float)(time() - g_trans_start) / (still ? 0.15f : 0.32f);
    float e = anim::ease_out_cubic(tt);
    bool transitioning = tt < 1.0f;
    if (transitioning) {
        gfx::push_alpha(std::min(1.0f, tt * 1.6f));
        gfx::push_transform(1.0f, 0, 0, still ? 0.0f : (1 - e) * 48.0f * g_trans_dir, 0);
    }
    if (s) s->frame();
    if (transitioning) {
        gfx::pop_transform();
        gfx::pop_alpha();
    }
    std::unique_ptr<pad_views::PadPass> pad_rest;  // what's left of the frame is the GamePad's, over its controls
    if (own_tv) pad_rest = std::make_unique<pad_views::PadPass>(true);

    if (!full) {
        mini_player::draw();
        draw_rail();
        if (!focus_in_group(g_rail_group)) draw_status();
    } else {
        mini_player::update_hidden();
    }
    if (g_focus_rail_request && !full) {
        set_focus(rail_id(g_section));
        g_focus_rail_request = false;
    }
    if (!prompting && !menu && pad_views::available() && ui::input().pressed_(BTN_MINUS)) {
        ui::input().eat(BTN_MINUS);
        pad_views::open_drawer();
    }
    if (!prompting && !menu) handle_back();
    screens::draw_menu();
    screens::draw_prompt();
    std::string skipped = player::take_skip_notice();
    if (!skipped.empty()) toast(skipped, ic::FAST_FORWARD);
    std::string quality = player::take_quality_notice();
    if (!quality.empty()) toast(quality, ic::HD);
    // Home, with nothing over it: the birthday celebration may play, and the idle mug may show.
    {
        const bool home_now = s && !full && g_stack.size() == 1 && s->section() == SEC_HOME && !menu && !prompting;
        static bool was_home = false;
        static std::set<std::string> celebrated;  // profiles asked about this run
        if (home_now && !was_home && !celebrate::active() && !celebrated.count(store::active_profile())) {
            celebrated.insert(store::active_profile());
            if (birthday::celebrate_now() && birthday::style() != birthday::STYLE_NONE)
            {
                celebrate::start(birthday::style(), greeting::birthday_line());
                audio::play(audio::SFX_SPARKLE);
            }
        }
        was_home = home_now;
        if (celebrate::active() && !celebrate::preview() && !home_now) celebrate::stop();  // something else came up
        celebrate::draw(dt);
        idle_mug::draw(dt, home_now && !celebrate::active());
    }
    draw_overlays();
    ambient::draw();
    (void)dt;
}

// Where the main thread's time goes, for the log: each frame is timed phase by phase. With
// developer updates on, a summary every 10 s, and the worst frame of each second that had one
// that took long. Also each thread's CPU use every 10 s, logged while something plays (always
// with developer updates on).
enum Phase { PH_INPUT, PH_CALLBACKS, PH_IMAGES, PH_PLAYER, PH_SCREEN, PH_DRAWING, PH_PRESENT, PH_AFTER, PH_COUNT };
// With developer updates on: a line of the heartbeat's this often all the time (core/heartbeat.hpp).
constexpr double HEARTBEAT_PERIOD = 10.0;
const char* const PHASE_NAMES[PH_COUNT] = {"input", "callbacks", "images", "player", "screen", "drawing", "present", "after"};
constexpr double SLOW_FRAME = 0.05;

// A main-thread callback's name for the log. Scope::run's are named after what the code that
// started the work runs when it's done (the third of Scope::run's template arguments).
std::string callback_name(const std::type_info& type) {
    std::string name = util::type_name(type);
    const char* run = "tasks::Scope::run<";
    size_t from = name.find(run);
    if (from == std::string::npos) return name;
    int depth = 0, arg = 0;
    size_t start = from + strlen(run), i = start;
    for (; i < name.size(); i++) {
        char c = name[i];
        if (c == '<' || c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == '>' || c == ')' || c == ']' || c == '}') {
            if (depth-- == 0) break;
        } else if (c == ',' && depth == 0) {
            if (arg == 2) break;
            arg++;
            start = i + 1;
        }
    }
    return arg == 2 && i < name.size() ? util::trim(name.substr(start, i - start)) : name;
}

class FrameProfile {
public:
    // The first phase takes in the time since the last one ended too (this class's own logging,
    // the loop around): every moment is in some frame, so a gap the frames don't explain can't
    // hide between them.
    void start() {
        if (!mark_) mark_ = SDL_GetPerformanceCounter();
    }

    // Phase p just ended.
    void mark(Phase p) {
        Uint64 t = SDL_GetPerformanceCounter();
        frame_.phases[p] = (double)(t - mark_) / SDL_GetPerformanceFrequency();
        mark_ = t;
        heartbeat::phase_done(PHASE_NAMES[p]);
        if (p == PH_CALLBACKS) frame_.callback = tasks::last_pump_slowest();
    }

    void end() {
        frame_.text = text::take_work();
        text_.glyphs += frame_.text.glyphs;
        text_.fonts += frame_.text.fonts;
        text_.glyph_seconds += frame_.text.glyph_seconds;
        double total = 0;
        for (int p = 0; p < PH_COUNT; p++) {
            total += frame_.phases[p];
            sum_[p] += frame_.phases[p];
        }
        frame_.total = total;
        // The main thread's own CPU time over the frame (developer only): a slow frame that ran
        // little was waiting, one that ran all of it was working.
        frame_.cpu = -1;
        if (developer_) {
            const uint64_t ran = platform::thread_cpu_ns(platform::current_thread());
            if (ran && cpu_mark_ && ran >= cpu_mark_) frame_.cpu = (double)(ran - cpu_mark_) / 1e9;
            cpu_mark_ = ran;
        } else {
            cpu_mark_ = 0;
        }
        frames_++;
        heartbeat::frame_done();
        if (total > 0.025) late_++;  // missed at least one screen update
        longest_ = std::max(longest_, total);
        double now = util::now_seconds();
        if (since_ == 0) reset(now);
        if (developer_ && total > SLOW_FRAME) {
            if (slow_count_++ == 0) slow_since_ = now;
            if (total > worst_.total) {
                worst_ = frame_;
                Screen* s = top();
                worst_.screen = s ? &typeid(*s) : nullptr;
            }
        }
        if (slow_count_ && now - slow_since_ >= 1) log_worst();
        if (now - since_ >= 10) summary(now);
    }

private:
    struct Frame {
        double phases[PH_COUNT] = {};
        double total = 0;
        double cpu = -1;  // the main thread's CPU time in it; -1 unknown
        tasks::Slowest callback;
        text::Work text;
        const std::type_info* screen = nullptr;
    };

    // "; text: 40 new glyphs in 120 ms (slowest 30 ms), 2 fonts opened in 15 ms"
    static std::string text_work(const text::Work& w, bool slowest) {
        if (!w.glyphs && !w.fonts) return "";
        std::string out = util::fmt("; text: %d new glyphs in %.0f ms", w.glyphs, w.glyph_seconds * 1e3);
        if (slowest && w.glyphs > 1) out += util::fmt(" (slowest %.1f ms)", w.slowest_glyph * 1e3);
        if (w.fonts) out += util::fmt(", %d fonts opened in %.0f ms", w.fonts, w.font_seconds * 1e3);
        return out;
    }

    // "screen 3.1, present 12.9 ms": the phases at least `min` long, times `scale`.
    static std::string breakdown(const double* t, double scale, double min) {
        std::string out;
        for (int p = 0; p < PH_COUNT; p++) {
            if (t[p] * scale < min) continue;
            if (!out.empty()) out += ", ";
            out += util::fmt("%s %.1f", PHASE_NAMES[p], t[p] * scale * 1e3);
        }
        return out + " ms";
    }

    static std::string screen_name(const std::type_info* type) { return type ? util::type_name(*type) : "no screen"; }

    void log_worst() {
        std::string callback, others;
        if (worst_.callback.type && worst_.callback.seconds > 0.005)
            callback = util::fmt("; slowest callback %.0f ms: %s", worst_.callback.seconds * 1e3,
                                 callback_name(*worst_.callback.type).substr(0, 600).c_str());
        if (slow_count_ > 1) others = util::fmt(" (%d slow frames in this second)", slow_count_);
        const std::string ran = worst_.cpu >= 0 ? util::fmt("; the main thread ran %.0f ms of it", worst_.cpu * 1e3) : "";
        log_message(LOG_WARNING, "Frames", "Slow frame: %.0f ms (%s) on %s%s%s%s%s", worst_.total * 1e3,
                    breakdown(worst_.phases, 1, 0.001).c_str(), screen_name(worst_.screen).c_str(), callback.c_str(),
                    text_work(worst_.text, true).c_str(), ran.c_str(), others.c_str());
        worst_ = {};
        slow_count_ = 0;
    }

    void summary(double now) {
        std::string use = cpu::report();
        player::State ps = player::state();
        if (!use.empty() && (developer_ || ps == player::PLAYING || ps == player::BUFFERING))
            log_message(LOG_OK, "CPU", "Share of one core by thread: %s", use.c_str());
        if (developer_ && summaries_++ % 6 == 0)  // every minute: enough to work out the units
            for (const std::string& line : cpu::clock_debug()) log_message(LOG_DEBUG, "Clocks", "%s", line.c_str());
        if (developer_) {
            Screen* s = top();
            log_message(LOG_OK, "Frames", "%d in %.0f s, %d late (longest %.0f ms); on average %s; on %s%s", frames_,
                        now - since_, late_, longest_ * 1e3, breakdown(sum_, 1.0 / frames_, 0.00005).c_str(),
                        screen_name(s ? &typeid(*s) : nullptr).c_str(), text_work(text_, false).c_str());
        }
        reset(now);
    }

    void reset(double now) {
        developer_ = updater::developer();
        heartbeat::keep(developer_ ? HEARTBEAT_PERIOD : 0, player::http_io_report);
        blackbox::keep(developer_, platform::data_dir());
        since_ = now;
        std::fill(sum_, sum_ + PH_COUNT, 0.0);
        frames_ = late_ = 0;
        longest_ = 0;
        text_ = {};
    }

    Uint64 mark_ = 0;
    uint64_t cpu_mark_ = 0;
    Frame frame_, worst_;
    text::Work text_;  // over the 10 s
    double sum_[PH_COUNT] = {};
    int frames_ = 0, late_ = 0, slow_count_ = 0, summaries_ = 0;
    double longest_ = 0, since_ = 0, slow_since_ = 0;
    bool developer_ = false;
};

FrameProfile g_profile;

}  // namespace

float Screen::content_x() { return RAIL_W + 36.0f; }

void push(std::unique_ptr<Screen> s) {
    if (!s) return;
    g_saved_focus.resize(g_stack.size());
    if (!g_stack.empty()) g_saved_focus.back() = ui::focused();
    g_stack.push_back(std::move(s));
    ui::reset_focus();
    start_transition(1);
    g_stack.back()->on_enter();
}

void pop() {
    if (g_stack.size() <= 1) return;
    g_dead.push_back(std::move(g_stack.back()));
    g_stack.pop_back();
    ui::reset_focus();
    if (g_saved_focus.size() >= g_stack.size() && g_saved_focus[g_stack.size() - 1])
        ui::set_focus(g_saved_focus[g_stack.size() - 1]);
    g_saved_focus.resize(g_stack.size() - 1);
    start_transition(-1);
    g_stack.back()->on_enter();
}

bool section_allowed(Section s) {
    if (!store::kids_profile()) return true;
    return s == SEC_HOME || s == SEC_YOUTUBE;  // what a kids profile may open
}

bool section_visible(Section s) {
    if (!section_allowed(s)) return false;
    return !personal::hidden(s);
}

void request_grownups() {
    // The way out of a kids profile: the grown-ups' code, then the profiles to choose from.
    auto proceed = [] {
        if (!store::kids_profile()) {
            push(screens::make_profile_picker(false));
            return;
        }
        // From a kids profile: choose another profile, or give this one a YouTube account (some videos only play signed in).
        screens::show_menu(tr("Grown-ups"), tr("You are in a kids profile"),
                           {{tr("Switch profile"), ic::PEOPLE, [] { push(screens::make_profile_picker(false)); }, ""},
                            {yt_account::signed_in() ? tr("YouTube account for this profile") : tr("Sign in to YouTube for this profile"),
                             ic::SMART_DISPLAY, [] { screens::yt::account_menu(); }, tr("Some videos only play with an account")}});
    };
    if (lock::is_set()) push(screens::make_lock_prompt(proceed));
    else proceed();
}

void open_section(Section s) {
    if (!section_visible(s)) s = SEC_HOME;
    personal::note_section(s);
    for (auto& sc : g_stack) g_dead.push_back(std::move(sc));
    g_stack.clear();
    g_saved_focus.clear();
    g_section = s;
    g_stack.push_back(screens::make_section_root(s));
    ui::reset_focus();
    start_transition(1);
    g_stack.back()->on_enter();
}

void use_profile(const std::string& id, bool go_home) {
    if (id != store::active_profile()) {
        player::close();  // what the last profile was watching doesn't play on
        stats::flush();   // its activity goes to it, not to the next
        store::switch_profile(id);
        yt_account::reload();
        jellyfin::reload();
    } else {
        store::switch_profile(id);  // remembered as the one used last
    }
    sound_settings::apply();  // a kids profile has quieter interface sounds
    if (go_home) open_section(personal::start_section());  // the page this profile chose to open to
}

Screen* top() { return g_stack.empty() ? nullptr : g_stack.back().get(); }
bool rail_focused() { return ui::focus_in_group(g_rail_group); }
void focus_rail() { g_focus_rail_request = true; }
void quit() { g_quit = true; }
void set_mini_player_visible(bool v) { g_mini_visible = v; }

namespace {

// From the time the app is asked to close (or decides to), it has this long before it's ended by force.
// Closing normally takes well under two seconds.
constexpr double CLOSE_DEADLINE = 10;

bool g_returned = false;  // the console gave the app back; the main loop opens the video again

// HOME was pressed, or the console is going to sleep (Wii U). The main thread now waits for the
// console to give the app back. A video is closed, hardware decoder and video memory too, and not
// just paused: twice the console froze at this moment with a video open (the log ends right after
// the release callbacks), and this runs before SDL lets go of its graphics memory, which a video
// texture may still be using. It is opened again, at the same second, when the app is back.
// Music and radio play on.
void on_leaving_foreground() {
    if (player::suspend_video()) {
        const bool closed = player::wait_closed(2000);
        log_message(closed ? LOG_OK : LOG_WARNING, "App", closed ? "The video is closed while the app isn't in front"
                                                               : "The video is still closing");
    }
    dev_log::flush_now();
}

void on_returned() { g_returned = true; }

// Closed from the HOME menu (Wii U). The player's decoder and downloads are let go of now, while
// the system is still waiting on the app, and not after the app has told it that it's done: a decoder
// that never returned from closing there is what a frozen console looked like. Should anything
// still hang from here on, the watchdog ends the process.
void on_exiting() {
    exit_watchdog::arm(CLOSE_DEADLINE);
    g_quit = true;
    player::close();
    bool closed = player::wait_closed(2000);
    log_message(closed ? LOG_OK : LOG_WARNING, "App", closed ? "The player has closed" : "The player is still closing");
    dev_log::flush_now();
}

// A line in the log, sent at once, before each step of closing: after a freeze the last one says
// which step it was.
struct Closing {
    double start = util::now_seconds();
    void step(const char* what) const {
        log_message(LOG_OK, "App", "Closing: %s (%.0f ms in)", what, (util::now_seconds() - start) * 1000);
        dev_log::flush_now();
    }
};

}  // namespace

int run(int, char**) {
    if (!platform::init()) return 1;
    crash_report::install(platform::data_dir());
    platform::Lifecycle lifecycle;
    lifecycle.leaving_foreground = on_leaving_foreground;
    lifecycle.returned = on_returned;
    lifecycle.exiting = on_exiting;
    platform::set_lifecycle(lifecycle);
    log_to_file(platform::data_dir());
    log_message(LOG_OK, "App", "CoffeeFlix starting on %s", platform::name());
    crash_report::report_last_run(platform::data_dir());
    blackbox::report_last_run(platform::data_dir());
    cpu::ThreadTag cpu_tag("main");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        log_message(LOG_ERROR, "App", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    // Everything is laid out at 1280x720 (the logical size below). The Wii U draws it at the TV's
    // size when that's 1080p, and SDL makes the GamePad a 1280x720 copy.
    int win_w = 1280, win_h = 720;
    SDL_DisplayMode tv;
    if (platform::is_wiiu() && SDL_GetDesktopDisplayMode(0, &tv) == 0 && tv.h >= 1080) {
        win_w = tv.w;
        win_h = tv.h;
    }
    {  // COFFEEFLIX_WINDOW=1158x540: a desktop window the shape of a phone's screen (with COFFEEFLIX_PHONE=1)
        int w = 0, h = 0;
        if (sscanf(util::env_or("COFFEEFLIX_WINDOW", "").c_str(), "%dx%d", &w, &h) == 2 && w > 200 && h > 200) { win_w = w; win_h = h; }
    }
    SDL_Window* window = SDL_CreateWindow("CoffeeFlix", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                                          platform::is_wiiu() ? 0 : SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!window || !renderer) {
        log_message(LOG_ERROR, "App", "Window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    {  // a phone's screen is wider than 16:9 and small: lay out at its shape, 540 high (a finger needs more than
       // a TV's 720 gives, every control 4/3 as big), no wider than the layouts were made for; a tablet gets less zoom
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
        if (platform::is_phone() && ow > 0 && oh > 0) {
            const double aspect = (double)ow / oh;
            const int ch = (int)std::lround(std::clamp(1100.0 / aspect, 540.0, 720.0));
            ui::W = (float)std::lround(ch * aspect);
            ui::H = (float)ch;
        }
        log_message(LOG_OK, "App", "Laid out at %.0fx%.0f on a %dx%d screen", ui::W, ui::H, ow, oh);
    }
    SDL_RenderSetLogicalSize(renderer, (int)ui::W, (int)ui::H);
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0) log_message(LOG_OK, "App", "Renderer: %s, drawing at %dx%d", info.name, win_w, win_h);
    platform::post_video_init(window, renderer);

    std::string content = platform::content_dir();
    gfx::init(renderer);
    pad_views::init(renderer);
    if (!text::init(content)) log_message(LOG_ERROR, "App", "Fonts missing in %s", content.c_str());
    store::load(platform::data_dir() + "/coffeeflix.json");
    store::set_user(platform::user_id());
    lock::init(platform::data_dir());
    i18n::init(content);
    http::init(content + "/cacert.pem", platform::tune_socket);
    http::set_verify_tls(store::get_bool("verify_tls", true));
    tasks::init();
    images::init(platform::is_wiiu() ? (80u << 20) : (256u << 20));
    audio::init();
    player::init();
    sound_settings::apply();
    ui::init();
    ui::set_accent((int)store::get_int("accent", 0));
    g_rail_group = ui::id("rail");
    updater::init();
#ifdef __ANDROID__
    {  // a coffeeflix.json copied over from the Wii U (Receive files, or adb push into the media folder)
        const auto imported = settings_import::run();
        if (imported.found) ui::toast(imported.summary, ic::CHECK_CIRCLE);
    }
#endif

    stats::app_opened();
    open_section(personal::start_section());
    if (store::has_other_profiles()) {
        // Left in a kids profile, the app starts in it: restarting is no way out of it.
        const std::string last = store::last_profile();
        if (store::profile(last).kids) use_profile(last, true);
        else push(screens::make_profile_picker(true));  // who's watching
    }

    // For the screenshot tests: a screen to open at once.
    if (const char* open = getenv("COFFEEFLIX_OPEN")) {
        if (!strcmp(open, "lock-setup")) push(screens::make_lock_setup(nullptr));
        else if (!strcmp(open, "lock-prompt")) push(screens::make_lock_prompt([] {}));
        else if (!strcmp(open, "activity")) push(screens::make_activity(""));
        else if (!strncmp(open, "sleep:", 6)) { sleep_timer::start(atoi(open + 6)); screens::show_sleep_timer_menu(); }
        else if (!strncmp(open, "screentime:", 11)) push(screens::make_screen_time(open + 11));
        else if (!strcmp(open, "updates")) push(screens::make_update());
        else if (!strcmp(open, "jellyfin")) push(screens::make_jellyfin());
        else if (!strncmp(open, "photos:", 7)) {  // the viewer on the pictures of a folder
            std::vector<std::string> paths;
            if (DIR* d = opendir(open + 7)) {
                while (dirent* de = readdir(d)) {
                    const std::string ext = util::file_extension(de->d_name);
                    if (ext == "jpg" || ext == "jpeg" || ext == "png") paths.push_back(util::join_path(open + 7, de->d_name));
                }
                closedir(d);
            }
            std::sort(paths.begin(), paths.end());
            push(screens::make_photo_viewer(std::move(paths), 0));
        }
        else if (!strncmp(open, "media:", 6)) push(screens::make_media_folder(open + 6, "Photos"));
        else if (!strncmp(open, "video:", 6)) {  // a video file, remembered like My Media's
            player::Source src;
            src.url = src.id = open + 6;
            src.service = "local";
            src.title = util::file_name(open + 6);
            screens::play_video(src);
        }
        else if (!strncmp(open, "yt-file:", 8)) {  // a file played as if it were a YouTube video (the GamePad remote's layout)
            player::Source src;
            src.url = open + 8;
            src.service = "youtube";
            src.id = "testvideo01";
            src.channel_id = "UCtestchannelxxxxxxxxxxx";
            src.title = getenv("COFFEEFLIX_TITLE") ? getenv("COFFEEFLIX_TITLE") : "A video with a title";
            src.subtitle = "Some Channel Name";
            src.remember_position = false;
            screens::play_video(src);
        }
        else if (!strncmp(open, "audio:", 6)) {  // a song from a file, playing quietly
            player::Source src;
            src.url = open + 6;
            src.service = "local";
            src.id = open + 6;
            src.title = util::file_name(open + 6);
            src.subtitle = "Test artist";
            src.remember_position = false;
            src.live = getenv("COFFEEFLIX_LIVE") != nullptr;   // a station: no length, no place to ask again from
            screens::play_audio_queue({src}, 0, false);
        }
        else if (!strncmp(open, "settings:", 9)) push(screens::make_settings_page(atoi(open + 9)));
        else if (!strncmp(open, "channel:", 8)) push(screens::yt::make_channel(open + 8, "Test channel"));
        else if (!strncmp(open, "play:", 5)) {
            youtube::Video v;
            v.id = open + 5;
            v.title = "Test video";
            screens::yt::play(v);
        }
    }

    Input in;
    RawInput raw;
    Uint64 last = SDL_GetPerformanceCounter();
    while (platform::running() && !g_quit) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        if (platform::fixed_dt() > 0) dt = platform::fixed_dt();

        g_profile.start();
        platform::poll(raw);
        input_update(in, raw, dt);
        pad_views::filter_input(in);
        g_profile.mark(PH_INPUT);
        tasks::pump();
        g_profile.mark(PH_CALLBACKS);
        updater::tick();
        stats::tick();
        check_screen_time();
        dev_log::tick();
        images::begin_frame();
        g_profile.mark(PH_IMAGES);
        if (g_returned) {  // after the console's callbacks, SDL's among them, have all run
            g_returned = false;
            player::resume_video();
        }
        player::update();
        {   // the sleep timer: stops what plays, with the sound fading out before, or at the end of this one
            const bool ended = player::state() == player::ENDED;
            if (!ended) sleep_timer::item_left();
            if (sleep_timer::update(dt, ended)) {
                const player::State ps = player::state();
                if (ps == player::PLAYING || ps == player::BUFFERING) {
                    if (player::live()) player::close();
                    else player::set_paused(true);
                }
                ui::toast(tr("Sleep timer ended. Good night!"), ic::SCHEDULE);
            }
            audio::set_media_fade(sleep_timer::fade());
        }
        {
            // Keep the screen on for videos and the console on for any playback.
            player::State ps = player::state();
            bool playing = ps == player::PLAYING || ps == player::BUFFERING || ps == player::OPENING;
            platform::keep_awake(!playing ? (top() && top()->prevents_sleep() ? platform::AWAKE_NO_POWEROFF : platform::AWAKE_NONE)
                                 : player::has_video() ? platform::AWAKE_FULL
                                                       : platform::AWAKE_NO_POWEROFF);
        }
        g_profile.mark(PH_PLAYER);

        gfx::begin_frame(gfx::BLACK);
        ui::begin_frame(in, dt);
        frame(dt);
        ui::end_frame();
        g_profile.mark(PH_SCREEN);
        gfx::end_frame();
        pad_views::update();  // the GamePad's picture and outputs, as this frame left them
        g_profile.mark(PH_DRAWING);

        platform::draw_pad_preview(renderer);
        if (const char* shot = platform::screenshot_request()) platform::save_screenshot(renderer, shot);
        platform::record_frame(renderer);
        SDL_RenderPresent(renderer);
        g_profile.mark(PH_PRESENT);
        g_dead.clear();
        store::tick();
        g_profile.mark(PH_AFTER);
        g_profile.end();
    }

    log_message(LOG_OK, "App", "Shutting down");
    exit_watchdog::arm(CLOSE_DEADLINE);
    const Closing closing;
    closing.step("updater and mini player");
    updater::stop();
    mini_player::shutdown();
    g_dead.clear();
    closing.step("screens");
    g_stack.clear();
    closing.step("player");
    player::shutdown();
    g_stack.clear();
    closing.step("saving");
    stats::flush();
    store::save_now();
    closing.step("tasks");
    http::cancel_running();  // the workers may be waiting for a slow server
    tasks::shutdown();
    closing.step("sound");
    audio::shutdown();
    closing.step("pictures, text and graphics");
    pad_views::shutdown();
    images::shutdown();
    text::shutdown();
    gfx::shutdown();
    closing.step("log and network");
    dev_log::shutdown();
    http::shutdown();
    IMG_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    updater::finish_on_exit();  // nothing reads bundled files any more
    blackbox::stop();
    log_shutdown();
    platform::shutdown();
    return 0;
}

}  // namespace app
