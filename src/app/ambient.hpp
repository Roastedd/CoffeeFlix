// The startup animation and the idle screensaver: full-screen layers drawn
// over the app that swallow input while they are up.
#pragma once

namespace ambient {

// Call at the start of the frame (after ui::begin_frame): eats input while the
// intro or screensaver is showing. `video_foreground` is true while a video
// plays full screen, which never starts the screensaver.
void begin(bool video_foreground);
// Call at the end of the frame to draw whichever layer is up.
void draw();

bool intro_running();

}  // namespace ambient
