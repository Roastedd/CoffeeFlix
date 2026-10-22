#include "app/greeting.hpp"

#include <algorithm>
#include <ctime>
#include <set>

#include "app/birthday.hpp"
#include "app/personal.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace greeting {

namespace {

constexpr const char* STYLE_KEY = "greeting_style";
constexpr const char* LAST_KEY = "greeting_last";       // the line said last, so it isn't said again
constexpr const char* SEEN_KEY = "last_seen_day";        // the day number the profile was here last
constexpr int AWAY_DAYS = 7;                              // away this long: "Welcome back"
constexpr double REPICK_SECONDS = 3600;
constexpr double STILL_MIN = 0.05, STILL_MAX = 0.90;      // how far into it, to be worth asking about
constexpr int64_t STILL_RECENT = 21 * 24 * 3600;

// Whole sentences, so each language can word them its own way (and put the name where it belongs); a
// line is said in the first form when the person has a name, else in the second.
struct Phrase {
    const char* key;
    const char* with_name;
    const char* without_name;
    int band;  // the time of day it fits, or -1 for any
};

// Simple: the time of day (the old greeting, with a name after it).
const Phrase SIMPLE_LINES[BAND_COUNT] = {
    {"simple-late", N_("Up late, {name}?"), N_("Up late?"), LATE_NIGHT},
    {"simple-morning", N_("Good morning, {name}"), N_("Good morning"), MORNING},
    {"simple-afternoon", N_("Good afternoon, {name}"), N_("Good afternoon"), AFTERNOON},
    {"simple-evening", N_("Good evening, {name}"), N_("Good evening"), EVENING},
};

// Playful: a few for any time of day and one for each part of it.
const Phrase PLAYFUL_LINES[] = {
    {"how", N_("How's it going, {name}?"), N_("How's it going?"), -1},
    {"see-you", N_("Good to see you, {name}"), N_("Good to see you"), -1},
    {"watching", N_("What are we watching, {name}?"), N_("What are we watching?"), -1},
    {"rise", N_("Rise and shine, {name}"), N_("Rise and shine"), MORNING},
    {"break", N_("Afternoon break, {name}?"), N_("Afternoon break?"), AFTERNOON},
    {"unwind", N_("Time to unwind, {name}"), N_("Time to unwind"), EVENING},
    {"still-up", N_("Still up, {name}?"), N_("Still up?"), LATE_NIGHT},
};
const Phrase WELCOME_BACK = {"welcome-back", N_("Welcome back, {name}"), N_("Welcome back"), -1};

Line say(const Phrase& p, const std::string& name) {
    Line l;
    l.key = p.key;
    l.text = name.empty() ? tr(p.without_name) : util::replace_all(tr(p.with_name), "{name}", name);
    return l;
}

int day_number() {
    std::tm lt;
    util::local_tm(&lt);
    std::tm base = lt;
    base.tm_hour = 12;  // by calendar day: not shifted by an hour of daylight saving
    base.tm_min = base.tm_sec = 0;
    base.tm_isdst = -1;
    return (int)(mktime(&base) / 86400);
}

// The newest half-watched thing on a service that shows, if it is worth asking about.
std::string half_watched() {
    const int64_t now = util::unix_time();
    const store::Resume* best = nullptr;
    const std::vector<store::Resume> all = store::resume_list(60);
    for (const store::Resume& r : all) {
        if (r.title.empty() || r.duration <= 0) continue;
        const double progress = r.position / r.duration;
        if (progress < STILL_MIN || progress > STILL_MAX) continue;
        if (r.updated <= 0 || now - r.updated > STILL_RECENT) continue;
        const app::Section s = personal::section_of_service(r.service);
        if (s != app::SEC_NONE && !app::section_visible(s)) continue;
        if (!best || r.updated > best->updated) best = &r;
    }
    return best ? best->title : std::string();
}

struct State {
    std::string profile;
    Band band = BAND_COUNT;
    Style style = SIMPLE;
    double picked_at = -1e9;
    std::string text;
    int picks = 0;
    bool bday = false;
    std::set<std::string> welcomed;  // profiles asked about being away, this run
};
State& state() {
    static State s;
    return s;
}

}  // namespace

