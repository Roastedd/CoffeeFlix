#include "screens/screens.hpp"

#include "ui/ui.hpp"

namespace screens {

namespace {

class Stub : public app::Screen {
public:
    Stub(app::Section s, const char* title) : s_(s), title_(title) {}
    app::Section section() const override { return s_; }
    void frame() override {
        using namespace ui;
        float x = content_x();
        text::draw(font::display, x, 80, title_, theme().text);
        Id g = id(title_);
        for (int i = 0; i < 6; i++) {
            Rect r(x + i * 190, 200, 170, 250);
            Item it = focusable(id(g, i), r, g, i == 0 ? F_DEFAULT : 0);
            float s = 1 + 0.07f * it.f;
            Rect br = r.scaled(s).offset(bump_x(id(g, i)), 0);
            gfx::shadow(br, 20, Color(0, 0, 0, 140));
            gfx::fill_rrect_vgrad(br, 14, gfx::lerp(theme().accent, theme().accent2, i / 5.0f), Color(40, 30, 50, 255));
            focus_ring(br, 14, it.f);
        }
    }

private:
    app::Section s_;
    const char* title_;
};

}  // namespace

std::unique_ptr<app::Screen> make_home() { return std::make_unique<Stub>(app::SEC_HOME, "Home"); }
std::unique_ptr<app::Screen> make_search() { return std::make_unique<Stub>(app::SEC_SEARCH, "Search"); }
std::unique_ptr<app::Screen> make_youtube() { return std::make_unique<Stub>(app::SEC_YOUTUBE, "YouTube"); }
std::unique_ptr<app::Screen> make_jellyfin() { return std::make_unique<Stub>(app::SEC_JELLYFIN, "Jellyfin"); }
std::unique_ptr<app::Screen> make_twitch() { return std::make_unique<Stub>(app::SEC_TWITCH, "Twitch"); }
std::unique_ptr<app::Screen> make_radio() { return std::make_unique<Stub>(app::SEC_RADIO, "Radio"); }
std::unique_ptr<app::Screen> make_podcasts() { return std::make_unique<Stub>(app::SEC_PODCASTS, "Podcasts"); }
std::unique_ptr<app::Screen> make_settings() { return std::make_unique<Stub>(app::SEC_SETTINGS, "Settings"); }

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
