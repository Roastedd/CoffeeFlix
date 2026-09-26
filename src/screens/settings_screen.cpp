// Settings: appearance, playback quality, region, network, accounts, about.
#include <cstring>

#include "app/updater.hpp"
#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/jellyfin.hpp"
#include "services/yt_account.hpp"
#include "services/yt_recs.hpp"
#include "screens/youtube_common.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

struct Choice {
    const char* key;
    std::vector<int> values;
    std::vector<const char*> labels;
    int def;
};

const Choice YT_QUALITY{"yt_quality", {360, 480, 720, 1080}, {"360p", "480p", "720p", "1080p"}, 720};
const Choice JF_QUALITY{"jf_quality", {480, 720, 1080}, {"480p", "720p", "1080p"}, 1080};
const Choice TW_QUALITY{"twitch_quality", {360, 480, 720, 1080}, {"360p", "480p", "720p", "1080p"}, 720};
// Wii U hardware H.264 decoding: 0 every picture, 1 without pictures nothing refers to (most
// B-frames; fewer frames per second but what the original FFmpeg-wiiu did), 2 software only. The
// player steps down by itself when the hardware decoder gives no pictures.
const Choice VIDEO_DECODING{"video_decoding", {0, 1, 2}, {N_("Hardware"), N_("Hardware, fewer frames"), N_("Software")}, 0};
const Choice SCREENSAVER{"screensaver", {0, 120, 300, 600, 1200}, {N_("Off"), N_("After 2 min"), N_("After 5 min"), N_("After 10 min"), N_("After 20 min")}, 300};
const char* COUNTRIES[] = {"US", "GB", "CA", "AU", "IE", "DE", "FR", "ES", "IT", "NL", "SE", "PL", "BR", "MX", "JP", "KR", "IN"};

