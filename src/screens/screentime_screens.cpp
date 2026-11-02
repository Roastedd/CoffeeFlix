// Screen time: the grown-ups' page to set it for a kids profile, and the "that's all for today" screen a kids profile
// meets when the time (or quiet hours) is up, which only the grown-ups' code gets past. See core/screentime.
#include <algorithm>
#include <cmath>

#include "app/app.hpp"
#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/lock.hpp"
#include "core/screentime.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

bool g_time_up = false;  // the screen below is up

// "30 min", "1.5 h".
std::string minutes_text(int minutes) {
    if (minutes <= 0) return tr("Off");
    if (minutes < 60) return util::fmt(tr("%d min"), minutes);
    const int h10 = minutes * 10 / 60;  // tenths of an hour
    return util::fmt(tr("%s h"), h10 % 10 ? util::fmt("%d.%d", h10 / 10, h10 % 10).c_str() : util::fmt("%d", h10 / 10).c_str());
}

const int LIMITS[] = {0, 30, 45, 60, 90, 120, 180, 240};

int next_limit(int minutes) {
    for (size_t i = 0; i < sizeof(LIMITS) / sizeof(LIMITS[0]); i++)
        if (LIMITS[i] == minutes) return LIMITS[(i + 1) % (sizeof(LIMITS) / sizeof(LIMITS[0]))];
    return LIMITS[0];
}

// A time of day, moved on by half an hour within a range (round to the end of it).
int step_time(int m, int from, int to) {
    m += 30;
    if (m > to) m = from;
    return m;
}

// --- the grown-ups' page ---------------------------------------------------------------------

class ScreenTimeScreen : public app::Screen {
public:
    explicit ScreenTimeScreen(std::string profile) : profile_(std::move(profile)) {}
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("screentime");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Screen time"), t.text);
        y += 74;
        const store::Profile who = store::profile(profile_);
        text::draw_fit(font::body, x0, y, w, who.name.empty() ? tr("Kids profile") : who.name, t.text2);
        y += 50;
        screentime::Rules r = screentime::rules(profile_);
        auto row = [&](const char* key, const std::string& label, const std::string& value, int icon, bool first = false, bool info = false) {
            Id iid = id(g, key);
            const bool hit = value_row(iid, Rect(x0, y, w, 64), label.c_str(), value.c_str(), icon, g, (first ? F_DEFAULT : 0) | F_WRAP | (info ? F_INFO : 0));
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
            return hit;
        };
        const screentime::Today today = screentime::today(profile_);
        row("today", tr("Today"),
            today.limit_minutes > 0 ? util::fmt(tr("%s of %s"), minutes_text((int)std::lround(today.used / 60)).c_str(), minutes_text(today.limit_minutes + today.extra_minutes).c_str())
                                    : (today.used > 0 ? minutes_text((int)std::lround(today.used / 60)) + "  \xC2\xB7  " + tr("No limit") : std::string(tr("No limit"))),
            ic::SCHEDULE, true, true);
        bool changed = false;
        if (row("week", tr("School days"), minutes_text(r.weekday_minutes), ic::SCHEDULE)) {
            r.weekday_minutes = next_limit(r.weekday_minutes);
            changed = true;
        }
        if (row("weekend", tr("Weekends"), minutes_text(r.weekend_minutes), ic::SCHEDULE)) {
            r.weekend_minutes = next_limit(r.weekend_minutes);
            changed = true;
        }
        Id qid = id(g, "quiet");
        bool quiet = r.quiet;
        if (toggle_row(qid, Rect(x0, y, w, 82), tr("Quiet time"), tr("Nothing plays between these times"), &quiet, g, F_WRAP)) {
            r.quiet = quiet;
            changed = true;
        }
        if (focused() == qid) page_.focus_range(y + page_.scroll() - 60, y + 82 + page_.scroll() + 20);
        y += 92;
        if (r.quiet) {
            if (row("from", tr("Starts"), screentime::clock_text(r.quiet_from), ic::SCHEDULE)) {
                r.quiet_from = step_time(r.quiet_from, 17 * 60, 23 * 60 + 30);
                changed = true;
            }
            if (row("to", tr("Ends"), screentime::clock_text(r.quiet_to), ic::SCHEDULE)) {
                r.quiet_to = step_time(r.quiet_to, 5 * 60, 10 * 60);
                changed = true;
            }
        }
        if (changed) screentime::set_rules(profile_, r);
        if (row("more", tr("Give more time today"), "", ic::ADD)) {
            show_menu(tr("Give more time today"), who.name,
                      {{util::fmt(tr("%d more minutes"), 15), ic::ADD, [p = profile_] { grant(p, 15); }, ""},
                       {util::fmt(tr("%d more minutes"), 30), ic::ADD, [p = profile_] { grant(p, 30); }, ""},
                       {tr("1 more hour"), ic::ADD, [p = profile_] { grant(p, 60); }, ""}});
        }
        if (row("reset", tr("Start today over"), "", ic::REFRESH)) screentime::reset_today(profile_);
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Change")}, {"B", tr("Back")}});
    }

