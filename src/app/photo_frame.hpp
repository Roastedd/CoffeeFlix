// The photo frame: when the screensaver comes on (or on request), a slow slideshow of the pictures on the SD card, each
// laid in a soft blur of itself, drifting a little and fading into the next, with the time small in a corner. The pictures
// are picked at random a few at a time (screens/surprise), never one after another on the same few, and the card is not
// scanned. With nothing to show it hands back to the plain clock screensaver.
#pragma once

#include <string>

namespace photo_frame {

enum Source { FROM_PHOTOS_FOLDER = 0, FROM_CARD = 1 };

// The screensaver's style, and what the frame was set to.
bool wanted();                    // "Photos" is the style the screensaver was given
void set_wanted(bool on);
Source source();
void set_source(Source s);
int seconds_per_photo();          // 12 unless chosen
void set_seconds_per_photo(int s);
bool show_clock();
void set_show_clock(bool on);

// The screensaver came on / went off.
void begin();
void end();
// Draws this frame (the whole screen) at `alpha`. False when there is nothing to show (no pictures found), so the
// screensaver draws the clock instead.
bool draw(float seconds, float dt);

// For the tests: the Ken Burns drift of a picture at `t` seconds into its time, 0..1 of the way, as a scale and a pan.
struct Drift { float scale, pan_x, pan_y; };
Drift drift_for(const std::string& path, float t, float duration, bool still);

}  // namespace photo_frame
