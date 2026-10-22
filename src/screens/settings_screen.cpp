// Settings: appearance, playback quality, region, network, accounts, about.
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "app/birthday.hpp"
#include "app/celebrate.hpp"
#include "app/greeting.hpp"
#include "app/idle_mug.hpp"
#include "app/ambient.hpp"
#include "app/personal.hpp"
#include "app/photo_frame.hpp"
#include "app/sound_settings.hpp"
#include "app/updater.hpp"
#include "core/settings_import.hpp"
#include "core/speedtest.hpp"
#include "core/stats.hpp"
#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/lock.hpp"
#include "services/kids.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/send_logs.hpp"
#include "screens/widgets.hpp"
#include "services/jellyfin.hpp"
#include "services/navidrome.hpp"
#include "services/yt_account.hpp"
#include "services/yt_recs.hpp"
#include "screens/youtube_common.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

struct Choice {
    const char* key;
    std::vector<int> values;
    std::vector<const char*> labels;
    int def;
};

// 0: Auto, up to 1080p as the downloads allow (player::auto_budget, auto_cap). 720p and 1080p
// with the most pictures a second to show (player::quality_fps): on the Wii U 1080p60 at about 45-48
// (its decoder can't show them all) or at 30, every other picture.
const Choice YT_QUALITY{"yt_quality",
                        {0, 360, 480, 72030, 72060, 108030, 108060},
                        {N_("Auto"), "360p", "480p", "720p 30 fps", "720p 60 fps", "1080p 30 fps", "1080p 60 fps"},
                        0};
// Auto: as much as the downloads carry (the Jellyfin server converts heavier files down to it).
const Choice JF_QUALITY{"jf_quality", {0, 480, 720, 1080}, {N_("Auto"), "480p", "720p", "1080p"}, 0};
const Choice TW_QUALITY{"twitch_quality", {0, 360, 480, 720, 1080}, {N_("Auto"), "360p", "480p", "720p", "1080p"}, 0};
// Wii U hardware H.264 decoding: 0 every picture, 1 without pictures nothing refers to (most
// B-frames; fewer frames per second but what the original FFmpeg-wiiu did), 2 software only. The
// player steps down by itself when the hardware decoder gives no pictures.
const Choice VIDEO_DECODING{"video_decoding", {0, 1, 2}, {N_("Hardware"), N_("Hardware, fewer frames"), N_("Software")}, 0};
// Android: the system's MediaCodec (0) or software only (2). A video the hardware gives no pictures for
// plays in software by itself.
const Choice VIDEO_DECODING_ANDROID{"video_decoding", {0, 2}, {N_("Hardware"), N_("Software")}, 0};
// Developers: video downloads at once (player/http_io.cpp; 0: its default).
const Choice DEV_CONNECTIONS{"dev_connections", {0, 2, 4, 6}, {"Default (3)", "2", "4", "6"}, 0};
// Where the sound plays (app/pad_views.cpp): the Wii U can send it to the TV, the GamePad, or both.
const Choice SOUND_OUTPUT{"sound_output", {0, 1, 2}, {N_("TV and GamePad"), N_("TV only"), N_("GamePad only")}, 0};
// The GamePad remote (app/pad_views.cpp): its screen goes to sleep after this long without a touch or
// button while a video plays on the TV; any button wakes it.
const Choice PAD_SLEEP{"pad_sleep", {0, 120, 300, 600}, {N_("Off"), N_("After 2 min"), N_("After 5 min"), N_("After 10 min")}, 0};
// The greeting above Home: off, the time of day with the name, or friendly lines that vary (app/greeting.cpp).
const Choice GREETINGS{"greeting_style", {0, 1, 2}, {N_("Off"), N_("Simple"), N_("Playful")}, 1};
// What the screensaver shows: the clock, or a slideshow of the photos on the card (app/photo_frame.cpp).
const Choice SCREENSAVER_STYLE{"screensaver_style", {0, 1}, {N_("Clock"), N_("Photos")}, 0};
const Choice FRAME_SOURCE{"frame_source", {0, 1}, {N_("The Photos folder"), N_("Anywhere on the card")}, 0};
const Choice FRAME_SECONDS{"frame_seconds", {8, 12, 20, 30}, {"8 s", "12 s", "20 s", "30 s"}, 12};
const Choice SCREENSAVER{"screensaver", {0, 120, 300, 600, 1200}, {N_("Off"), N_("After 2 min"), N_("After 5 min"), N_("After 10 min"), N_("After 20 min")}, 300};
const char* COUNTRIES[] = {"US", "GB", "CA", "AU", "IE", "DE", "FR", "ES", "IT", "NL", "SE", "PL", "BR", "MX", "JP", "KR", "IN"};

