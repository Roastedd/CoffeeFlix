// Settings > GamePad lab (developers, on the Wii U): puts the GamePad's display, picture and sound
// off one at a time and shows what the console answers and does, so the views planned for it (a
// remote while the TV plays, a TV-only mode that saves the GamePad's battery) rest on what the
// hardware really does. Everything goes back to normal after a time chosen here, on B, and when
// the screen closes. Not translated: developers only.
#include <algorithm>
#include <deque>
#include <string>

#include "app/pad_views.hpp"
#include "audio/mixer.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

constexpr int REVERT_SECONDS[] = {15, 60, 300, 900};
constexpr const char* REVERT_NAMES[] = {"15 s", "1 min", "5 min", "15 min"};
constexpr const char* SCREEN_NAMES[] = {"On", "Standby (dark until a button)", "Off"};
constexpr const char* PICTURE_NAMES[] = {"Shown", "Not sent (keeps the last)", "Stream stopped (may freeze)"};
constexpr const char* SCREEN_SHORT[] = {"on", "standby", "off"};
constexpr const char* PICTURE_SHORT[] = {"shown", "not sent", "stopped"};
constexpr int MAX_NOTES = 8;

bool normal(const platform::PadOutputs& o) {
    return o.screen == platform::PAD_SCREEN_ON && o.picture == platform::PAD_PICTURE_SHOWN && o.pad_sound && o.tv_sound &&
           !o.leave_dark_on_exit;
}

bool same(const platform::PadOutputs& a, const platform::PadOutputs& b) {
    return a.screen == b.screen && a.picture == b.picture && a.pad_sound == b.pad_sound && a.tv_sound == b.tv_sound &&
           a.leave_dark_on_exit == b.leave_dark_on_exit;
}

class GamePadLab : public app::Screen {
public:
    GamePadLab() : ui_sounds_(store::get_bool("ui_sounds", true)), opened_(time()) {
        audio::set_sfx_enabled(true);  // its beeps and clicks are what tells which speaker plays
        pad_views::suspend(true);      // the lab sets the outputs itself while it's open
        note("Lab opened");
    }
    ~GamePadLab() override {
        platform::set_pad_outputs(platform::PadOutputs());
        pad_views::suspend(false);     // the view in use takes over again
        audio::set_sfx_enabled(ui_sounds_);
    }

    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        const double now = time();
        const float x0 = content_x(), w = 640;
        const Id g = id("padlab");
        float y = 52;
        text::draw(font::display, x0, y, "GamePad lab", t.text);
        y += 78;

        platform::PadOutputs next = out_;
        auto row = [&](const char* key, const char* label, const char* value, int icon, int flags = 0) {
            const bool hit = value_row(id(g, key), Rect(x0, y, w, 56), label, value, icon, g, flags);
            y += 64;
            return hit;
        };
        if (row("screen", "GamePad screen", SCREEN_NAMES[out_.screen], ic::GAMEPAD, F_DEFAULT))
            next.screen = (platform::PadScreen)((out_.screen + 1) % 3);
        if (row("picture", "GamePad picture", PICTURE_NAMES[out_.picture], ic::TV))
            next.picture = (platform::PadPicture)((out_.picture + 1) % 3);
        if (row("padsound", "GamePad sound", out_.pad_sound ? "On" : "Off", out_.pad_sound ? ic::VOLUME_UP : ic::VOLUME_OFF))
            next.pad_sound = !out_.pad_sound;
        if (row("tvsound", "TV sound", out_.tv_sound ? "On" : "Off", out_.tv_sound ? ic::VOLUME_UP : ic::VOLUME_OFF))
            next.tv_sound = !out_.tv_sound;
        if (row("revert", "Back to normal after", REVERT_NAMES[revert_], ic::SCHEDULE)) {
            revert_ = (revert_ + 1) % 4;
            changed_at_ = now;
        }
        if (row("beep", "Test sound every 2 s", beeps_ ? "On" : "Off", ic::AUDIOTRACK)) beeps_ = !beeps_;
        if (row("leave", "Test: leave it dark when closing", out_.leave_dark_on_exit ? "On: close with HOME while it's dark" : "Off",
                ic::WARNING))
            next.leave_dark_on_exit = !out_.leave_dark_on_exit;
        if (row("normal", "Back to normal now", nullptr, ic::RESTORE)) next = platform::PadOutputs();

