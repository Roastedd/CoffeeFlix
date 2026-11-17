// The sound level from the controller, wherever something plays (a video, music, the radio, a podcast), so
// the TV's own volume can stay where it is:
//   - the right stick up and down (GamePad, Pro and Classic Controller)
//   - B held with Plus or Minus (a Wii Remote has no right stick). While something plays, a press of B reaches
//     the screens when it's let go, not when it goes down: held with Plus or Minus, it never reaches them.
// A change shows for a moment at the top right of the picture.
#pragma once

#include "core/input.hpp"

namespace volume_keys {

// Every frame, before anything else reads the input. `active`: something plays and nothing on screen takes
// the buttons for itself (a button code being entered).
void begin_frame(Input& in, bool active);

// Louder (steps > 0) or quieter by 5% a step, shown at the top right; a bump at either end.
void nudge(int steps);
// The level changed some other way (a slider): shown the same.
void changed();
// Not shown this frame (a screen showing the level itself, as the player's volume bar does).
void hide_level();
// Draws the level after a change, over what's on screen.
void draw_level();

}  // namespace volume_keys
