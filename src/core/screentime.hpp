// Screen time for a kids profile, set by the grown-ups: how long a day it may watch and listen (one limit for school
// days, one for the weekend) and quiet hours at night. The time is what the profile played, counted as it plays; a
// warning comes at ten minutes and at one, and then a "that's all for today" screen that only the grown-ups' code
// gets past. It goes by the console's clock and calendar. Kept per profile on this console.
#pragma once

#include <ctime>
#include <string>

namespace screentime {

struct Rules {
    int weekday_minutes = 0;        // Monday to Friday; 0 is no limit
    int weekend_minutes = 0;        // Saturday and Sunday
    bool quiet = false;             // quiet hours: nothing plays
    int quiet_from = 20 * 60;       // minutes after midnight: from this time ...
    int quiet_to = 7 * 60;          // ... until this one (the next morning when it is the earlier)
    bool any() const { return weekday_minutes > 0 || weekend_minutes > 0 || quiet; }
};

Rules rules(const std::string& profile = "");     // the profile in use when empty
void set_rules(const std::string& profile, const Rules& r);

// Minutes allowed on a day: the weekday or the weekend one.
int limit_minutes(const Rules& r, const std::tm& day);
bool in_quiet_hours(const Rules& r, const std::tm& now);

struct Today {
    double used = 0;                // seconds played today
    int extra_minutes = 0;          // granted today by a grown-up
    int limit_minutes = 0;          // 0: none
    double remaining() const;       // seconds left today, or a very large number without a limit
};
Today today(const std::string& profile = "");

// The profile in use played for `seconds` more.
void playing(double seconds);
// A grown-up gives more time today, or lets quiet hours be broken for a while (`minutes` from now).
void add_extra(const std::string& profile, int minutes);
void allow_quiet_for(const std::string& profile, int minutes);
void reset_today(const std::string& profile);

enum State { OK, TEN_LEFT, ONE_LEFT, TIME_UP, QUIET };
struct Status {
    State state = OK;
    double remaining = 1e9;         // seconds of today's time left
    int quiet_ends = 0;             // when quiet hours end, minutes after midnight (QUIET)
};
Status status(const std::string& profile = "");

// "20:00", for showing (the 24-hour clock the status bar uses).
std::string clock_text(int minutes_after_midnight);

}  // namespace screentime
