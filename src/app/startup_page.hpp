// Stable IDs for the page chosen in Settings > Start screen.
#pragma once

#include <string_view>

#include "app/app.hpp"
#include "core/i18n.hpp"

namespace app {

inline constexpr const char* STARTUP_PAGE_KEY = "startup_page";

struct StartupPage {
    const char* id;
    Section section;
    const char* label;
};

// Store names rather than enum ordinals, so adding/reordering sections cannot
// send an existing installation to a different page after an update.
inline constexpr StartupPage STARTUP_PAGES[] = {
    {"home", SEC_HOME, N_("Home")},
    {"search", SEC_SEARCH, N_("Search")},
    {"youtube", SEC_YOUTUBE, "YouTube"},
    {"jellyfin", SEC_JELLYFIN, "Jellyfin"},
    {"twitch", SEC_TWITCH, "Twitch"},
    {"radio", SEC_RADIO, N_("Radio")},
    {"podcasts", SEC_PODCASTS, N_("Podcasts")},
    {"media", SEC_MEDIA, N_("My Media")},
};

inline const StartupPage& startup_page_for_id(std::string_view id) {
    for (const auto& page : STARTUP_PAGES)
        if (page.id == id) return page;
    return STARTUP_PAGES[0];
}

}  // namespace app
