#include "app/birthday.hpp"

#include <ctime>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace birthday {

namespace {
constexpr const char* KEY = "birthday";            // "MM-DD"
constexpr const char* STYLE_KEY = "birthday_style";
constexpr const char* EVERY_KEY = "birthday_every";
constexpr const char* LAST_KEY = "birthday_last";   // the day it was last celebrated, "YYYYMMDD"

bool leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

std::string today_key(const std::tm& t) { return util::fmt("%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); }
}  // namespace

int days_in_month(int month) {
    static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

bool valid(int month, int day) { return month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(month); }

Date get(const std::string& profile) {
    const std::string raw = profile.empty() ? store::get_str(KEY) : store::get_str_of(profile, KEY);
    Date d;
    int m = 0, day = 0;
    if (sscanf(raw.c_str(), "%d-%d", &m, &day) == 2 && valid(m, day)) {
        d.month = m;
        d.day = day;
    }
    return d;
}

bool set(const std::string& profile, int month, int day) {
    std::string raw;
    if (month != 0 || day != 0) {
        if (!valid(month, day)) return false;
        raw = util::fmt("%02d-%02d", month, day);
    }
    if (profile.empty()) store::set_str(KEY, raw);
    else store::set_str_of(profile, KEY, raw);
    return true;
}

bool is_today(const std::string& profile) {
    const Date d = get(profile);
    if (!d.set()) return false;
    std::tm now;
    util::local_tm(&now);
    const int month = now.tm_mon + 1, day = now.tm_mday;
    if (d.month == 2 && d.day == 29 && !leap(now.tm_year + 1900)) return month == 2 && day == 28;
    return d.month == month && d.day == day;
}

Style style() {
    const int64_t v = store::get_int(STYLE_KEY, STYLE_BALLOONS);
    return v == STYLE_NONE ? STYLE_NONE : v == STYLE_SPARKLERS ? STYLE_SPARKLERS : v == STYLE_FIREWORKS ? STYLE_FIREWORKS : STYLE_BALLOONS;
}
void set_style(Style s) { store::set_int(STYLE_KEY, s); }
bool every_open() { return store::get_bool(EVERY_KEY, false); }
void set_every_open(bool on) { store::set_bool(EVERY_KEY, on); }

bool celebrate_now() {
    if (!is_today()) return false;
    std::tm now;
    util::local_tm(&now);
    const std::string today = today_key(now);
    if (!every_open() && store::get_str(LAST_KEY) == today) return false;
    store::set_str(LAST_KEY, today);
    return true;
}

std::string long_text(const Date& d) {
    if (!d.set()) return "";
    std::string s = tr("{month} {day}");  // each language puts them in its own order
    s = util::replace_all(s, "{month}", i18n::month_name(d.month - 1));
    return util::replace_all(s, "{day}", std::to_string(d.day));
}

}  // namespace birthday
