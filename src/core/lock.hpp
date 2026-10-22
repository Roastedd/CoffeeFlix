// The grown-ups' lock: a 4-digit PIN or a 4 to 6 press button code, one for the whole console. It leaves a Kids
// profile and opens what children shouldn't reach. The code is kept salted and stretched, not as it is: that
// stops someone reading it off the SD card, not someone who copies the card and tries all 10,000 PINs, so the
// real protection is the lockout after wrong tries.
#pragma once

#include <string>

namespace lock {

enum class Kind { NONE, PIN, BUTTONS };

// A code is text: digits for a PIN, letters from "UDLRABXY" (up, down, left, right, A, B, X, Y) for a button code.
constexpr const char* BUTTON_SYMBOLS = "UDLRABXY";
constexpr size_t PIN_LENGTH = 4, MIN_BUTTONS = 4, MAX_BUTTONS = 6;

bool is_set();
Kind kind();
size_t length();  // of the code that's set (0: none)
bool valid(Kind kind, const std::string& code);

// Sets the code (replacing any); false when it isn't a valid code of that kind.
bool set(Kind kind, const std::string& code);
void clear();

enum class Result { OK, WRONG, LOCKED_OUT };
// Whether `code` is the code. Wrong tries add up: 30 s of waiting after the third, 5 minutes after the fifth, an hour
// after the seventh and every one after. While waiting nothing is checked (LOCKED_OUT, even for the right code).
Result check(const std::string& code);
// Seconds left before the next try (0: may try).
int wait_seconds();

// At the start: a file named reset-lock.txt in the data folder clears the lock (and is deleted), for a forgotten code. Whoever can
// put a file on the SD card can also read the code's hash off it, so it asks no more than that.
void init(const std::string& data_dir);

}  // namespace lock