private:
    static void grant(const std::string& profile, int minutes) {
        screentime::add_extra(profile, minutes);
        toast(util::fmt(tr("%d more minutes"), minutes), ic::CHECK_CIRCLE);
    }

    std::string profile_;
    Page page_;
};

// --- "that's all for today" -------------------------------------------------------------------

class TimeUpScreen : public app::Screen {
public:
    TimeUpScreen(bool quiet, int quiet_ends) : quiet_(quiet), ends_(quiet_ends) {
        g_time_up = true;
        player::close();  // nothing goes on playing behind it
        audio::play(audio::SFX_OPEN);
    }
    ~TimeUpScreen() override { g_time_up = false; }

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }
    app::Section section() const override { return app::SEC_NONE; }
    bool on_back() override { return true; }  // nowhere to go back to

    void frame() override {
        const Theme& t = theme();
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        const float cy = 250;
        gfx::glow(Rect(W * 0.5f - 260, cy - 200, 520, 400), t.accent.alpha(0.25f));
        gfx::fill_circle(W * 0.5f, cy, 66, t.accent.alpha(0.9f));
        text::icon(ic::SCHEDULE, 70, W * 0.5f, cy, gfx::rgb(0x1A1016));
        text::draw_fit(font::display, W * 0.5f, cy + 100, W - 128, quiet_ ? tr("Quiet time") : tr("That's all for today"), t.text, text::CENTER);
        const std::string sub = quiet_ ? util::fmt(tr("Back at %s."), screentime::clock_text(ends_).c_str()) : std::string(tr("You've had your time for today. See you tomorrow!"));
        text::draw_fit(font::body, W * 0.5f, cy + 168, W - 160, sub, t.text2, text::CENTER);
        Id g = id("timeup");
        const float bw = 360;
        if (button(id(g, "ask"), Rect((W - bw) * 0.5f, cy + 240, bw, 56), tr("Ask a grown-up"), ic::LOCK, BTN_PRIMARY, g, F_DEFAULT)) ask();
        hint_bar({{"A", tr("Select")}});
    }

private:
    void ask() {
        auto proceed = [this] {
            const std::string profile = store::active_profile();
            std::vector<MenuItem> items;
            if (quiet_) {
                items.push_back({util::fmt(tr("%d more minutes"), 30), ic::ADD, [this, profile] { give_quiet(profile, 30); }, ""});
                items.push_back({tr("1 more hour"), ic::ADD, [this, profile] { give_quiet(profile, 60); }, ""});
            } else {
                items.push_back({util::fmt(tr("%d more minutes"), 15), ic::ADD, [this, profile] { give(profile, 15); }, ""});
                items.push_back({util::fmt(tr("%d more minutes"), 30), ic::ADD, [this, profile] { give(profile, 30); }, ""});
                items.push_back({tr("1 more hour"), ic::ADD, [this, profile] { give(profile, 60); }, ""});
            }
            items.push_back({tr("Switch profile"), ic::PEOPLE, [] { app::push(make_profile_picker(false)); }, ""});
            show_menu(tr("Grown-ups"), "", std::move(items));
        };
        if (lock::is_set()) app::push(make_lock_prompt(proceed));
        else proceed();
    }
    void give(const std::string& profile, int minutes) {
        screentime::add_extra(profile, minutes);
        toast(util::fmt(tr("%d more minutes"), minutes), ic::CHECK_CIRCLE);
        app::pop();
    }
    void give_quiet(const std::string& profile, int minutes) {
        screentime::allow_quiet_for(profile, minutes);
        toast(minutes >= 60 ? std::string(tr("1 more hour")) : util::fmt(tr("%d more minutes"), minutes), ic::CHECK_CIRCLE);
        app::pop();
    }

    bool quiet_;
    int ends_;
};

}  // namespace

std::unique_ptr<app::Screen> make_screen_time(const std::string& profile) { return std::make_unique<ScreenTimeScreen>(profile); }
std::unique_ptr<app::Screen> make_time_up(bool quiet, int quiet_ends) { return std::make_unique<TimeUpScreen>(quiet, quiet_ends); }
bool time_up_active() { return g_time_up; }

}  // namespace screens
