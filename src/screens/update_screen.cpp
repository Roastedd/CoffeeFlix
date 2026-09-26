// Settings > Updates: what's new in the latest release, downloading it, and installing it as
// CoffeeFlix closes (app/updater).
#include "app/updater.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

std::string ago(int64_t when) {
    if (when <= 0) return "never";
    int64_t s = std::max<int64_t>(0, util::unix_time() - when);
    if (s < 90) return "just now";
    if (s < 3600) return util::fmt("%d minutes ago", (int)(s / 60));
    if (s < 2 * 86400) return util::fmt("%d hours ago", (int)(s / 3600));
    return util::fmt("%d days ago", (int)(s / 86400));
}

class UpdateScreen : public app::Screen {
public:
    UpdateScreen() {
        // Look again, unless that was just now (developer builds change more often than that).
        updater::State s = updater::state();
        bool recent = !updater::developer() && util::unix_time() - updater::last_checked() < 600;
        if (s == updater::IDLE || s == updater::FAILED || (s == updater::UP_TO_DATE && !recent)) updater::check();
    }
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        secret_code();
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("update");
        updater::State s = updater::state();
        bool dev = updater::developer();
        const char* src = dev ? "your computer" : "GitHub";
        if (s != shown_) {
            shown_ = s;
            reset_focus();
        }
        const updater::Release& rel = updater::release();
        const char* v = rel.version.c_str();
        bool offer = s == updater::AVAILABLE || s == updater::DOWNLOADING || s == updater::READY ||
                     (s == updater::FAILED && !rel.url.empty());

        std::string title = !updater::supported()                ? "Updates"
                            : dev && s == updater::AVAILABLE    ? "New developer build"
                            : dev && s == updater::DOWNLOADING  ? "Downloading the developer build"
                            : dev && s == updater::READY        ? "The developer build is ready"
                            : dev && s != updater::FAILED       ? "Developer updates"
                            : s == updater::UP_TO_DATE          ? "You're up to date"
                            : s == updater::AVAILABLE           ? util::fmt("CoffeeFlix %s is available", v)
                            : s == updater::DOWNLOADING         ? util::fmt("Downloading CoffeeFlix %s", v)
                            : s == updater::READY               ? util::fmt("CoffeeFlix %s is ready", v)
                            : s == updater::FAILED              ? "Couldn't update"
                                                                : "Updates";
        std::string sub = util::fmt("You have CoffeeFlix %s", updater::version());
        if (dev) sub += " \xC2\xB7 builds from " + (updater::dev_server().empty() ? std::string("your computer") : updater::dev_server());
        else if (offer && rel.size > 0) sub += " \xC2\xB7 the new one is " + util::format_bytes(rel.size);
        text::draw(font::display, x0, 60, title, t.text);
        text::draw(font::body, x0 + 2, 122, sub, t.text2);

