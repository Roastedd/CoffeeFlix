// Calmer titles: our own small take on what DeArrow does, without anyone's data. A title that SHOUTS in capitals is
// put in sentence case, and runs of "!!!" or "???" become one. Nothing else is touched: no rewording, no guessing at
// clickbait, and titles that aren't shouting come back exactly as they were.
#pragma once

#include <string>

namespace title {

// Settings > "Calmer titles" (on by default).
bool enabled();
// The tidied title (always, whatever the setting).
std::string tidy(const std::string& title);
// The tidied title when the setting is on, else as it is.
std::string shown(const std::string& title);

}  // namespace title