// Settings > Language: the console's language, or one chosen here.
class LanguageScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("language");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Language"), t.text);
        y += 90;
        std::string chosen = i18n::chosen();
        auto row = [&](const char* code, const char* label, const char* value) {
            Id iid = id(g, code);
            bool on = chosen == code;
            if (value_row(iid, Rect(x0, y, w, 64), label, value, on ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED, g,
                          on ? F_DEFAULT : 0) &&
                !on)
                i18n::choose(code);
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        };
        std::string same = util::fmt(tr("Same as the Wii U (%s)"), i18n::system().name);
        row("", same.c_str(), nullptr);
        for (int i = 0; i < i18n::LANGUAGE_COUNT; i++) {
            const i18n::Language& l = i18n::LANGUAGES[i];
            const char* english = tr(l.english);
            row(l.code, l.name, strcmp(english, l.name) ? english : nullptr);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

// The channels kids profiles may watch: well-known ones to add, and each one to take off. (A channel's own page
// has "Allow for kids" for any other.)
void kids_channels_menu() {
    std::vector<MenuItem> items;
    items.push_back({tr("Add suggested channels"), ic::ADD, [] {
                         const int n = kids::add_suggested();
                         toast(n ? util::fmt(tr("%d channels added"), n) : std::string(tr("They are all there already")), ic::CHECK_CIRCLE);
                     }, tr("PBS Kids, Sesame Street, Bluey and other well-known children's channels")});
    items.push_back({tr("Add suggested podcasts and radio"), ic::ADD, [] {
                         const int n = kids::add_suggested_audio();
                         toast(n ? util::fmt(tr("%d added"), n) : std::string(tr("They are all there already")), ic::CHECK_CIRCLE);
                     }, tr("Children's podcasts and radio stations, checked when they were chosen")});
    for (const kids::Channel& c : kids::channels())
        items.push_back({util::fmt(tr("Remove %s"), c.name.empty() ? c.id.c_str() : c.name.c_str()), ic::DELETE,
                         [id = c.id] { kids::remove(id); }, ""});
    for (const kids::Podcast& p : kids::podcasts())
        items.push_back({util::fmt(tr("Remove %s"), p.name.c_str()), ic::DELETE, [u = p.feed_url] { kids::remove_podcast(u); }, tr("Podcast")});
    for (const kids::Station& s : kids::stations())
        items.push_back({util::fmt(tr("Remove %s"), s.name.c_str()), ic::DELETE, [u = s.url] { kids::remove_station(u); }, tr("Radio")});
    const size_t n = kids::channels().size();
    show_menu(tr("Channels for kids"), n ? util::fmt(tr("%zu channels"), n) : std::string(tr("None yet: add some, or open a channel and choose Allow for kids.")),
              std::move(items));
}

// The connection speed test: it runs on a thread of its own and keeps its answer here, so leaving Settings
// and coming back shows it.
struct SpeedTestState {
    std::mutex m;
    bool running = false, have = false;
    speedtest::Result result;
    std::atomic<bool> cancel{false};
};
SpeedTestState& speed_state() {
    static SpeedTestState* s = new SpeedTestState;  // never destroyed: the thread may still be writing as the app ends
    return *s;
}
void start_speed_test() {
    SpeedTestState& st = speed_state();
    {
        std::lock_guard<std::mutex> lock(st.m);
        if (st.running) return;
        st.running = true;
        st.cancel = false;
    }
    // The developer computer: found by an update check this run, or the one remembered from an earlier run.
    std::string dev = updater::developer() ? updater::dev_server() : std::string();
    if (dev.empty() && updater::developer()) dev = store::get_str("update_dev_server");
    std::thread([dev] {
        SpeedTestState& st = speed_state();
        speedtest::Result r = speedtest::run(dev, &st.cancel);
        for (const auto& leg : r.legs) {
            std::string steps;
            for (const auto& st : leg.steps) steps += util::fmt(" %d: %.0f KB/s (%.0f ms);", st.connections, st.kb, st.loaded_ms);
            log_message(leg.ok ? LOG_OK : LOG_WARNING, "SpeedTest", "%s: %s, first connection: lookup %.0f ms, connect %.0f ms, secure %.0f ms (later ones %.0f ms), first byte %.0f ms; new connection %.0f ms, small request %.0f ms idle and %.0f ms at the best step, best %d connections (%.0f KB/s);%s",
                        leg.name.c_str(), leg.ok ? "ok" : leg.error.c_str(), leg.dns_ms, leg.tcp_ms, leg.tls_ms, leg.later_tls_ms, leg.wait_ms, leg.first_byte_ms, leg.idle_ms, leg.best_loaded_ms, leg.best_connections, leg.best_kb, steps.c_str());
        }
        std::lock_guard<std::mutex> lock(st.m);
        st.result = std::move(r);
        st.have = true;
        st.running = false;
    }).detach();
}
std::string speed_text(double kb) { return kb >= 1000 ? util::fmt("%.1f MB/s", kb / 1024) : util::fmt("%.0f KB/s", kb); }

// Settings > Make it yours > Start screen: the page CoffeeFlix opens to.
class StartPageScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("startpage");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Start screen"), t.text);
        y += 74;
        text::draw(font::body, x0, y, tr("Where CoffeeFlix opens."), t.text2);
        y += 50;
        const std::string chosen = personal::start_id();
        auto row = [&](const char* key, const std::string& label) {
            Id iid = id(g, key);
            const bool on = chosen == key;
            if (value_row(iid, Rect(x0, y, w, 64), label.c_str(), nullptr, on ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED, g,
                          (on ? F_DEFAULT : 0) | F_WRAP) &&
                !on)
                personal::set_start_id(key);
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        };
        row("home", tr("Home"));
        row(personal::START_LAST, tr("Last page I used"));
        for (size_t i = 0; i < personal::SECTION_COUNT; i++) {
            const personal::SectionInfo& s = personal::SECTIONS[i];
            if (!s.can_start || s.section == app::SEC_HOME || !app::section_visible(s.section)) continue;
            row(s.id, tr(s.label));
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

// Settings > Make it yours > Birthday: the month and the day, and how it is celebrated.
class BirthdayScreen : public app::Screen {
public:
    explicit BirthdayScreen(std::string profile) : profile_(std::move(profile)) {
        const birthday::Date d = birthday::get(profile_);
        month_ = d.month;
        day_ = d.day;
    }
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("birthday");
        const bool mine = profile_.empty() || profile_ == store::active_profile();
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Birthday"), t.text);
        y += 74;
        const store::Profile who = store::profile(mine ? store::active_profile() : profile_);
        text::draw_fit(font::body, x0, y, w, mine || who.name.empty() ? tr("Stays on this console.") : who.name, t.text2);
        y += 50;
        auto header = [&](const char* s) {
            text::draw(font::small_bold, x0 + 6, y + 10, s, t.accent);
            y += 44;
        };
        header(tr("MONTH"));
        const float cw = 170, ch = 46, gap = 12;
        for (int m = 1; m <= 12; m++) {
            const int col = (m - 1) % 4, row = (m - 1) / 4;
            Id cid = id(g, util::fmt("m%d", m).c_str());
            if (chip(cid, Rect(x0 + col * (cw + gap), y + row * (ch + gap), cw, ch), i18n::month_name(m - 1), month_ == m, g)) pick_month(m);
            if (focused() == cid) page_.focus_range(y + row * (ch + gap) + page_.scroll() - 60, y + row * (ch + gap) + ch + page_.scroll() + 20);
        }
        y += 3 * (ch + gap) + 14;
        header(tr("DAY"));
        const float dw = 96, dh = 44, dgap = 10;
        const int days = month_ ? birthday::days_in_month(month_) : 31;
        for (int d = 1; d <= days; d++) {
            const int col = (d - 1) % 7, row = (d - 1) / 7;
            Id cid = id(g, util::fmt("d%d", d).c_str());
            if (chip(cid, Rect(x0 + col * (dw + dgap), y + row * (dh + dgap), dw, dh), std::to_string(d).c_str(), day_ == d, g)) pick_day(d);
            if (focused() == cid) page_.focus_range(y + row * (dh + dgap) + page_.scroll() - 60, y + row * (dh + dgap) + dh + page_.scroll() + 20);
        }
        y += 5 * (dh + dgap) + 14;
        text::draw_wrapped(font::small, Rect(x0 + 6, y, w - 12, 44), tr("Born on February 29? It's celebrated on February 28 in other years."), t.text3, 2);
        y += 56;
        if (mine) {
            const char* styles[] = {N_("None"), N_("Balloons"), N_("Sparklers"), N_("Fireworks")};
            Id sid = id(g, "style");
            if (value_row(sid, Rect(x0, y, w, 64), tr("Celebration"), tr(styles[(int)birthday::style()]), ic::STAR, g, F_WRAP))
                birthday::set_style((birthday::Style)(((int)birthday::style() + 1) % 4));
            if (focused() == sid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
            Id eid = id(g, "every");
            bool every = birthday::every_open();
            if (toggle_row(eid, Rect(x0, y, w, 82), tr("Celebrate every time I open CoffeeFlix"), tr("Otherwise only the first time that day"), &every, g, F_WRAP))
                birthday::set_every_open(every);
            if (focused() == eid) page_.focus_range(y + page_.scroll() - 60, y + 82 + page_.scroll() + 20);
            y += 92;
            Id pid = id(g, "preview");
            if (value_row(pid, Rect(x0, y, w, 64), tr("Preview"), "", ic::PLAY_CIRCLE, g, F_WRAP)) {
                celebrate::start(birthday::style(), greeting::birthday_line(), true);
                app::pop();  // back to the list, where it plays over the page
            }
            if (focused() == pid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        }
        if (birthday::get(profile_).set()) {
            Id rid = id(g, "remove");
            if (value_row(rid, Rect(x0, y, w, 64), tr("Remove birthday"), "", ic::DELETE, g, F_WRAP)) {
                birthday::set(profile_, 0, 0);
                month_ = day_ = 0;
            }
            if (focused() == rid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void save() {
        if (month_ && day_) birthday::set(profile_, month_, day_);
    }
    void pick_month(int m) {
        month_ = m;
        if (day_ > birthday::days_in_month(m)) day_ = birthday::days_in_month(m);
        save();
    }
    void pick_day(int d) {
        day_ = d;
        save();
    }

    std::string profile_;
    int month_ = 0, day_ = 0;
    Page page_;
};

// Settings > Make it yours > Your activity: what a profile watched and listened to, in round numbers.
class ActivityScreen : public app::Screen {
public:
    explicit ActivityScreen(std::string profile) : profile_(std::move(profile)) {}
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("activity");
        const bool mine = profile_.empty() || profile_ == store::active_profile();
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Your activity"), t.text);
        y += 74;
        const stats::Report r = stats::report(profile_);
        if (!mine) {
            const store::Profile p = store::profile(profile_);
            text::draw(font::body, x0, y, p.name, t.text2);
            y += 50;
        } else if (!r.since.empty()) {
            text::draw(font::body, x0, y, util::fmt(tr("Since %s"), i18n::long_date(r.since).c_str()), t.text2);
            y += 50;
        }
        auto row = [&](const char* key, const std::string& label, const std::string& value, bool first = false) {
            Id iid = id(g, key);
            value_row(iid, Rect(x0, y, w, 64), label.c_str(), value.c_str(), 0, g, (first ? F_DEFAULT : 0) | F_WRAP | F_INFO);
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        };
        auto header = [&](const char* s) {
            text::draw(font::small_bold, x0 + 6, y + 10, s, t.accent);
            y += 44;
        };
        if (!r.any) {
            text::draw(font::body, x0, y, tr("Nothing yet. It appears as you watch and listen."), t.text2);
            y += 60;
        } else {
            row("week", tr("This week"), stats::rounded(r.week.seconds), true);
            row("month", tr("This month"), stats::rounded(r.month.seconds));
            row("all", tr("All time"), stats::rounded(r.all.seconds));
            row("played", tr("Played: this week, this month, in all"), util::fmt("%d  \xC2\xB7  %d  \xC2\xB7  %d", r.week.watched, r.month.watched, r.all.watched));
            row("opened", tr("Times CoffeeFlix was opened"), std::to_string(r.opened));
            if (!r.top.empty()) {
                y += 10;
                header(tr("MOST WATCHED"));
                int i = 0;
                for (const stats::Title& tl : r.top) {
                    std::string v = stats::rounded(tl.seconds);
                    if (tl.plays > 0) v += util::fmt("  \xC2\xB7  %d\xC3\x97", tl.plays);
                    row(util::fmt("top%d", i++).c_str(), tl.title, v);
                }
            }
            if (!r.services.empty()) {
                y += 10;
                header(tr("WHERE THE TIME WENT THIS MONTH"));
                int i = 0;
                for (const stats::Share& s : r.services) {
                    if (i >= 5) break;
                    row(util::fmt("svc%d", i++).c_str(), stats::label(s.service), stats::rounded(s.seconds));
                }
            }
        }
        if (mine) {
            y += 10;
            bool_row("record", tr("Record activity"), tr("Stays on this console. Nothing is sent anywhere."), g, x0, w, y);
            Id rid = id(g, "reset");
            if (value_row(rid, Rect(x0, y, w, 64), tr("Reset activity"), "", ic::DELETE, g, F_WRAP))
                show_menu(tr("Forget everything recorded?"), tr("It can't be brought back."),
                          {{tr("Cancel"), ic::CLOSE, [] {}}, {tr("Reset activity"), ic::DELETE, [] { stats::reset(); }}});
            if (focused() == rid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"B", tr("Back")}});
    }

private:
    void bool_row(const char* key, const char* label, const char* desc, Id g, float x0, float w, float& y) {
        Id iid = id(g, key);
        bool v = stats::enabled();
        if (toggle_row(iid, Rect(x0, y, w, 82), label, desc, &v, g, F_WRAP)) stats::set_enabled(v);
        if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 82 + page_.scroll() + 20);
        y += 92;
    }

    std::string profile_;
    Page page_;
};

// Settings > Make it yours > Side menu: which services show. Home and Settings always do.
class SideMenuScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("sidemenu");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Side menu"), t.text);
        y += 74;
        text::draw(font::body, x0, y, tr("Hidden services also leave Home and Search."), t.text2);
        y += 50;
        bool first = true;
        for (size_t i = 0; i < personal::SECTION_COUNT; i++) {
            const personal::SectionInfo& s = personal::SECTIONS[i];
            if (!s.can_hide) continue;
            Id iid = id(g, s.id);
            bool shown = !personal::hidden(s.section);
            if (toggle_row(iid, Rect(x0, y, w, 64), tr(s.label), nullptr, &shown, g, (first ? F_DEFAULT : 0) | F_WRAP))
                personal::set_hidden(s.section, !shown);
            first = false;
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        }
        y += 10;
        Id rid = id(g, "reset");
        const int n = personal::hidden_count();
        if (value_row(rid, Rect(x0, y, w, 64), tr("Show everything again"), n ? util::fmt(tr("%d turned off"), n).c_str() : "", ic::RESTORE, g, F_WRAP))
            personal::show_all();
        if (focused() == rid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
        y += 74;
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Change")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

// Developers: every interface sound on its own, the held D-pad, and the two sets side by side. Not translated.
class SoundTestScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("soundtest");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Sound test", t.text);
        y += 74;
        text::draw(font::body, x0, y, "A plays each sound at the level chosen in Settings", t.text2);
        y += 50;
        auto row = [&](const char* key, const char* label, const char* value, bool first = false) {
            Id iid = id(g, key);
            const bool hit = value_row(iid, Rect(x0, y, w, 64), label, value, ic::PLAY_CIRCLE, g, (first ? F_DEFAULT : 0) | F_WRAP | F_SILENT);
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
            return hit;
        };
        const sound_settings::Mode m = sound_settings::mode();
        if (row("style", "Set", m == sound_settings::OFF ? "Off" : m == sound_settings::CLASSIC ? "Classic" : "Subtle", true))
            sound_settings::set_mode((sound_settings::Mode)(((int)m + 1) % 3));
        struct S { const char* key; const char* label; audio::Sfx sfx; };
        const S sounds[] = {{"move", "Move", audio::SFX_MOVE}, {"select", "Select", audio::SFX_SELECT}, {"back", "Back", audio::SFX_BACK},
                            {"bump", "Edge", audio::SFX_BUMP}, {"on", "Switch on", audio::SFX_TOGGLE}, {"off", "Switch off", audio::SFX_TOGGLE_OFF},
                            {"error", "Error", audio::SFX_ERROR}, {"open", "Open", audio::SFX_OPEN}, {"sparkle", "Birthday", audio::SFX_SPARKLE},
                            {"shuffle", "Surprise me", audio::SFX_SHUFFLE}};
        for (const S& s : sounds)
            if (row(s.key, s.label, "")) audio::play(s.sfx);
        // The D-pad held: ten ticks a second for three seconds, as it would be.
        if (row("held", "Held D-pad: ten ticks a second for three seconds", held_ > 0 ? "playing" : "")) held_ = 3.0;
        if (held_ > 0) {
            held_ -= dt();
            held_tick_ -= dt();
            if (held_tick_ <= 0) {
                audio::play(audio::SFX_MOVE);
                held_tick_ = 0.1;
            }
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"B", "Back"}});
    }

private:
    double held_ = 0, held_tick_ = 0;
    Page page_;
};

