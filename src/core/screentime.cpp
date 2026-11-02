#include "core/screentime.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/store.hpp"
#include "core/util.hpp"

namespace screentime {

namespace {
constexpr const char* WEEK_KEY = "st_week";            // minutes
constexpr const char* WEEKEND_KEY = "st_weekend";
constexpr const char* QUIET_KEY = "st_quiet";           // "1" when on
constexpr const char* FROM_KEY = "st_from";             // minutes after midnight
constexpr const char* TO_KEY = "st_to";
constexpr const char* USED_KEY = "st_used";             // "YYYYMMDD,seconds,extra minutes"
constexpr const char* OVERRIDE_KEY = "st_override";      // unix time quiet hours are let off until

std::string get(const std::string& profile, const char* key, const std::string& def = "") {
    return profile.empty() ? store::get_str(key, def) : store::get_str_of(profile, key, def);
}
void put(const std::string& profile, const char* key, const std::string& v) {
    if (profile.empty()) store::set_str(key, v);
    else store::set_str_of(profile, key, v);
}
int geti(const std::string& profile, const char* key, int def) {
    const std::string s = get(profile, key);
    return s.empty() ? def : atoi(s.c_str());
}
std::string day_key(const std::tm& t) { return util::fmt("%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); }

struct Used {
    std::string day;
    double seconds = 0;
    int extra = 0;
};
Used read_used(const std::string& profile) {
    Used u;
    const std::string raw = get(profile, USED_KEY);
    char day[16] = {};
    double sec = 0;
    int extra = 0;
    if (sscanf(raw.c_str(), "%8[0-9],%lf,%d", day, &sec, &extra) >= 2) {
        u.day = day;
        u.seconds = sec;
        u.extra = extra;
    }
    return u;
}
void write_used(const std::string& profile, const Used& u) { put(profile, USED_KEY, util::fmt("%s,%.0f,%d", u.day.c_str(), u.seconds, u.extra)); }

// Today's record, started over when the day has changed.
Used used_today(const std::string& profile, const std::tm& now) {
    Used u = read_used(profile);
    if (u.day != day_key(now)) {
        u = Used();
        u.day = day_key(now);
    }
    return u;
}
}  // namespace

Rules rules(const std::string& profile) {
    Rules r;
    r.weekday_minutes = std::max(0, geti(profile, WEEK_KEY, 0));
    r.weekend_minutes = std::max(0, geti(profile, WEEKEND_KEY, 0));
    r.quiet = get(profile, QUIET_KEY) == "1";
    r.quiet_from = std::clamp(geti(profile, FROM_KEY, 20 * 60), 0, 24 * 60 - 1);
    r.quiet_to = std::clamp(geti(profile, TO_KEY, 7 * 60), 0, 24 * 60 - 1);
    return r;
}

void set_rules(const std::string& profile, const Rules& r) {
    put(profile, WEEK_KEY, std::to_string(std::max(0, r.weekday_minutes)));
    put(profile, WEEKEND_KEY, std::to_string(std::max(0, r.weekend_minutes)));
    put(profile, QUIET_KEY, r.quiet ? "1" : "0");
    put(profile, FROM_KEY, std::to_string(r.quiet_from));
    put(profile, TO_KEY, std::to_string(r.quiet_to));
}

int limit_minutes(const Rules& r, const std::tm& day) { return day.tm_wday == 0 || day.tm_wday == 6 ? r.weekend_minutes : r.weekday_minutes; }

bool in_quiet_hours(const Rules& r, const std::tm& now) {
    if (!r.quiet || r.quiet_from == r.quiet_to) return false;
    const int m = now.tm_hour * 60 + now.tm_min;
    if (r.quiet_from < r.quiet_to) return m >= r.quiet_from && m < r.quiet_to;  // within a day
    return m >= r.quiet_from || m < r.quiet_to;                                  // across midnight
}

double Today::remaining() const {
    if (limit_minutes <= 0) return 1e9;
    return std::max(0.0, (limit_minutes + extra_minutes) * 60.0 - used);
}

Today today(const std::string& profile) {
    std::tm now;
    util::local_tm(&now);
    const Used u = used_today(profile, now);
    Today t;
    t.used = u.seconds;
    t.extra_minutes = u.extra;
    t.limit_minutes = limit_minutes(rules(profile), now);
    return t;
}

void playing(double seconds) {
    if (seconds <= 0) return;
    std::tm now;
    util::local_tm(&now);
    Used u = used_today("", now);
    u.seconds += std::min(seconds, 30.0);
    write_used("", u);
}

void add_extra(const std::string& profile, int minutes) {
    std::tm now;
    util::local_tm(&now);
    Used u = used_today(profile, now);
    u.extra += minutes;
    write_used(profile, u);
}

void allow_quiet_for(const std::string& profile, int minutes) {
    put(profile, OVERRIDE_KEY, std::to_string((long long)util::unix_time() + (long long)minutes * 60));
}

void reset_today(const std::string& profile) {
    std::tm now;
    util::local_tm(&now);
    Used u;
    u.day = day_key(now);
    write_used(profile, u);
    put(profile, OVERRIDE_KEY, "0");
}

Status status(const std::string& profile) {
    Status s;
    std::tm now;
    util::local_tm(&now);
    const Rules r = rules(profile);
    if (!r.any()) return s;
    const long long until = atoll(get(profile, OVERRIDE_KEY, "0").c_str());
    if (in_quiet_hours(r, now) && (long long)util::unix_time() >= until) {
        s.state = QUIET;
        s.quiet_ends = r.quiet_to;
        return s;
    }
    const Today t = today(profile);
    s.remaining = t.remaining();
    if (t.limit_minutes <= 0) return s;
    if (s.remaining <= 0) s.state = TIME_UP;
    else if (s.remaining <= 60) s.state = ONE_LEFT;
    else if (s.remaining <= 600) s.state = TEN_LEFT;
    return s;
}

std::string clock_text(int m) { return util::fmt("%02d:%02d", m / 60, m % 60); }

}  // namespace screentime
