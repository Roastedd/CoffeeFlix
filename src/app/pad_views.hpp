// The GamePad's views, picked from the panel that opens with MINUS (or a tap on the GamePad icon at
// the top right):
//   Mirror      both screens show the same, as always;
//   TV only     the GamePad's screen and sound are off, to save its battery, and its buttons work.
// Default mirrors browsing, then splits the playback screen automatically.
// Where the sound plays is Settings' "Sound plays on".
// See development/GAMEPAD-VIEWS.md.
#pragma once

#include <SDL2/SDL.h>

#include <string>

#include "core/input.hpp"

namespace pad_views {

// Keep the surviving IDs stable; the old value 1 is no longer a supported choice.
enum View { VIEW_MIRROR = 0, VIEW_TV_ONLY = 2, VIEW_HYBRID = 3 };

// Whether there is a GamePad whose outputs can be told apart (the Wii U).
bool available();
View view();
void set_view(View v);

// Opens the panel on the right.
void open_drawer();

// The GamePad's battery, from the console's own reading (VPADStatus.battery). Only 7 steps come from it, and the console
// gives no percentage, so none is shown: a number would be a guess that cannot match the GamePad's own indicator (a
// supporter noticed it didn't). The logs show it going 6, 5, 4, 3, 2 over hours on the battery, and sitting at 0 while the
// GamePad was charging, so 0 is "charging" and 6 is full; 1 to 5 are the steps between. What the gauge shows is those
// steps: five segments, red at the last.
struct Battery {
    int level = -1;         // the reading, 0-6; -1: not known
    int bars = 0;           // segments lit, 0-5
    bool charging = false;  // plugged in and charging (reads 0)
    bool full = false;      // charged (reads 6)
    bool low = false;       // the last step: the red LED is about to blink
    bool known() const { return level >= 0; }
};
Battery battery();
// A few words for the level ("About half"), the same ones the GamePad panel shows.
std::string battery_words(const Battery& b);
// Draws the gauge (five segments, a bolt while charging) and its words, ending at `right`, centred on `cy`. Returns the
// width it took.
float draw_battery(float right, float cy, float scale = 1.0f, bool words = true);

// The status bar's battery: a larger gauge of five segments (a bolt over it while charging).
// Occupies [right - width, right] centred on `cy`; returns the width (0 when the level is not known).
float battery_width();
float draw_status_battery(float right, float cy);

// Once, with the window's renderer; and at the end.
void init(SDL_Renderer* renderer);
void shutdown();

// Remote view: the GamePad has a picture of its own this frame, a texture drawn apart from the
// window (which is the TV's). False in the other views, and while a keyboard is open, which both
// screens show alike.
bool pad_scene();
// Resolve automatic view before drawing. The screen's playback capability must not depend on pad_scene().
void begin_frame(bool playback_screen);
// Once a frame, before update(): the top screen draws the TV's picture itself (a video), so the
// GamePad is silent.
void set_tv_scene(bool on);

// Draws into the GamePad's own picture while it's alive, when `on`; into the window's again after.
class PadPass {
public:
    explicit PadPass(bool on);
    ~PadPass();
    PadPass(const PadPass&) = delete;
    PadPass& operator=(const PadPass&) = delete;

private:
    bool on_;
};
// Makes the GamePad's picture black (a first or a stale one).
void clear_pad_scene();

// In the remote view while a video plays on the TV: where the sound goes (0 the TV, 1 the GamePad's
// speakers and headphones, 2 both). Not kept: it's the TV every time.
int remote_sound();
void cycle_remote_sound();
// The GamePad's screen asleep (Standby) while a video plays on the TV, until a button or the
// touch screen is used; the picture isn't sent meanwhile. Settings > "GamePad screen sleeps".
void sleep_pad();
bool pad_sleeping();
// The GamePad's screen is off (the TV only view, or put to sleep while a video plays): nothing drawn only there can be seen.
bool pad_screen_off();
// Once a frame, with the input: wakes a sleeping GamePad (the press that wakes it is not for the
// screen under it) and puts it to sleep after the time Settings says without any.
void filter_input(Input& in);

// Every frame, once the frame is drawn: puts the GamePad's outputs as the view and the sound
// setting say. The GamePad's screen is on while a keyboard is open, whatever the view.
void update();
// The developer lab sets the GamePad's outputs itself while it is open.
void suspend(bool on);

}  // namespace pad_views
