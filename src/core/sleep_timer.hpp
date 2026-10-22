// The sleep timer: stops what is playing after a while (music, radio, a podcast, a video), the sound fading away over its
// last half minute, or at the end of what is playing now. The timer only keeps time and the fade; the app asks it every
// frame and does the stopping.
#pragma once

namespace sleep_timer {

constexpr int END_OF_ITEM = -1;
constexpr double FADE_SECONDS = 30;

void start(int minutes);            // minutes > 0, or END_OF_ITEM
void cancel();
bool active();
bool at_end_of_item();              // started as "at the end of this one"
double remaining_seconds();         // minutes mode only (0 otherwise)
int chosen_minutes();               // what it was started with, for showing the menu's current choice

// Every frame. `item_ended`: what plays has come to its end. True on the frame the timer fires.
bool update(double dt, bool item_ended);
// Playing on to the next of a queue (or an autoplay countdown) must not start: the timer is waiting for the end of this one,
// or has just stopped at it.
bool end_pending();
// The app tells it the ended item is no longer showing (something else started, or it was closed).
void item_left();
// The sound's multiplier: 1, then down to 0 over the last half minute.
float fade();

}  // namespace sleep_timer