std::string birthday_line() {
    const std::string name = store::profile(store::active_profile()).name;
    return name.empty() ? std::string(tr("Happy birthday!")) : util::replace_all(tr("Happy birthday, {name}!"), "{name}", name);
}

Band band_now() {
    std::tm lt;
    util::local_tm(&lt);
    return lt.tm_hour < 5 ? LATE_NIGHT : lt.tm_hour < 12 ? MORNING : lt.tm_hour < 18 ? AFTERNOON : EVENING;
}

Style style() {
    const int64_t v = store::get_int(STYLE_KEY, SIMPLE);
    return v == OFF ? OFF : v == PLAYFUL ? PLAYFUL : SIMPLE;
}

std::vector<Line> candidates(const Context& c) {
    std::vector<Line> out;
    if (c.style == OFF) return out;
    if (c.style == SIMPLE) {
        out.push_back(say(SIMPLE_LINES[c.band], c.name));
        return out;
    }
    if (c.welcome_back) {
        out.push_back(say(WELCOME_BACK, c.name));
        return out;
    }
    for (const Phrase& p : PLAYFUL_LINES)
        if (p.band < 0 || p.band == c.band) out.push_back(say(p, c.name));
    if (!c.still_title.empty()) {  // asked about twice as often as one of the others: it is the useful one
        Line l;
        l.key = "still";
        l.text = util::replace_all(tr("Still watching {title}?"), "{title}", c.still_title);
        out.push_back(l);
        out.push_back(l);
    }
    return out;
}

const Line& choose(const std::vector<Line>& lines, uint64_t seed, const std::string& avoid) {
    static const Line none;
    if (lines.empty()) return none;
    std::vector<const Line*> options;
    for (const Line& l : lines)
        if (l.key != avoid) options.push_back(&l);
    if (options.empty()) return lines[0];  // all of it was what was said last: there is no other
    return *options[seed % options.size()];
}

void forget() { state().picked_at = -1e9; }

const std::string& current() {
    State& st = state();
    const Style sty = style();
    const std::string profile = store::active_profile();
    const Band band = band_now();
    const double now = util::now_seconds();
    // On the birthday it says so, whatever the style (it was asked for by giving the date).
    const bool bday = birthday::is_today();
    if (bday) {
        if (st.profile != profile || !st.bday || now - st.picked_at >= REPICK_SECONDS) {
            st.text = birthday_line();
            st.profile = profile;
            st.band = band;
            st.style = sty;
            st.bday = true;
            st.picked_at = now;
        }
        return st.text;
    }
    st.bday = false;
    if (sty == OFF) {
        st.text.clear();
        st.style = sty;
        return st.text;
    }
    if (st.profile == profile && st.band == band && st.style == sty && now - st.picked_at < REPICK_SECONDS) return st.text;

    Context c;
    c.style = sty;
    c.band = band;
    c.name = store::profile(profile).name;
    if (sty == PLAYFUL) c.still_title = half_watched();
    // Away for a week or more: "Welcome back", once for the profile this run.
    const int today = day_number();
    if (!st.welcomed.count(profile)) {
        st.welcomed.insert(profile);
        const int64_t seen = store::get_int(SEEN_KEY, 0);
        c.welcome_back = seen > 0 && today - seen >= AWAY_DAYS;
        if (seen != today) store::set_int(SEEN_KEY, today);
    }
    const std::vector<Line> lines = candidates(c);
    const std::string avoid = store::get_str(LAST_KEY);
    const uint64_t seed = util::hash64(profile + ":" + std::to_string(today) + ":" + std::to_string((int)band) + ":" + std::to_string(st.picks++));
    const Line& line = choose(lines, seed, avoid);
    st.text = line.text;
    if (!line.key.empty() && line.key != avoid) store::set_str(LAST_KEY, line.key);
    st.profile = profile;
    st.band = band;
    st.style = sty;
    st.picked_at = now;
    return st.text;
}

}  // namespace greeting
