// A keyboard drawn by the app, on the TV, for the controllers that can't use the system keyboard: the Wii U's own keyboard
// (swkbd) is drawn on the GamePad's screen, so with the GamePad dark (TV only view) or put down for a Pro Controller or a
// Wii Remote it can't be seen. This one is moved with the D-pad or a stick and typed with A (or the pointer, or a tap).
//   A type   B cancel   X space   Y backspace   L or R shift   + done
#pragma once

#include <functional>
#include <string>

#include "core/input.hpp"

namespace screens::osk {

// Whether to use this keyboard rather than the system's for the next text entry.
bool wanted();

void open(const std::string& title, const std::string& initial, bool password, std::function<void(std::string)> done);
bool active();
// Before the frame's screens see the input: takes the presses it uses (all of them, while it is open).
void input(Input& in);
// Draws the keyboard over everything.
void draw();

// Characters of a page, for the tests.
std::string page_chars(int page);
// The text after a key, for the tests: `text` + what one press of `key` does (a character, or BACKSPACE).
std::string press(std::string text, const std::string& key);
constexpr const char* BACKSPACE = "\b";

}  // namespace screens::osk