class SettingsScreen : public app::Screen {
public:
    // Settings is a list of pages (ROOT), and each page has its own rows.
    enum Cat { ROOT, GENERAL, PERSONAL, PLAYBACK, YOUTUBE, PROFILES, ACCOUNTS, NETWORK, ABOUT };
    explicit SettingsScreen(Cat cat = ROOT) : cat_(cat) {}
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        float w = 760;
        Id g = id("settings");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, cat_ == ROOT ? tr("Settings") : tr(CATEGORIES[cat_].label), t.text);
        y += 90;

        auto header = [&](const char* s) {
            text::draw(font::small_bold, x0 + 6, y + 10, s, t.accent);
            y += 44;
        };
        auto row_h = [&](float h) {
            float top = y;
            y += h + 10;
            return top;
        };
        auto track = [&](Id iid, float top, float h) {
            if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + h + page_.scroll() + 20);
        };

        const Cat cat = cat_;
        auto in = [&](Cat c) { return cat == c; };
        if (cat == ROOT) {  // the list of pages
            for (int c = GENERAL; c <= ABOUT; c++) {
                Id iid = id(g, CATEGORIES[c].key);
                float top = row_h(72);
                const std::string summary = category_summary((Cat)c);
                if (value_row(iid, Rect(x0, top, w, 72), tr(CATEGORIES[c].label), summary.c_str(), CATEGORIES[c].icon, g, (c == GENERAL ? F_DEFAULT : 0) | F_WRAP))
                    app::push(std::make_unique<SettingsScreen>((Cat)c));
                track(iid, top, 72);
            }
            page_.end(y + page_.scroll());
            hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
            return;
        }

        if (in(GENERAL)) {
            header(tr("LANGUAGE"));
            {
                Id iid = id(g, "language");
                float top = row_h(64);
                // Also in English, for whoever chose a language they can't read.
                std::string label = tr("Language");
                if (label != "Language") label += " (Language)";
                if (value_row(iid, Rect(x0, top, w, 64), label.c_str(), i18n::current().name, ic::TRANSLATE, g, F_DEFAULT))
                    app::push(std::make_unique<LanguageScreen>());
                track(iid, top, 64);
            }

            header(tr("APPEARANCE"));
            {
                Id iid = id(g, "accent");
                float top = row_h(64);
                int a = (int)store::get_int("accent", 0);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Accent color"), accent_name(a), ic::PALETTE, g)) {
                    a = (a + 1) % accent_count();
                    store::set_int("accent", a);
                    set_accent(a);
                }
                // Swatches
                for (int i = 0; i < accent_count(); i++) {
                    float sx = x0 + w + 30 + i * 34;
                    set_accent(i);
                    Color c1 = theme().accent, c2 = theme().accent2;
                    set_accent(a);
                    gfx::fill_rrect_hgrad(Rect(sx, top + 18, 26, 26), 13, c1, c2);
                    if (i == a) gfx::stroke_circle(sx + 13, top + 31, 17, 2, gfx::WHITE);
                }
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "sounds");
                float top = row_h(64);
                const sound_settings::Mode m = sound_settings::mode();
                const char* names[] = {N_("Subtle"), N_("Classic"), N_("Off")};
                if (value_row(iid, Rect(x0, top, w, 64), tr("Interface sounds"), tr(names[(int)m]), ic::VOLUME_UP, g)) {
                    sound_settings::set_mode((sound_settings::Mode)(((int)m + 1) % 3));
                    sound_settings::preview();
                }
                track(iid, top, 64);
            }
            if (sound_settings::mode() != sound_settings::OFF) {
                {
                    Id iid = id(g, "sfxvolume");
                    float top = row_h(64);
                    float v = sound_settings::volume();
                    if (slider_row(iid, Rect(x0, top, w, 64), tr("Interface sounds volume"), &v, 0.05f, g)) {
                        sound_settings::set_volume(v);
                        sound_settings::preview();
                    }
                    track(iid, top, 64);
                }
                bool_row(g, "sfxticks", "sfx_ticks", true, tr("Tick when the selector moves"), tr("A soft tick as you move around; the other sounds stay"), x0, w, y);
                sound_settings::apply();  // the switch above only wrote the store
            }
            if (platform::has_pad_outputs()) choice_row(g, "soundout", SOUND_OUTPUT, tr("Sound plays on"), ic::VOLUME_UP, x0, w, y);
            if (platform::has_pad_outputs()) choice_row(g, "padsleep", PAD_SLEEP, tr("GamePad screen sleeps"), ic::GAMEPAD, x0, w, y);
            choice_row(g, "saver", SCREENSAVER, tr("Screensaver"), ic::BRIGHTNESS, x0, w, y);
            choice_row(g, "saverstyle", SCREENSAVER_STYLE, tr("Screensaver shows"), ic::IMAGE, x0, w, y);
            if (photo_frame::wanted()) {
                choice_row(g, "framesrc", FRAME_SOURCE, tr("Photos from"), ic::FOLDER, x0, w, y);
                choice_row(g, "frametime", FRAME_SECONDS, tr("Photo changes every"), ic::SCHEDULE, x0, w, y);
                bool_row(g, "frameclock", "frame_clock", true, tr("Show the clock"), tr("Small, in a corner of the photo"), x0, w, y);
            }
            {
                Id iid = id(g, "frameshow");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Show the photo frame now"), "", ic::PLAY_CIRCLE, g)) ambient::show_now();
                track(iid, top, 64);
            }

            header(tr("REGION"));
            {
                Id iid = id(g, "country");
                float top = row_h(64);
                std::string cc = store::get_str("radio_country", "US");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Country"), cc.c_str(), ic::PUBLIC, g)) {
                    size_t n = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]), i = 0;
                    while (i < n && cc != COUNTRIES[i]) i++;
                    cc = COUNTRIES[(i + 1) % n];
                    store::set_str("radio_country", cc);
                    store::set_str("yt_region", cc);
                }
                track(iid, top, 64);
            }

        }
        if (in(PERSONAL)) {
            {
                Id iid = id(g, "name");
                float top = row_h(64);
                const std::string id_now = store::active_profile();
                const store::Profile p = store::profile(id_now);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Your name"), p.name.empty() ? tr("Not set") : p.name.c_str(), ic::EDIT, g, F_DEFAULT))
                    prompt_text(tr("Your name"), p.name, tr("Your name"), [id_now](std::string v) {
                        v = store::clean_profile_name(v);
                        if (!v.empty()) store::rename_profile(id_now, v);
                    });
                track(iid, top, 64);
            }
            choice_row(g, "greet", GREETINGS, tr("Greetings"), ic::PERSON, x0, w, y);
            {
                Id iid = id(g, "birthday");
                float top = row_h(64);
                const birthday::Date d = birthday::get();
                if (value_row(iid, Rect(x0, top, w, 64), tr("Birthday"), d.set() ? birthday::long_text(d).c_str() : tr("Not set"), ic::STAR, g))
                    app::push(std::make_unique<BirthdayScreen>(""));
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "start");
                float top = row_h(64);
                std::string v;
                const std::string sid = personal::start_id();
                if (sid == personal::START_LAST) v = tr("Last page I used");
                else if (const personal::SectionInfo* s = personal::info_for_id(sid)) {
                    v = tr(s->label);
                    if (!app::section_visible(s->section)) v = util::fmt(tr("%s (turned off)"), v.c_str());
                }
                if (value_row(iid, Rect(x0, top, w, 64), tr("Start screen"), v.c_str(), ic::HOME, g)) app::push(std::make_unique<StartPageScreen>());
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "sidemenu");
                float top = row_h(64);
                const int n = personal::hidden_count();
                if (value_row(iid, Rect(x0, top, w, 64), tr("Side menu"), n ? util::fmt(tr("%d turned off"), n).c_str() : tr("Everything"), ic::TUNE, g))
                    app::push(std::make_unique<SideMenuScreen>());
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "activity");
                float top = row_h(64);
                const stats::Report r = stats::report();
                if (value_row(iid, Rect(x0, top, w, 64), tr("Your activity"), r.any ? stats::rounded(r.month.seconds).c_str() : tr("Nothing yet"), ic::HISTORY, g))
                    app::push(std::make_unique<ActivityScreen>(""));
                track(iid, top, 64);
            }
            bool_row(g, "idlemug", "idle_mug", true, tr("Idle animation"), tr("A little coffee mug with steam when Home sits idle"), x0, w, y);
            bool_row(g, "reducemotion", "reduce_motion", false, tr("Reduce motion"), tr("Less sliding and fading between screens"), x0, w, y);
        }
        if (in(PLAYBACK)) {
            choice_row(g, "yt_q", YT_QUALITY, tr("YouTube quality"), ic::SMART_DISPLAY, x0, w, y);
            choice_row(g, "jf_q", JF_QUALITY, tr("Jellyfin quality"), ic::VIDEO_LIBRARY, x0, w, y);
            bool_row(g, "jfautoplay", "jf_autoplay", true, tr("Jellyfin autoplay"),
                     tr("Play the next episode, with a countdown you can cancel"), x0, w, y);
            choice_row(g, "tw_q", TW_QUALITY, tr("Twitch quality"), ic::LIVE_TV, x0, w, y);
            if (platform::is_wiiu()) choice_row(g, "vdec", VIDEO_DECODING, tr("Video decoding"), ic::TUNE, x0, w, y);
            if (platform::is_android()) choice_row(g, "vdec", VIDEO_DECODING_ANDROID, tr("Video decoding"), ic::TUNE, x0, w, y);
            if (updater::developer())  // not translated: developers only
                choice_row(g, "conns", DEV_CONNECTIONS, "Video downloads at once", ic::TUNE, x0, w, y);
            if (updater::developer() && platform::is_wiiu())
                bool_row(g, "bicubic", "dev_bicubic", false, "Sharper video scaling",
                         "Video filtered bicubic instead of bilinear: sharper where it's scaled up", x0, w, y);
            {
                Id iid = id(g, "mediavolume");
                float top = row_h(64);
                float v = sound_settings::media_volume();
                if (slider_row(iid, Rect(x0, top, w, 64), tr("Volume"), &v, 0.05f, g)) sound_settings::set_media_volume(v);
                track(iid, top, 64);
            }
            if (updater::developer() && platform::is_wiiu()) {  // not translated: developers only
                bool_row(g, "netstop", "dev_honour_net_stop", false, "Stop the network when the system asks",
                         "When the console goes to sleep or to the HOME menu: ends downloads and waits (takes effect at once)", x0, w, y);
                bool_row(g, "noack", "dev_noackdelay", false, "Acknowledge at once",
                         "Sockets don't delay their acknowledgements (applies from the next start)", x0, w, y);
            }
            bool_row(g, "subs", "subs_default_on", true, tr("Subtitles on by default"), tr("When a video comes with subtitles"), x0, w, y);
        }
        if (in(YOUTUBE)) {
            {
                Id iid = id(g, "yt");
                float top = row_h(64);
                bool in = yt_account::signed_in();
                if (value_row(iid, Rect(x0, top, w, 64), "YouTube", in ? yt_account::name().c_str() : tr("Not signed in (optional)"),
                              ic::SMART_DISPLAY, g))
                    yt::account_menu();
                track(iid, top, 64);
            }
            bool_row(g, "ytautoplay", "yt_autoplay", true, tr("YouTube autoplay"),
                     tr("Play a suggested video next, with a countdown you can cancel"), x0, w, y);
            bool_row(g, "yttidy", "yt_tidy_titles", true, tr("Calmer titles"),
                     tr("Titles written in capitals are shown in normal letters, and !!! becomes !"), x0, w, y);
            bool_row(g, "ytframes", "yt_frames", false, tr("Video frames as thumbnails"),
                     tr("A picture from the video itself, not the one the uploader picked"), x0, w, y);
            bool_row(g, "ytryd", "yt_ryd", true, tr("Dislike counts"),
                     tr("From Return YouTube Dislike, which is sent the video's ID"), x0, w, y);
            if (yt_account::signed_in()) {
                bool_row(g, "ytrate", "yt_rate_buttons", true, tr("Like and dislike buttons"),
                         tr("In the video player, for videos on YouTube"), x0, w, y);
                bool_row(g, "ythist", "yt_history_sync", true, tr("Save watch history to your account"),
                         tr("Videos you watch here show in your YouTube history, with how far you got"), x0, w, y);
                Id iid = id(g, "ytcopy");
                float top = row_h(64);
                size_t n = yt::local_subscriptions().size();
                bool copying = yt::copying_subscriptions();
                std::string v = copying ? tr("Adding\xE2\x80\xA6") : n ? util::fmt(tr("%zu channels"), n) : tr("All on your account");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Add this console's subscriptions to your account"), v.c_str(),
                              ic::CLOUD_UPLOAD, g) && n && !copying)
                    yt::copy_subscriptions_to_account();
                track(iid, top, 64);
            }
            bool_row(g, "ytcc", "yt_captions", false, tr("YouTube captions"), tr("Turn captions on automatically (your language first)"), x0, w, y);
            bool_row(g, "ytlocal", "yt_local_bias", true, tr("Fewer videos from other countries"),
                     tr("In YouTube's popular lists, pushes down videos made for another country's audience unless you watch them"), x0, w, y);
            bool_row(g, "ytsb", "yt_sponsorblock", true, tr("Skip sponsors on YouTube"),
                     tr("SponsorBlock: jumps over sponsor segments, self-promotion and subscribe reminders"), x0, w, y);

            {
                Id iid = id(g, "recs");
                float top = row_h(64);
                bool learned = yt_recs::has_profile();
                if (value_row(iid, Rect(x0, top, w, 64), tr("Reset YouTube recommendations"), learned ? tr("Learning") : tr("Nothing yet"),
                              ic::AUTO_AWESOME, g) && learned) {
                    yt_recs::reset();
                    toast(tr("YouTube recommendations reset"), ic::DELETE);
                }
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "ytimport");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Import YouTube subscriptions"), tr("Takeout or NewPipe file"), ic::FILE_DOWNLOAD, g))
                    toast(yt::import_subscriptions(), ic::SUBSCRIPTIONS);
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "ytexport");
                float top = row_h(64);
                size_t n = yt::subscriptions().size();
                std::string v = n ? util::fmt(tr("%zu channels"), n) : tr("None yet");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Export YouTube subscriptions"), v.c_str(), ic::FILE_UPLOAD, g) && n)
                    toast(yt::export_subscriptions(), ic::SUBSCRIPTIONS);
                track(iid, top, 64);
            }

        }
        if (in(PROFILES)) {
            {
                Id iid = id(g, "profiles");
                float top = row_h(64);
                const store::Profile p = store::profile(store::active_profile());
                const std::string shown = !p.name.empty() ? p.name : std::string(tr("Main"));
                if (value_row(iid, Rect(x0, top, w, 64), tr("Profiles"), shown.c_str(), ic::PEOPLE, g))
                    app::push(make_profile_picker(false));
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "kidschannels");
                float top = row_h(64);
                const size_t n = kids::channels().size();
                const std::string v = n ? util::fmt(tr("%zu channels"), n) : std::string(tr("None"));
                if (value_row(iid, Rect(x0, top, w, 64), tr("Channels for kids"), v.c_str(), ic::SMART_DISPLAY, g)) kids_channels_menu();
                track(iid, top, 64);
            }
            {
                bool_row(g, "kidspod", "kids_show_podcasts", true, tr("Podcasts on kids profiles"),
                     tr("Their Home shows the podcasts you allowed"), x0, w, y);
            bool_row(g, "kidsradio", "kids_show_radio", true, tr("Radio on kids profiles"),
                     tr("Their Home shows the stations you allowed"), x0, w, y);
            Id iid = id(g, "lock");
                float top = row_h(64);
                const lock::Kind k = lock::kind();
                const char* v = k == lock::Kind::PIN ? tr("PIN") : k == lock::Kind::BUTTONS ? tr("Button code") : tr("Not set");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Grown-ups' code"), v, ic::LOCK, g)) {
                    if (k == lock::Kind::NONE) {
                        app::push(make_lock_setup(nullptr));
                    } else {  // changing it, or taking it off, asks for it first
                        app::push(make_lock_prompt([] {
                            show_menu(tr("Grown-ups' code"), "",
                                      {{tr("Change code"), ic::EDIT, [] { app::push(make_lock_setup(nullptr)); }, ""},
                                       {tr("Remove code"), ic::DELETE, [] {
                                            for (const auto& p : store::profiles())
                                                if (p.kids) {
                                                    toast(tr("Change the kids profiles first: they need a code"), ic::ERROR_OUTLINE);
                                                    return;
                                                }
                                            lock::clear();
                                            toast(tr("Code removed"), ic::CHECK_CIRCLE);
                                        }, tr("Not while there are kids profiles: they need a code")}});
                        }));
                    }
                }
                track(iid, top, 64);
            }
        }
        if (in(ACCOUNTS)) {
            {
                Id iid = id(g, "jf");
                float top = row_h(64);
                const jellyfin::Account& a = jellyfin::account();
                std::string v = a.valid() ? util::fmt(tr("%s on %s"), a.user_name.c_str(),
                                                      (a.server_name.empty() ? a.server : a.server_name).c_str())
                                          : tr("Not connected");
                if (value_row(iid, Rect(x0, top, w, 64), "Jellyfin", v.c_str(), ic::VIDEO_LIBRARY, g)) jellyfin_account_menu();
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "nd");
                float top = row_h(64);
                const navidrome::Account& a = navidrome::account();
                std::string v = a.valid() ? util::fmt(tr("%s on %s"), a.user_name.c_str(),
                                                      (a.server_name.empty() ? a.server : a.server_name).c_str())
                                          : tr("Not connected");
                if (value_row(iid, Rect(x0, top, w, 64), "Navidrome", v.c_str(), ic::LIBRARY_MUSIC, g)) navidrome_account_menu();
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "history");
                float top = row_h(64);
                size_t n = store::resume_list(100).size();
                std::string v = n ? util::fmt(tr("%zu items"), n) : tr("Empty");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Clear watch history"), v.c_str(), ic::HISTORY, g) && n) {
                    store::resume_clear();
                    toast(tr("Watch history cleared"), ic::DELETE);
                }
                track(iid, top, 64);
            }
        }
