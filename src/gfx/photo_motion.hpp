// How a slideshow moves over a photo: one move per picture, planned from where its people are (gfx/focus). One person: it
// comes in close on them. Two or three: it settles on the group. A crowd: it stays back and pans along them, or draws back to
// show all. A landscape: a slow drift toward the part that stands out. The same plan serves the viewer and the screensaver,
// and a picture that is fading out keeps going along its own move (it does not jump back to where it began).
#pragma once

#include <cstdint>

#include "gfx/focus.hpp"

namespace photo_motion {

// What is shown: `zoom` 1 is the whole picture fitted to the screen, 2 is half of it across; (cx, cy) is the point of the picture
// in the middle of the screen, 0..1.
struct View { float zoom = 1, cx = 0.5f, cy = 0.5f; };

struct Plan {
    View from, to;
    const char* kind = "drift";   // "close", "group", "crowd", "drift": for the log and the tests
};

// `picture_aspect` and `screen_aspect` are width / height. `seed` makes each picture move its own way. `still`: no move.
Plan plan(const focus::Focus& f, float picture_aspect, float screen_aspect, uint64_t seed, bool still);

// The same move with less of it: `k` 0..1 of the zoom and of the shift (a screensaver moves less than a slideshow).
Plan soften(Plan plan, float k);

// Where the move is `p` of the way (0..1, a little past 1 carries on at the same pace). Eased gently at the start only.
View at(const Plan& plan, float p);

// The picture drawn so `v` is in view: scale about the screen's centre, then shift, both in screen pixels. `drawn_w/h` is the
// picture fitted to the screen (what image_contain draws at zoom 1).
struct Transform { float scale, dx, dy; };
Transform transform(const View& v, float drawn_w, float drawn_h);

// A view kept inside the picture (no empty edge shown), where the picture is bigger than the screen at that zoom.
View clamp(View v, float picture_aspect, float screen_aspect);

}  // namespace photo_motion
