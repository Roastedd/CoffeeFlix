// The card every player shows when a video ends: what plays next, with a countdown, or that it finished.
#pragma once
#include <string>

namespace screens {

enum class EndKind { EPISODE, VIDEO };
enum class EndAction { NONE, PLAY_NEXT, CANCEL, REPLAY, TOGGLE_AUTOPLAY, EXTRA, BACK };

constexpr double END_COUNTDOWN = 10;  // seconds before the next one starts by itself

struct EndCard {
    EndKind kind = EndKind::EPISODE;
    std::string title, subtitle;                         // what just played
    bool has_next = false, loading = false;              // loading: still looking for the next one
    std::string next_title, next_subtitle, next_overview, next_image;
    bool autoplay = true;                                // the setting
    bool counting = false;                               // the countdown is running
    double elapsed = 0;                                  // seconds since the video ended
    const char* extra = nullptr;                         // one more button, e.g. "Choose source" (EndAction::EXTRA)
    int extra_icon = 0;
};

// Draws it over whatever is on screen and returns what was chosen this frame.
EndAction draw_end_card(const EndCard& card);

}  // namespace screens
