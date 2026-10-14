#include "screens/screens.hpp"

namespace screens {

std::unique_ptr<app::Screen> make_section_root(app::Section s) {
    switch (s) {
        case app::SEC_SEARCH: return make_search();
        case app::SEC_YOUTUBE: return make_youtube();
        case app::SEC_JELLYFIN: return make_jellyfin();
        case app::SEC_TWITCH: return make_twitch();
        case app::SEC_RADIO: return make_radio();
        case app::SEC_PODCASTS: return make_podcasts();
        case app::SEC_MEDIA: return make_media();
        case app::SEC_SETTINGS: return make_settings();
        default: return make_home();
    }
}

}  // namespace screens