// Settings > Language: the console's language, or one chosen here.
class LanguageScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = 760;
        Id g = id("language");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Language"), t.text);
        y += 90;
        std::string chosen = i18n::chosen();
        auto row = [&](const char* code, const char* label, const char* value) {
            Id iid = id(g, code);
            bool on = chosen == code;
            if (value_row(iid, Rect(x0, y, w, 64), label, value, on ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED, g,
                          on ? F_DEFAULT : 0) &&
                !on)
                i18n::choose(code);
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + 64 + page_.scroll() + 20);
            y += 74;
        };
        std::string same = util::fmt(tr("Same as the Wii U (%s)"), i18n::system().name);
        row("", same.c_str(), nullptr);
        for (int i = 0; i < i18n::LANGUAGE_COUNT; i++) {
            const i18n::Language& l = i18n::LANGUAGES[i];
            const char* english = tr(l.english);
            row(l.code, l.name, strcmp(english, l.name) ? english : nullptr);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

class SettingsScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        float w = 760;
        Id g = id("settings");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, tr("Settings"), t.text);
        y += 90;

        auto header = [&](const char* s) {
            text::draw(font::small_bold, x0 + 6, y + 10, s, t.accent);
            y += 44;
        };
        auto row_h = [&](float h) {
            float top = y;
            y += h + 10;
            return top;
        };
        auto track = [&](Id iid, float top, float h) {
            if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + h + page_.scroll() + 20);
        };

        header(tr("LANGUAGE"));
        {
            Id iid = id(g, "language");
            float top = row_h(64);
            // Also in English, for whoever chose a language they can't read.
            std::string label = tr("Language");
            if (label != "Language") label += " (Language)";
            if (value_row(iid, Rect(x0, top, w, 64), label.c_str(), i18n::current().name, ic::TRANSLATE, g, F_DEFAULT))
                app::push(std::make_unique<LanguageScreen>());
            track(iid, top, 64);
        }

        header(tr("APPEARANCE"));
        {
            Id iid = id(g, "accent");
            float top = row_h(64);
            int a = (int)store::get_int("accent", 0);
            if (value_row(iid, Rect(x0, top, w, 64), tr("Accent color"), accent_name(a), ic::PALETTE, g)) {
                a = (a + 1) % accent_count();
                store::set_int("accent", a);
                set_accent(a);
            }
            // Swatches
            for (int i = 0; i < accent_count(); i++) {
                float sx = x0 + w + 30 + i * 34;
                set_accent(i);
                Color c1 = theme().accent, c2 = theme().accent2;
                set_accent(a);
                gfx::fill_rrect_hgrad(Rect(sx, top + 18, 26, 26), 13, c1, c2);
                if (i == a) gfx::stroke_circle(sx + 13, top + 31, 17, 2, gfx::WHITE);
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "sounds");
            float top = row_h(82);
            bool v = store::get_bool("ui_sounds", true);
            if (toggle_row(iid, Rect(x0, top, w, 82), tr("Interface sounds"), tr("Little clicks and whooshes as you navigate"), &v, g)) {
                store::set_bool("ui_sounds", v);
                audio::set_sfx_enabled(v);
            }
            track(iid, top, 82);
        }
        choice_row(g, "saver", SCREENSAVER, tr("Screensaver"), ic::BRIGHTNESS, x0, w, y);

        header(tr("PLAYBACK"));
        choice_row(g, "yt_q", YT_QUALITY, tr("YouTube quality"), ic::SMART_DISPLAY, x0, w, y);
        choice_row(g, "jf_q", JF_QUALITY, tr("Jellyfin quality"), ic::VIDEO_LIBRARY, x0, w, y);
        choice_row(g, "tw_q", TW_QUALITY, tr("Twitch quality"), ic::LIVE_TV, x0, w, y);
        bool_row(g, "60fps", "allow_60fps", true, tr("Allow 60 fps streams"), tr("Up to 720p; 1080p stays at 30 fps"), x0, w, y);
        if (platform::is_wiiu()) choice_row(g, "vdec", VIDEO_DECODING, tr("Video decoding"), ic::TUNE, x0, w, y);
        bool_row(g, "subs", "subs_default_on", true, tr("Subtitles on by default"), tr("When a video comes with subtitles"), x0, w, y);
        bool_row(g, "ytcc", "yt_captions", false, tr("YouTube captions"), tr("Turn captions on automatically (your language first)"), x0, w, y);
        bool_row(g, "ytsb", "yt_sponsorblock", true, tr("Skip sponsors on YouTube"),
                 tr("SponsorBlock: jumps over sponsor segments, self-promotion and subscribe reminders"), x0, w, y);

        header(tr("REGION"));
        {
            Id iid = id(g, "country");
            float top = row_h(64);
            std::string cc = store::get_str("radio_country", "US");
            if (value_row(iid, Rect(x0, top, w, 64), tr("Country"), cc.c_str(), ic::PUBLIC, g)) {
                size_t n = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]), i = 0;
                while (i < n && cc != COUNTRIES[i]) i++;
                cc = COUNTRIES[(i + 1) % n];
                store::set_str("radio_country", cc);
                store::set_str("yt_region", cc);
            }
            track(iid, top, 64);
        }

        header(tr("NETWORK"));
        {
            Id iid = id(g, "tls");
            float top = row_h(82);
            bool v = store::get_bool("verify_tls", true);
            if (toggle_row(iid, Rect(x0, top, w, 82), tr("Verify secure connections"),
                           tr("Turn off only if everything fails because the console clock is wrong"), &v, g)) {
                store::set_bool("verify_tls", v);
                http::set_verify_tls(v);
            }
            track(iid, top, 82);
        }
        {
            Id iid = id(g, "ip");
            float top = row_h(64);
            std::string ip = platform::ip_address();
            value_row(iid, Rect(x0, top, w, 64), tr("IP address"), ip.empty() ? tr("Not connected") : ip.c_str(), ic::WIFI, g);
            track(iid, top, 64);
        }

        header(tr("ACCOUNTS & DATA"));
        {
            Id iid = id(g, "jf");
            float top = row_h(64);
            const jellyfin::Account& a = jellyfin::account();
            std::string v = a.valid() ? util::fmt(tr("%s on %s"), a.user_name.c_str(),
                                                  (a.server_name.empty() ? a.server : a.server_name).c_str())
                                      : tr("Not connected");
            if (value_row(iid, Rect(x0, top, w, 64), a.valid() ? tr("Jellyfin \xC2\xB7 Sign out") : "Jellyfin", v.c_str(),
                          ic::VIDEO_LIBRARY, g)) {
                if (a.valid()) {
                    jellyfin::sign_out();
                    toast(tr("Signed out of Jellyfin"), ic::LOGOUT);
                } else {
                    app::open_section(app::SEC_JELLYFIN);
                }
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "yt");
            float top = row_h(64);
            bool in = yt_account::signed_in();
            if (value_row(iid, Rect(x0, top, w, 64), in ? tr("YouTube \xC2\xB7 Sign out") : "YouTube",
                          in ? yt_account::name().c_str() : tr("Not signed in (optional)"), ic::SMART_DISPLAY, g)) {
                if (in) {
                    yt_account::sign_out();
                    toast(tr("Signed out of YouTube"), ic::LOGOUT);
                } else {
                    app::push(yt::make_sign_in());
                }
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "history");
            float top = row_h(64);
            size_t n = store::resume_list(100).size();
            std::string v = n ? util::fmt(tr("%zu items"), n) : tr("Empty");
            if (value_row(iid, Rect(x0, top, w, 64), tr("Clear watch history"), v.c_str(), ic::HISTORY, g) && n) {
                store::resume_clear();
                toast(tr("Watch history cleared"), ic::DELETE);
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "recs");
            float top = row_h(64);
            bool learned = yt_recs::has_profile();
            if (value_row(iid, Rect(x0, top, w, 64), tr("Reset YouTube recommendations"), learned ? tr("Learning") : tr("Nothing yet"),
                          ic::AUTO_AWESOME, g) && learned) {
                yt_recs::reset();
                toast(tr("YouTube recommendations reset"), ic::DELETE);
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "ytimport");
            float top = row_h(64);
            if (value_row(iid, Rect(x0, top, w, 64), tr("Import YouTube subscriptions"), tr("Takeout or NewPipe file"), ic::FILE_DOWNLOAD, g))
                toast(yt::import_subscriptions(), ic::SUBSCRIPTIONS);
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "ytexport");
            float top = row_h(64);
            size_t n = yt::subscriptions().size();
            std::string v = n ? util::fmt(tr("%zu channels"), n) : tr("None yet");
            if (value_row(iid, Rect(x0, top, w, 64), tr("Export YouTube subscriptions"), v.c_str(), ic::FILE_UPLOAD, g) && n)
                toast(yt::export_subscriptions(), ic::SUBSCRIPTIONS);
            track(iid, top, 64);
        }

        header(tr("UPDATES"));
        {
            Id iid = id(g, "update");
            float top = row_h(64);
            std::string label = tr("Check for updates"), value;
            const char* nv = updater::release().version.c_str();
            bool dev = updater::developer();
            switch (updater::supported() ? updater::state() : updater::IDLE) {
                case updater::CHECKING: value = tr("Checking\xE2\x80\xA6"); break;
                case updater::UP_TO_DATE: value = tr("Up to date"); break;
                case updater::AVAILABLE:
                    label = util::fmt(dev ? tr("Install developer build %s") : tr("Update to CoffeeFlix %s"), nv);
                    value = tr("Available");
                    break;
                case updater::DOWNLOADING:
                    label = util::fmt(dev ? tr("Downloading developer build %s") : tr("Downloading CoffeeFlix %s"), nv);
                    value = util::fmt("%d%%", (int)(updater::progress() * 100));
                    break;
                case updater::READY:
                    label = dev ? tr("The developer build is ready") : util::fmt(tr("CoffeeFlix %s is ready"), nv);
                    value = tr("Installs when you close");
                    break;
                case updater::FAILED: value = tr("Didn't work"); break;
                default: value = updater::supported() ? "" : tr("Not available here"); break;
            }
            if (value_row(iid, Rect(x0, top, w, 64), label.c_str(), value.c_str(), ic::SYSTEM_UPDATE, g))
                app::push(make_update());
            track(iid, top, 64);
        }
        if (updater::supported()) {
            Id iid = id(g, "autoupdate");
            float top = row_h(82);
            bool v = updater::automatic();
            if (toggle_row(iid, Rect(x0, top, w, 82), tr("Install updates automatically"),
                           tr("New versions download in the background and install when you close CoffeeFlix"), &v, g))
                updater::set_automatic(v);
            track(iid, top, 82);
        }
        if (updater::supported() && updater::dev_unlocked()) {
            Id iid = id(g, "devupdates");
            float top = row_h(82);
            bool v = updater::developer();
            if (toggle_row(iid, Rect(x0, top, w, 82), tr("Developer updates"),
                           tr("Test builds from tools/dev-update.sh on your computer, instead of releases"), &v, g) &&
                !updater::set_developer(v))
                toast(tr("Cancel the download first."), ic::INFO);
            track(iid, top, 82);
        }
        if (updater::has_previous()) {
            Id iid = id(g, "previous");
            float top = row_h(64);
            std::string pv = updater::previous_version();
            std::string name = "CoffeeFlix " + pv;
            if (value_row(iid, Rect(x0, top, w, 64), tr("Switch to the previous version"),
                          pv.empty() ? tr("Kept from before the last update") : name.c_str(), ic::RESTORE, g)) {
                show_menu(pv.empty() ? tr("Switch to the previous version?") : util::fmt(tr("Switch to %s?"), name.c_str()),
                          tr("Then start it again from the Wii U Menu."),
                          {{tr("Switch and close CoffeeFlix"), ic::RESTORE, [] { updater::switch_to_previous(); }},
                           {tr("Cancel"), ic::CLOSE, [] {}}});
            }
            track(iid, top, 64);
        }

        header(tr("ABOUT"));
        {
            float top = row_h(150);
            Rect r(x0, top, w, 150);
            gfx::fill_rrect(r, 16, t.surface);
            gfx::fill_rrect_hgrad(Rect(r.x + 24, r.y + 26, 56, 56), 16, t.accent, t.accent2);
            text::icon(ic::LOCAL_CAFE, 32, r.x + 52, r.y + 54, gfx::rgb(0x1A1016));
            text::draw(font::title, r.x + 100, r.y + 24, std::string("CoffeeFlix ") + updater::version(), t.text);
            text::draw(font::small, r.x + 100, r.y + 60, util::fmt(tr("%s \xC2\xB7 built %s"), platform::name(), __DATE__), t.text3);
            text::draw_wrapped(font::small, Rect(r.x + 24, r.y + 96, r.w - 48, 50),
                               tr("Free for noncommercial use (PolyForm Noncommercial). Not affiliated with Nintendo, "
                                  "YouTube, Twitch or Jellyfin."),
                               t.text3, 2);
            Id iid = id(g, "about");
            Item it = focusable(iid, r, g);
            focus_ring(r, 16, it.f);
            track(iid, top, 150);
        }
        {
            Id iid = id(g, "licenses");
            float top = row_h(64);
            if (value_row(iid, Rect(x0, top, w, 64), tr("Licenses"), tr("CoffeeFlix and the software it includes"), ic::DESCRIPTION, g))
                app::push(make_licenses());
            track(iid, top, 64);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Change")}, {"B", tr("Back")}});
    }