#if defined(__ANDROID__) && !defined(PUBLIC_BUILD)
        if (in(ACCOUNTS)) {
            {  // (not translated: for bringing a Wii U's sources and sign-ins over)
                Id iid = id(g, "import");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), "Import sources from your Wii U", "coffeeflix.json", ic::FILE_DOWNLOAD, g)) {
                    const auto r = settings_import::run();
                    toast(r.found ? r.summary : "Send coffeeflix.json from your Wii U's SD card (wiiu/apps/coffeeflix) with Receive files, then choose this again",
                          r.found && r.imported ? ic::CHECK_CIRCLE : ic::INFO);
                }
                track(iid, top, 64);
            }
        }
#endif
        if (in(NETWORK)) {
            {
                Id iid = id(g, "tls");
                float top = row_h(82);
                bool v = store::get_bool("verify_tls", true);
                if (toggle_row(iid, Rect(x0, top, w, 82), tr("Verify secure connections"),
                               tr("Turn off only if everything fails because the console clock is wrong"), &v, g)) {
                    store::set_bool("verify_tls", v);
                    http::set_verify_tls(v);
                }
                track(iid, top, 82);
            }
            {
                Id iid = id(g, "ip");
                float top = row_h(64);
                std::string ip = platform::ip_address();
                value_row(iid, Rect(x0, top, w, 64), tr("IP address"), ip.empty() ? tr("Not connected") : ip.c_str(), ic::WIFI, g, F_INFO);
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "speedtest");
                float top = row_h(64);
                SpeedTestState& st = speed_state();
                bool running, have;
                speedtest::Result result;
                {
                    std::lock_guard<std::mutex> lock(st.m);
                    running = st.running;
                    have = st.have;
                    if (have) result = st.result;
                }
                if (value_row(iid, Rect(x0, top, w, 64), tr("Test connection speed"), running ? tr("Testing\xE2\x80\xA6") : tr("Start"), ic::WIFI, g) && !running)
                    start_speed_test();
                track(iid, top, 64);
                if (have && !running) {
                    for (const auto& leg : result.legs) {
                        Id lid = id(g, leg.name.c_str());
                        float ltop = row_h(64);
                        const std::string v = leg.ok ? util::fmt("%s  \xC2\xB7  %d  \xC2\xB7  %s", speed_text(leg.best_kb).c_str(), leg.best_connections,
                                                                     leg.idle_ms > 0 && leg.best_loaded_ms > 0 ? util::fmt("%.0f \xE2\x86\x92 %.0f ms", leg.idle_ms, leg.best_loaded_ms).c_str()
                                                                                                                 : util::fmt("%.0f ms", leg.first_byte_ms).c_str())
                                                     : std::string(tr("Didn't work"));
                        value_row(lid, Rect(x0, ltop, w, 64), tr(leg.name == "Internet" ? N_("Internet") : N_("Home network")), v.c_str(), ic::WIFI, g, F_INFO);
                        track(lid, ltop, 64);
                        if (!leg.ok) continue;
                        {
                            Id nid = id(g, (leg.name + "-new").c_str());
                            float ntop = row_h(64);
                            value_row(nid, Rect(x0, ntop, w, 64), tr("New connection: lookup, connect, secure, first byte"),
                                      util::fmt("%.0f / %.0f / %.0f / %.0f ms", leg.dns_ms, leg.tcp_ms, leg.tls_ms, leg.wait_ms).c_str(), ic::WIFI, g, F_INFO);
                            track(nid, ntop, 64);
                        }
                        Id did = id(g, (leg.name + "-steps").c_str());
                        float dtop = row_h(64);
                        std::string steps, counts;
                        for (const auto& st : leg.steps) {
                            counts += (counts.empty() ? "" : " / ") + std::to_string(st.connections);
                            steps += (steps.empty() ? "" : " / ") + speed_text(st.kb);
                        }
                        value_row(did, Rect(x0, dtop, w, 64), util::fmt(tr("%s connections"), counts.c_str()).c_str(), steps.c_str(), ic::WIFI, g, F_INFO);
                        track(did, dtop, 64);
                    }
                }
            }
        }
        if (in(ABOUT)) {
            if (updater::developer()) {  // not translated: developers only
                Id iid = id(g, "soundtest");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), "Sound test", "", ic::VOLUME_UP, g)) app::push(std::make_unique<SoundTestScreen>());
                track(iid, top, 64);
            }
            header(tr("UPDATES"));
            {
                Id iid = id(g, "update");
                float top = row_h(64);
                std::string label = tr("Check for updates"), value;
                const char* nv = updater::release().version.c_str();
                bool dev = updater::developer();
                switch (updater::supported() ? updater::state() : updater::IDLE) {
                    case updater::CHECKING: value = tr("Checking\xE2\x80\xA6"); break;
                    case updater::UP_TO_DATE: value = tr("Up to date"); break;
                    case updater::AVAILABLE:
                        label = util::fmt(dev ? tr("Install developer build %s") : tr("Update to CoffeeFlix %s"), nv);
                        value = tr("Available");
                        break;
                    case updater::DOWNLOADING:
                        label = util::fmt(dev ? tr("Downloading developer build %s") : tr("Downloading CoffeeFlix %s"), nv);
                        value = util::fmt("%d%%", (int)(updater::progress() * 100));
                        break;
                    case updater::READY:
                        label = dev ? tr("The developer build is ready") : util::fmt(tr("CoffeeFlix %s is ready"), nv);
                        value = platform::is_android() ? "Ready to install" : tr("Installs when you close");
                        break;
                    case updater::FAILED: value = tr("Didn't work"); break;
                    default: value = updater::supported() ? "" : tr("Not available here"); break;
                }
                if (value_row(iid, Rect(x0, top, w, 64), label.c_str(), value.c_str(), ic::SYSTEM_UPDATE, g))
                    app::push(make_update());
                track(iid, top, 64);
            }
            if (updater::supported() && !platform::is_android()) {  // Android: always developer builds, installed by hand
                Id iid = id(g, "autoupdate");
                float top = row_h(82);
                bool v = updater::automatic();
                if (toggle_row(iid, Rect(x0, top, w, 82), tr("Install updates automatically"),
                               tr("New versions download in the background and install when you close CoffeeFlix"), &v, g))
                    updater::set_automatic(v);
                track(iid, top, 82);
            }
            if (updater::supported() && updater::supporter_available() && !platform::is_android()) {
                Id iid = id(g, "supporter");
                float top = row_h(64);
                const bool on = updater::supporter_on();
                const std::string value = on ? (updater::supporter_message().empty() ? tr("On") : tr("Needs a look")) : tr("Off");
                if (value_row(iid, Rect(x0, top, w, 64), tr("Supporter updates"), value.c_str(), ic::FAVORITE, g)) {
                    std::string sub = on ? updater::supporter_email() : std::string(tr("Supporters get previews here"));
                    if (on && !updater::supporter_message().empty()) sub = updater::supporter_message();
                    std::vector<MenuItem> items;
                    items.push_back({tr("Email used on Ko-fi"), ic::EDIT, [] {
                        prompt_text(tr("Email used on Ko-fi"), updater::supporter_email(), tr("name@example.com"), [](std::string q) {
                            if (q.empty()) return;
                            if (!updater::set_supporter_email(q)) toast(tr("Cancel the download first."), ic::INFO);
                            else toast(tr("Looking for a supporter preview\xE2\x80\xA6"), ic::FAVORITE);
                        });
                    }, tr("Type it with the keyboard; it stays on this console and goes only to the supporter server")});
                    if (on)
                        items.push_back({tr("Turn off"), ic::CLOSE, [] {
                            if (!updater::set_supporter_email("")) toast(tr("Cancel the download first."), ic::INFO);
                        }, ""});
                    show_menu(tr("Supporter updates"), sub, std::move(items));
                }
                track(iid, top, 64);
            }
