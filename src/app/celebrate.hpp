// The birthday celebration: balloons rising, sparklers twinkling or soft fireworks, over whatever is on screen,
// for six seconds at most. It never takes input. Everything fades in and out slowly, and nothing flashes (no more
// than a gentle swell, far under three a second, and nothing red or full-screen). "Reduce motion" gets a still
// ribbon instead. Drawn from the main frame.
#pragma once

#include <string>

#include "app/birthday.hpp"

namespace celebrate {

constexpr float DURATION = 6.0f;

// Starts it. `preview` is from the birthday settings, where it may play over that screen; otherwise it plays only
// on Home and stops if something else comes up.
void start(birthday::Style style, const std::string& ribbon_text, bool preview = false);
void stop();
bool active();
bool preview();
// Draws this frame's picture (nothing when it isn't playing) and advances it.
void draw(float dt);

// For the tests: how visible the whole thing is `t` seconds in, 0 to 1, and a particle's twinkle at `t`.
float envelope(float t);
float twinkle(float t, float period, float phase);

}  // namespace celebrate
