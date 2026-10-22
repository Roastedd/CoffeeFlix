// Screen stack, navigation rail and the main loop.
#pragma once

#include <memory>
#include <string>

namespace app {

enum Section { SEC_HOME, SEC_SEARCH, SEC_YOUTUBE, SEC_JELLYFIN, SEC_NAVIDROME, SEC_TWITCH, SEC_RADIO, SEC_PODCASTS, SEC_MEDIA,
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
    // for the GamePad, and the TV stays black. This describes the screen's capability,
    // independent of the selected view, so automatic view selection has no circular dependency.
    virtual bool tv_scene() const { return false; }
    virtual bool draws_background() const { return false; }
    virtual Section section() const { return SEC_NONE; }
    // Content area left edge when the rail is shown.
    static float content_x();
};

void push(std::unique_ptr<Screen> s);
void pop();
void open_section(Section s);   // replaces the stack with the section's root
// Makes `id` the profile in use: stops what plays, loads its sign-ins; `go_home` starts over at Home.
void use_profile(const std::string& id, bool go_home);
// A kids profile may only open some sections (the others open Home instead).
bool section_allowed(Section s);
// Whether the section shows at all for the profile in use: allowed, and not turned off in Settings > Make it yours.
// Home and Settings always do. A turned-off service has no side menu entry, Home row or search results, and
// makes no network requests for them.
bool section_visible(Section s);
// "Grown-ups": the code, then the profiles; the way out of a kids profile.
void request_grownups();
Screen* top();
bool rail_focused();
void focus_rail();
void quit();

// Currently playing background audio (music, radio, podcast) shows a mini player.
void set_mini_player_visible(bool v);

int run(int argc, char** argv);

}  // namespace app