#ifdef __ANDROID__
            if (updater::supported()) {  // no secret code needed here: one switch for developer mode (not translated: developers only)
                Id iid = id(g, "devupdates");
                float top = row_h(82);
                bool v = updater::developer();
                if (toggle_row(iid, Rect(x0, top, w, 82), "Developer mode",
                               "Lola's picks, test builds from tools/dev-update-android.sh on your computer, and extra detail while playing", &v, g)) {
                    if (v) updater::set_dev_unlocked(true);
                    if (!updater::set_developer(v)) toast(tr("Cancel the download first."), ic::INFO);
                }
                track(iid, top, 82);
            }
#else
            if (updater::supported() && updater::dev_unlocked()) {
                Id iid = id(g, "devupdates");
                float top = row_h(82);
                bool v = updater::developer();
                if (toggle_row(iid, Rect(x0, top, w, 82), tr("Developer updates"),
                               platform::is_android() ? "Test builds from tools/dev-update-android.sh on your computer, instead of releases"
                                                      : tr("Test builds from tools/dev-update.sh on your computer, instead of releases"), &v, g) &&
                    !updater::set_developer(v))
                    toast(tr("Cancel the download first."), ic::INFO);
                track(iid, top, 82);
            }
#endif
            if (updater::developer() && platform::has_pad_outputs()) {  // not translated: developers only
                Id iid = id(g, "padlab");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), "GamePad lab", "Screen, picture and sound", ic::GAMEPAD, g))
                    app::push(make_gamepad_lab());
                track(iid, top, 64);
            }
            if (platform::is_android()) {  // for ADB debugging and installing apps from outside the store
                Id iid = id(g, "tvdev");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), "Fire TV developer options", "ADB debugging, unknown apps", ic::SETTINGS, g)) {
                    std::string how = platform::open_developer_options();
                    if (!how.empty()) toast(how, ic::INFO);
                }
                track(iid, top, 64);
            }
            if (updater::has_previous()) {
                Id iid = id(g, "previous");
                float top = row_h(64);
                std::string pv = updater::previous_version();
                std::string name = "CoffeeFlix " + pv;
                if (value_row(iid, Rect(x0, top, w, 64), tr("Switch to the previous version"),
                              pv.empty() ? tr("Kept from before the last update") : name.c_str(), ic::RESTORE, g)) {
                    show_menu(pv.empty() ? tr("Switch to the previous version?") : util::fmt(tr("Switch to %s?"), name.c_str()),
                              tr("Then start it again from the Wii U Menu."),
                              {{tr("Switch and close CoffeeFlix"), ic::RESTORE, [] { updater::switch_to_previous(); }},
                               {tr("Cancel"), ic::CLOSE, [] {}}});
                }
                track(iid, top, 64);
            }

            header(tr("ABOUT"));
            {
                float top = row_h(150);
                Rect r(x0, top, w, 150);
                gfx::fill_rrect(r, 16, t.surface);
                gfx::fill_rrect_hgrad(Rect(r.x + 24, r.y + 26, 56, 56), 16, t.accent, t.accent2);
                text::icon(ic::LOCAL_CAFE, 32, r.x + 52, r.y + 54, gfx::rgb(0x1A1016));
                text::draw(font::title, r.x + 100, r.y + 24, std::string("CoffeeFlix ") + updater::version(), t.text);
                text::draw(font::small, r.x + 100, r.y + 60, util::fmt(tr("%s \xC2\xB7 built %s"), platform::name(), __DATE__), t.text3);
                text::draw_wrapped(font::small, Rect(r.x + 24, r.y + 96, r.w - 48, 50),
                                   tr("Free for noncommercial use (PolyForm Noncommercial). Not affiliated with Nintendo, "
                                      "YouTube, Twitch or Jellyfin."),
                                   t.text3, 2);
                Id iid = id(g, "about");
                Item it = focusable(iid, r, g);
                focus_ring(r, 16, it.f);
                track(iid, top, 150);
            }
            {
                Id iid = id(g, "sendlogs");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Send logs"), tr("To report a problem on GitHub"), ic::FORUM, g))
                    app::push(make_send_logs());
                track(iid, top, 64);
            }
            {
                Id iid = id(g, "licenses");
                float top = row_h(64);
                if (value_row(iid, Rect(x0, top, w, 64), tr("Licenses"), tr("CoffeeFlix and the software it includes"), ic::DESCRIPTION, g))
                    app::push(make_licenses());
                track(iid, top, 64);
            }
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Change")}, {"B", tr("Back")}});
    }

