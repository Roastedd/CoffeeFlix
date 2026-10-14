// Screen stack, navigation rail and the main loop.
#pragma once

#include <memory>
#include <string>

namespace app {

enum Section { SEC_HOME, SEC_SEARCH, SEC_YOUTUBE, SEC_JELLYFIN, SEC_TWITCH, SEC_RADIO, SEC_PODCASTS, SEC_MEDIA,
               SEC_SETTINGS, SEC_COUNT, SEC_NONE = -1 };

class Screen {
public:
    virtual ~Screen() = default;
    virtual void frame() = 0;                       // update + draw, every frame while on top
    virtual void on_enter() {}                      // became the top screen (again)
    virtual bool on_back() { return false; }        // true = handled internally
    virtual bool prevents_sleep() const { return false; } // temporary transfers keep the console awake
    virtual bool fullscreen() const { return false; }  // hide the rail (players, viewers)
    // In the GamePad's Remote view: this screen draws the TV's picture itself (a video) and the
    // GamePad's own (its controls) from frame(), with pad_views::PadPass; everything else is drawn
    // for the GamePad, and the TV stays black.
    // Capability is independent of the selected view; automatic routing queries it before drawing.
    virtual bool tv_scene() const { return false; }
    virtual bool draws_background() const { return false; }
    virtual Section section() const { return SEC_NONE; }
    // Content area left edge when the rail is shown.
    static float content_x();
};

void push(std::unique_ptr<Screen> s);
void pop();
void open_section(Section s);   // replaces the stack with the section's root
Screen* top();
bool rail_focused();
void focus_rail();
void quit();

// Currently playing background audio (music, radio, podcast) shows a mini player.
void set_mini_player_visible(bool v);

int run(int argc, char** argv);

}  // namespace app