        if (!same(next, out_)) {
            out_ = next;
            changed_at_ = now;
            platform::set_pad_outputs(out_);
            note(util::fmt("Set: screen %s, picture %s, TV %s, pad %s%s", SCREEN_SHORT[out_.screen], PICTURE_SHORT[out_.picture],
                           out_.tv_sound ? "on" : "off", out_.pad_sound ? "on" : "off", out_.leave_dark_on_exit ? ", stay dark" : ""));
        }
        double left = 0;
        if (!normal(out_)) {
            left = REVERT_SECONDS[revert_] - (now - changed_at_);
            if (left <= 0) {
                out_ = platform::PadOutputs();
                platform::set_pad_outputs(out_);
                note("Back to normal: the time is up");
                left = 0;
            }
        }
        if (beeps_ && now - last_beep_ >= 2.0) {
            audio::play(audio::SFX_OPEN);
            last_beep_ = now;
        }

        draw_status(x0 + w + 20, 130, left);
        hint_bar({{"A", "Change"}, {"B", "Back (everything normal)"}});
    }

private:
    void note(const std::string& s) {
        log_message(LOG_OK, "Lab", "%s", s.c_str());
        notes_.push_front(util::fmt("%5.1f s  %s", time() - opened_, s.c_str()));
        while ((int)notes_.size() > MAX_NOTES) notes_.pop_back();
    }

    void draw_status(float x, float y, double left) {
        const Theme& t = theme();
        const float w = 1280 - 30 - x;
        const platform::PadStatus st = platform::pad_status();

        // Frames a second, to see whether not drawing the GamePad's picture is felt.
        fps_ += (dt() > 0 ? 1.0f / dt() - fps_ : 0.0f) * 0.05f;

        // What arrives from the GamePad while it's dark is the point of the test.
        if (st.held != held_) {
            held_ = st.held;
            note(held_ ? util::fmt("GamePad buttons 0x%05x", (unsigned)held_) : std::string("GamePad buttons released"));
        }
        if (st.touching != touching_) {
            touching_ = st.touching;
            note(touching_ ? "GamePad touch began" : "GamePad touch ended");
        }

        gfx::fill_rrect(Rect(x, y, w, 284), 14, t.surface);
        float ty = y + 16;
        auto line = [&](const std::string& s, gfx::Color c) {
            text::draw_fit(font::small, x + 18, ty, w - 36, s, c);
            ty += 26;
        };
        line("GamePad, live", t.accent);
        line(util::fmt("Display: asked %s, VPAD answered %d", SCREEN_SHORT[out_.screen], st.lcd_set), t.text);
        line(util::fmt("Display reads back %d (on 255, standby 0, off 1)", st.lcd_mode), t.text);
        line(util::fmt("Battery reading %d", st.battery), t.text);
        line(util::fmt("Buttons 0x%05x, touch %s, read %d", (unsigned)st.held, st.touching ? "yes" : "no", st.read_error), t.text);
        line(util::fmt("Frames a second %.0f", fps_), t.text);
        line(left > 0 ? util::fmt("Back to normal in %.0f s", left) : std::string("Everything normal"), left > 0 ? t.warn : t.text2);
        line("Sound test: UI clicks and a beep every 2 s", t.text2);
        line("A dark GamePad: its buttons and touch show below.", t.text2);
        line("Starting CoffeeFlix turns a dark GamePad back on.", t.text2);

        float ny = y + 300;
        text::draw(font::small_bold, x + 6, ny, "Events", t.accent);
        ny += 28;
        for (const std::string& n : notes_) {
            text::draw_fit(font::caption, x + 6, ny, w - 12, n, t.text2);
            ny += 22;
        }
    }

    bool ui_sounds_;
    double opened_;
    platform::PadOutputs out_;
    int revert_ = 0;
    double changed_at_ = 0;
    bool beeps_ = true;
    double last_beep_ = 0;
    float fps_ = 60;
    uint32_t held_ = 0;
    bool touching_ = false;
    std::deque<std::string> notes_;
};

}  // namespace

std::unique_ptr<app::Screen> make_gamepad_lab() { return std::make_unique<GamePadLab>(); }

}  // namespace screens
