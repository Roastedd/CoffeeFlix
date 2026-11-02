// A birthday, optional and kept on this console for the profile only: the month and the day, no year (so no age is
// kept or shown). On the day CoffeeFlix greets by name, and plays a short celebration the first time Home is reached
// that day (or every time it is opened, if chosen).
#pragma once

#include <string>

namespace birthday {

struct Date {
    int month = 0, day = 0;  // 0: none
    bool set() const { return month >= 1 && day >= 1; }
};
enum Style { STYLE_NONE = 0, STYLE_BALLOONS = 1, STYLE_SPARKLERS = 2, STYLE_FIREWORKS = 3 };

int days_in_month(int month);                 // February has 29: someone is born on the 29th
bool valid(int month, int day);

Date get(const std::string& profile = "");    // the profile in use when empty
// An impossible date (February 30th, month 13) is not kept. 0, 0 removes it.
bool set(const std::string& profile, int month, int day);

// Whether it is the profile's birthday now, by the console's own date. Born on February 29th: celebrated on the
// 28th in the years that have no 29th.
bool is_today(const std::string& profile = "");

Style style();                                // the animation; balloons unless chosen otherwise
void set_style(Style s);
bool every_open();                            // celebrate each time CoffeeFlix is opened that day, not the first time only
void set_every_open(bool on);

// Home asks as it is reached: whether to play the celebration now. True the first time on the birthday (and each
// time CoffeeFlix is opened if every_open), remembering the day.
bool celebrate_now();

// "March 4", in the current language.
std::string long_text(const Date& d);

}  // namespace birthday