private:
    void choice_row(Id g, const char* key, const Choice& c, const char* label, int icon, float x0, float w, float& y) {
        Id iid = id(g, key);
        float top = y;
        int cur = (int)store::get_int(c.key, c.def);
        size_t idx = 0;
        while (idx < c.values.size() && c.values[idx] != cur) idx++;
        if (idx >= c.values.size()) idx = 0;
        if (value_row(iid, Rect(x0, top, w, 64), label, tr(c.labels[idx]), icon, g)) {
            idx = (idx + 1) % c.values.size();
            store::set_int(c.key, c.values[idx]);
            if (!std::strcmp(c.key, "video_decoding")) player::reset_decoding_fallback();
        }
        if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + 64 + page_.scroll() + 20);
        y += 74;
    }

    void bool_row(Id g, const char* id_key, const char* key, bool def, const char* label, const char* desc, float x0, float w,
                  float& y) {
        Id iid = id(g, id_key);
        float top = y;
        bool v = store::get_bool(key, def);
        if (toggle_row(iid, Rect(x0, top, w, 82), label, desc, &v, g)) store::set_bool(key, v);
        if (focused() == iid) page_.focus_range(top + page_.scroll() - 60, top + 82 + page_.scroll() + 20);
        y += 92;
    }

    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_settings() { return std::make_unique<SettingsScreen>(); }

}  // namespace screens
