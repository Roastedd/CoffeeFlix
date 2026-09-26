// Settings > Updates: what's new in the latest release, downloading it, and installing it as
// CoffeeFlix closes (app/updater).
#include "app/updater.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

std::string checked(int64_t when) {
    if (when <= 0) return "";
    int64_t s = std::max<int64_t>(0, util::unix_time() - when);
    if (s < 90) return tr("Checked just now.");
    if (s < 3600) return util::fmt(tr("Checked %d minutes ago."), (int)(s / 60));
    if (s < 2 * 86400) return util::fmt(tr("Checked %d hours ago."), (int)(s / 3600));
    return util::fmt(tr("Checked %d days ago."), (int)(s / 86400));
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
        if (s != shown_) {
            shown_ = s;
            reset_focus();
        }
        const updater::Release& rel = updater::release();
        const char* v = rel.version.c_str();
        bool offer = s == updater::AVAILABLE || s == updater::DOWNLOADING || s == updater::READY ||
                     (s == updater::FAILED && !rel.url.empty());

        std::string title = !updater::supported()                ? tr("Updates")
                            : dev && s == updater::AVAILABLE    ? tr("New developer build")
                            : dev && s == updater::DOWNLOADING  ? tr("Downloading the developer build")
                            : dev && s == updater::READY        ? tr("The developer build is ready")
                            : dev && s != updater::FAILED       ? tr("Developer updates")
                            : s == updater::UP_TO_DATE          ? tr("You're up to date")
                            : s == updater::AVAILABLE           ? util::fmt(tr("CoffeeFlix %s is available"), v)
                            : s == updater::DOWNLOADING         ? util::fmt(tr("Downloading CoffeeFlix %s"), v)
                            : s == updater::READY               ? util::fmt(tr("CoffeeFlix %s is ready"), v)
                            : s == updater::FAILED              ? tr("Couldn't update")
                                                                : tr("Updates");
        std::string sub =
            dev && !updater::dev_server().empty()
                ? util::fmt(tr("You have CoffeeFlix %s \xC2\xB7 builds from %s"), updater::version(), updater::dev_server().c_str())
            : dev ? util::fmt(tr("You have CoffeeFlix %s \xC2\xB7 builds from your computer"), updater::version())
            : offer && rel.size > 0
                ? util::fmt(tr("You have CoffeeFlix %s \xC2\xB7 the new one is %s"), updater::version(),
                            util::format_bytes(rel.size).c_str())
                : util::fmt(tr("You have CoffeeFlix %s"), updater::version());
        text::draw_fit(font::display, x0, 60, W - x0 - 60, title, t.text);
        text::draw_fit(font::body, x0 + 2, 122, W - x0 - 62, sub, t.text2);

        Rect panel(x0, 176, W - x0 - 60, 400);
        gfx::shadow(panel, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(panel, 26, Color(255, 255, 255, 12));
        gfx::stroke_rrect(panel, 26, 1, Color(255, 255, 255, 18));
        float px = panel.x + 48, pw = panel.w - 96;

        if (!updater::supported()) {
            focusable(id(g, "wait"), panel, 0, F_DEFAULT | F_SILENT);  // keeps the sidebar closed
            message(panel, ic::INFO, t.text2, tr("Not available here"), updater::unsupported_reason());
            hint_bar({{"B", tr("Back")}});
            return;
        }
        if (s == updater::CHECKING || s == updater::IDLE) {
            // Holds focus here until there's a button, so the sidebar doesn't open meanwhile.
            focusable(id(g, "wait"), panel, 0, F_DEFAULT | F_SILENT);
            loading_indicator(panel.cx(), panel.cy() - 10,
                              rel.version.empty() ? tr("Checking for updates\xE2\x80\xA6") : tr("Checking the download\xE2\x80\xA6"));
            hint_bar({{"B", tr("Back")}});
            return;
        }
        if (s == updater::UP_TO_DATE) {
            message(panel, ic::CHECK_CIRCLE, t.good, dev ? tr("You have the latest developer build") : tr("You have the latest version"),
                    dev ? tr("Build again with tools/dev-update.sh, then look again here.") : checked(updater::last_checked()));
        } else if (s == updater::FAILED && !offer) {
            message(panel, ic::ERROR_OUTLINE, t.bad, dev ? tr("No developer build") : tr("Couldn't check for updates"), updater::error());
        } else {
            // What's new, then where things stand along the bottom.
            text::draw(font::small_bold, px, panel.y + 40, util::fmt(tr("WHAT'S NEW IN %s"), v), t.accent);
            std::string notes = !rel.notes.empty() ? rel.notes
                                : dev              ? tr("No notes came with this build.")
                                                   : tr("See github.com/Roastedd/CoffeeFlix/releases for what changed.");
            text::draw_wrapped(font::body, Rect(px, panel.y + 76, pw, 220), notes, t.text, 8);

            float sy = panel.b() - 70;
            gfx::fill_rect(Rect(px, sy - 18, pw, 1), Color(255, 255, 255, 18));
            if (s == updater::DOWNLOADING) {
                progress_bar(Rect(px, sy + 8, pw, 8), updater::progress());
                std::string p = updater::cancelling()          ? std::string(tr("Stopping\xE2\x80\xA6"))
                                : updater::progress() >= 1 && dev ? tr("Checking it against your computer's checksum\xE2\x80\xA6")
                                : updater::progress() >= 1        ? tr("Checking it against GitHub's checksum\xE2\x80\xA6")
                                    : util::fmt(tr("%s of %s"), util::format_bytes((uint64_t)(updater::progress() * rel.size)).c_str(),
                                                util::format_bytes(rel.size).c_str());
                text::draw(font::small, px, sy + 26, p, t.text2);
            } else if (s == updater::READY) {
                text::icon(ic::VERIFIED, 24, px + 12, sy + 16, t.good);
                text::draw_wrapped(font::small_bold, Rect(px + 34, sy + 6, pw - 34, 56),
                                   dev ? tr("Downloaded, checked and signed with your key. It installs when you close CoffeeFlix.")
                                       : tr("Downloaded and checked against GitHub's checksum. It installs when you close CoffeeFlix."),
                                   t.text2, 2);
            } else if (s == updater::FAILED) {
                text::icon(ic::ERROR_OUTLINE, 24, px + 12, sy + 16, t.bad);
                text::draw_fit(font::small_bold, px + 34, sy + 6, pw - 34, updater::error(), t.bad);
            } else {
                text::icon(ic::VERIFIED, 24, px + 12, sy + 16, t.text3);
                text::draw_wrapped(font::small, Rect(px + 34, sy + 6, pw - 34, 56),
                                   dev ? tr("From your computer, signed with your developer key, and checked before anything changes.")
                                       : tr("From CoffeeFlix's GitHub releases, and checked against GitHub's checksum before anything changes."),
                                   t.text3, 2);
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
        const char* note = offer ? tr("The version you have now is kept, and you can switch back to it in Settings.")
                           : dev ? tr("To get releases again, turn off Developer updates in Settings.")
                                 : "";
        switch (s) {
            case updater::AVAILABLE:
                if (put("download", tr("Download"), ic::CLOUD_DOWNLOAD, BTN_PRIMARY, true)) updater::download();
                if (put("later", tr("Not now"), 0, BTN_GHOST, false)) {
                    updater::cancel();
                    app::pop();
                    return;
                }
                break;
            case updater::DOWNLOADING:
                if (!updater::cancelling() && put("cancel", tr("Cancel"), ic::CLOSE, BTN_NORMAL, true)) updater::cancel();
                break;
            case updater::READY:
                note = tr("CoffeeFlix closes and goes back to the Wii U Menu. Start it again from there.");
                if (put("install", tr("Install and close"), ic::SYSTEM_UPDATE, BTN_PRIMARY, true)) updater::install_now();
                if (put("drop", tr("Don't install"), 0, BTN_GHOST, false)) updater::cancel();
                break;
            case updater::FAILED:
                if (offer) {
                    if (put("retry", tr("Try again"), ic::REFRESH, BTN_PRIMARY, true)) updater::download();
                } else if (put("check", tr("Check again"), ic::REFRESH, BTN_PRIMARY, true)) {
                    updater::check();
                }
                break;
            default:
                if (put("check", tr("Check again"), ic::REFRESH, BTN_NORMAL, true)) updater::check();
                break;
        }
        text::draw_wrapped(font::small, Rect(x0 + 4, by + 2, right - x0 - 30, 46), note, t.text3, 2);
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
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
                toast(tr("Cancel the download first."), ic::INFO);
                continue;
            }
            if (unlock) updater::set_developer(true);
            platform::rumble(0.25f);
            toast(unlock ? tr("Developer updates are on. Builds come from tools/dev-update.sh on your computer.")
                         : tr("Developer updates are off, and hidden again."),
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