        Rect panel(x0, 176, W - x0 - 60, 400);
        gfx::shadow(panel, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(panel, 26, Color(255, 255, 255, 12));
        gfx::stroke_rrect(panel, 26, 1, Color(255, 255, 255, 18));
        float px = panel.x + 48, pw = panel.w - 96;

        if (!updater::supported()) {
            message(panel, ic::INFO, t.text2, "Not available here", updater::unsupported_reason());
            hint_bar({{"B", "Back"}});
            return;
        }
        if (s == updater::CHECKING || s == updater::IDLE) {
            // Holds focus here until there's a button, so the sidebar doesn't open meanwhile.
            focusable(id(g, "wait"), panel, 0, F_DEFAULT | F_SILENT);
            loading_indicator(panel.cx(), panel.cy() - 10,
                              rel.version.empty() ? "Checking for updates\xE2\x80\xA6" : "Checking the download\xE2\x80\xA6");
            hint_bar({{"B", "Back"}});
            return;
        }
        if (s == updater::UP_TO_DATE) {
            message(panel, ic::CHECK_CIRCLE, t.good, dev ? "You have the latest developer build" : "You have the latest version",
                    dev ? "Build again with tools/dev-update.sh, then look again here." : "Checked " + ago(updater::last_checked()) + ".");
        } else if (s == updater::FAILED && !offer) {
            message(panel, ic::ERROR_OUTLINE, t.bad, dev ? "No developer build" : "Couldn't check for updates", updater::error());
        } else {
            // What's new, then where things stand along the bottom.
            text::draw(font::small_bold, px, panel.y + 40, util::fmt("WHAT'S NEW IN %s", v), t.accent);
            std::string notes = !rel.notes.empty() ? rel.notes
                                : dev              ? "No notes came with this build."
                                                   : "See github.com/Roastedd/CoffeeFlix/releases for what changed.";
            text::draw_wrapped(font::body, Rect(px, panel.y + 76, pw, 220), notes, t.text, 8);

            float sy = panel.b() - 70;
            gfx::fill_rect(Rect(px, sy - 18, pw, 1), Color(255, 255, 255, 18));
            if (s == updater::DOWNLOADING) {
                progress_bar(Rect(px, sy + 8, pw, 8), updater::progress());
                std::string p = updater::cancelling() ? std::string("Stopping\xE2\x80\xA6")
                                : updater::progress() >= 1 ? util::fmt("Checking it against %s's checksum\xE2\x80\xA6", src)
                                                           : util::fmt("%s of %s", util::format_bytes((uint64_t)(updater::progress() * rel.size)).c_str(),
                                                                       util::format_bytes(rel.size).c_str());
                text::draw(font::small, px, sy + 26, p, t.text2);
            } else if (s == updater::READY) {
                text::icon(ic::VERIFIED, 24, px + 12, sy + 16, t.good);
                text::draw(font::small_bold, px + 34, sy + 6,
                           dev ? "Downloaded, checked and signed with your key. It installs when you close CoffeeFlix."
                               : "Downloaded and checked against GitHub's checksum. It installs when you close CoffeeFlix.",
                           t.text2);
            } else if (s == updater::FAILED) {
                text::icon(ic::ERROR_OUTLINE, 24, px + 12, sy + 16, t.bad);
                text::draw_fit(font::small_bold, px + 34, sy + 6, pw - 34, updater::error(), t.bad);
            } else {
                text::icon(ic::VERIFIED, 24, px + 12, sy + 16, t.text3);
                text::draw(font::small, px + 34, sy + 6,
                           dev ? "From your computer, signed with your developer key, and checked before anything changes."
                               : "From CoffeeFlix's GitHub releases, and checked against GitHub's checksum before anything changes.",
                           t.text3);
            }
        }

        // Below the panel: a note on the left, the buttons on the right.
        float by = panel.b() + 22;
        Id bg = id(g, "buttons");
        float right = panel.r();
        auto put = [&](const char* key, const char* label, int icon, int style, bool main) {
            float bw = measure_button(label, icon);
            right -= bw;
            bool hit = button(id(bg, key), Rect(right, by, bw, 46), label, icon, style, bg, main ? F_DEFAULT : 0);
            right -= 14;
            return hit;
        };
        const char* note = offer ? "The version you have now is kept, and you can switch back to it in Settings."
                           : dev ? "To get releases again, turn off Developer updates in Settings."
                                 : "";
        switch (s) {
            case updater::AVAILABLE:
                if (put("download", "Download", ic::CLOUD_DOWNLOAD, BTN_PRIMARY, true)) updater::download();
                if (put("later", "Not now", 0, BTN_GHOST, false)) {
                    updater::cancel();
                    app::pop();
                    return;
                }
                break;
            case updater::DOWNLOADING:
                if (!updater::cancelling() && put("cancel", "Cancel", ic::CLOSE, BTN_NORMAL, true)) updater::cancel();
                break;
            case updater::READY:
                note = "CoffeeFlix closes and goes back to the Wii U Menu. Start it again from there.";
                if (put("install", "Install and close", ic::SYSTEM_UPDATE, BTN_PRIMARY, true)) updater::install_now();
                if (put("drop", "Don't install", 0, BTN_GHOST, false)) updater::cancel();
                break;
            case updater::FAILED:
                if (offer) {
                    if (put("retry", "Try again", ic::REFRESH, BTN_PRIMARY, true)) updater::download();
                } else if (put("check", "Check again", ic::REFRESH, BTN_PRIMARY, true)) {
                    updater::check();
                }
                break;
            default:
                if (put("check", "Check again", ic::REFRESH, BTN_NORMAL, true)) updater::check();
                break;
        }
        text::draw_wrapped(font::small, Rect(x0 + 4, by + 2, right - x0 - 30, 46), note, t.text3, 2);
        hint_bar({{"A", "Select"}, {"B", "Back"}});
    }

private:
    // Up Up Down Down Left Right Left Right B A shows developer updates (Settings), or hides
    // them again. B and A count toward the code instead of going back or pressing a button.
    void secret_code() {
        static const Button CODE[] = {BTN_UP, BTN_UP, BTN_DOWN, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_LEFT, BTN_RIGHT, BTN_B, BTN_A};
        const int n = sizeof(CODE) / sizeof(CODE[0]);
        Input& in = input();
        for (int b = 0; b < BTN_COUNT; b++) {
            if (!in.pressed_((Button)b)) continue;
            bool match = b == CODE[code_];
            if (match && (b == BTN_B || b == BTN_A)) in.eat((Button)b);
            if (match && code_ >= 4) suspend_nav();  // Left doesn't open the sidebar partway through
            code_ = match ? code_ + 1 : b == BTN_UP ? (code_ == 2 ? 2 : 1) : 0;
            if (code_ < n) continue;
            code_ = 0;
            bool unlock = !updater::dev_unlocked();
            if (!updater::set_dev_unlocked(unlock)) {
                toast("Cancel the download first.", ic::INFO);
                continue;
            }
            if (unlock) updater::set_developer(true);
            platform::rumble(0.25f);
            toast(unlock ? "Developer updates are on. Builds come from tools/dev-update.sh on your computer."
                         : "Developer updates are off, and hidden again.",
                  unlock ? ic::KEY : ic::LOCK);
        }
    }

    void message(const Rect& panel, int icon, Color c, const std::string& head, const std::string& body) {
        const Theme& t = theme();
        text::icon(icon, 64, panel.cx(), panel.y + 110, c);
        text::draw(font::headline, panel.cx(), panel.y + 164, head, t.text, text::CENTER);
        text::draw_wrapped(font::body, Rect(panel.cx() - 330, panel.y + 222, 660, 120), body, t.text2, 4, text::CENTER);
    }

    updater::State shown_ = updater::IDLE;
    int code_ = 0;  // how much of the secret code has been pressed
};

}  // namespace

std::unique_ptr<app::Screen> make_update() { return std::make_unique<UpdateScreen>(); }

}  // namespace screens
