// The greeting above Home's picture: nothing, the time of day with the person's name, or a rotating friendly
// line (and "Still watching ...?" when there is really something half-watched). Chosen once and kept while
// Home is open, never the same line twice in a row, and the person's name is optional: without one every line
// reads well without it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace greeting {

enum Style { OFF = 0, SIMPLE = 1, PLAYFUL = 2 };
enum Band { LATE_NIGHT, MORNING, AFTERNOON, EVENING, BAND_COUNT };

// "Happy birthday, Edward!", or without the name. On the day, it is also what Home says above the picture.
std::string birthday_line();

Style style();                         // the profile in use; Simple unless chosen otherwise
const std::string& current();          // "" when off; picks again when the profile or time of day changed, or an hour passed
Band band_now();

// The pieces, for the tests: what can be said now, and the line picked from them.
struct Context {
    Style style = SIMPLE;
    Band band = MORNING;
    std::string name;                  // "" when the profile has none
    std::string still_title;           // "" when nothing half-watched
    bool welcome_back = false;         // away for a week or more, and not yet welcomed
};
struct Line {
    std::string key;                   // stable, to avoid saying the same one twice
    std::string text;                  // translated, with the name put in
};
std::vector<Line> candidates(const Context& c);
// A line from the candidates by the seed, other than `avoid` when there is a choice.
const Line& choose(const std::vector<Line>& lines, uint64_t seed, const std::string& avoid);

// What the Home screen asked for, without a restart: forget the choice made.
void forget();

}  // namespace greeting