private:
    struct CatInfo {
        const char* key;
        const char* label;
        int icon;
    };
    static constexpr CatInfo CATEGORIES[9] = {
        {"root", N_("Settings"), ic::SETTINGS},
        {"general", N_("General"), ic::TUNE},
        {"personal", N_("Make it yours"), ic::PALETTE},
        {"playback", N_("Playback"), ic::PLAY_CIRCLE},
        {"youtube", N_("YouTube"), ic::SMART_DISPLAY},
        {"profiles", N_("Profiles and kids"), ic::PEOPLE},
        {"accounts", N_("Servers and data"), ic::VIDEO_LIBRARY},
        {"network", N_("Network"), ic::WIFI},
        {"about", N_("Updates and about"), ic::SYSTEM_UPDATE},
    };

    // One line under each page's name, so the list says what's in it.
    std::string category_summary(Cat c) const {
        switch (c) {
            case GENERAL: return tr("Language, sounds, region");
            case PERSONAL: return tr("Name, greetings, birthday, start screen, activity");
            case PLAYBACK: return tr("Quality and subtitles");
            case YOUTUBE: return yt_account::signed_in() ? yt_account::name() : std::string(tr("Not signed in"));
            case PROFILES: {
                const store::Profile p = store::profile(store::active_profile());
                return !p.name.empty() ? p.name : std::string(tr("Main"));
            }
            case ACCOUNTS: return tr("Jellyfin, Navidrome, history");
            case NETWORK: return tr("Connection");
            case ABOUT: return tr("Updates, logs, licenses");
            default: return "";
        }
    }

    Cat cat_ = ROOT;

    void choice_row(Id g, const char* key, const Choice& c, const char* label, int icon, float x0, float w, float& y) {
        Id iid = id(g, key);
        float top = y;
        int cur = (int)store::get_int(c.key, c.def);
        size_t idx = 0;
        while (idx < c.values.size() && c.values[idx] != cur) idx++;
        if (idx >= c.values.size()) idx = 0;
        if (value_row(iid, Rect(x0, top, w, 64), label, tr(c.labels[idx]), icon, g)) {
            idx = (idx + 1) % c.values.size();
            store::set_int(c.key, c.values[idx]);
            if (!std::strcmp(c.key, "video_decoding")) player::reset_decoding_fallback();
        }
        if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + 64 + page_.scroll() + 20);
        y += 74;
    }

    void bool_row(Id g, const char* id_key, const char* key, bool def, const char* label, const char* desc, float x0, float w,
                  float& y) {
        Id iid = id(g, id_key);
        float top = y;
        bool v = store::get_bool(key, def);
        if (toggle_row(iid, Rect(x0, top, w, 82), label, desc, &v, g)) store::set_bool(key, v);
        if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + 82 + page_.scroll() + 20);
        y += 92;
    }

    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_settings() { return std::make_unique<SettingsScreen>(); }
std::unique_ptr<app::Screen> make_activity(const std::string& profile) { return std::make_unique<ActivityScreen>(profile); }
std::unique_ptr<app::Screen> make_birthday(const std::string& profile) { return std::make_unique<BirthdayScreen>(profile); }
std::unique_ptr<app::Screen> make_settings_page(int page) {
    return std::make_unique<SettingsScreen>(page >= 1 && page <= 8 ? (SettingsScreen::Cat)page : SettingsScreen::ROOT);
}

}  // namespace screens
