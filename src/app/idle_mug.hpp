// A little coffee mug with steam in the corner of Home after a minute without anyone touching anything: a bit of
// personality, nothing more. It is faint, takes no input, and is gone (in a third of a second) at the first
// button. Off in Settings; a still mug with "Reduce motion".
#pragma once

namespace idle_mug {

constexpr double IDLE_SECONDS = 60;

bool enabled();
void set_enabled(bool on);
// Every frame; `eligible`: Home is what is showing and nothing is over it.
void draw(float dt, bool eligible);
// For the tests: how visible it is `idle` seconds in with nothing touched (0..1).
float visibility_after(double idle);

}  // namespace idle_mug
