// Where the people are in a photo, as far as a few lines of arithmetic can tell: patches of skin tone that are the size and shape
// of a face or a head and shoulders are counted as people, and where there are none, the busiest part of the picture is its
// subject. A guess (there is no face finder in the console's memory to spare), made on a small copy in a few milliseconds, to
// decide how a slideshow moves over the picture: close in on one person, stay back for a crowd.
#pragma once

#include <SDL2/SDL.h>

namespace focus {

struct Focus {
    int people = 0;                      // patches that look like people (0: none found)
    float cx = 0.5f, cy = 0.5f;          // the main subject: the largest person, else the busiest part, 0..1 of the picture
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1;  // a box round all the people (round the busy part when there are none)
    float main_w = 0, main_h = 0;        // the box round the main subject alone
    bool found = false;                  // something stood out (false: a flat picture, the middle is as good as anywhere)
};

// Any surface of RGBA32 or an RGB format of 32 bits.
Focus analyze(SDL_Surface* s);

}  // namespace focus
