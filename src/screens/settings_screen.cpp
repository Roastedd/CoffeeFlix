// Settings: appearance, playback quality, region, network, accounts, about.
#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/jellyfin.hpp"
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
const Choice SCREENSAVER{"screensaver", {0, 120, 300, 600, 1200}, {"Off", "After 2 min", "After 5 min", "After 10 min", "After 20 min"}, 300};
const char* COUNTRIES[] = {"US", "GB", "CA", "AU", "IE", "DE", "FR", "ES", "IT", "NL", "SE", "PL", "BR", "MX", "JP", "KR", "IN"};

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
        text::draw(font::display, x0, y, "Settings", t.text);
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

        header("APPEARANCE");
        {
            Id iid = id(g, "accent");
            float top = row_h(64);
            int a = (int)store::get_int("accent", 0);
            if (value_row(iid, Rect(x0, top, w, 64), "Accent color", accent_name(a), ic::PALETTE, g, F_DEFAULT)) {
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
            if (toggle_row(iid, Rect(x0, top, w, 82), "Interface sounds", "Little clicks and whooshes as you navigate", &v, g)) {
                store::set_bool("ui_sounds", v);
                audio::set_sfx_enabled(v);
            }
            track(iid, top, 82);
        }
        choice_row(g, "saver", SCREENSAVER, "Screensaver", ic::BRIGHTNESS, x0, w, y);

        header("PLAYBACK");
        choice_row(g, "yt_q", YT_QUALITY, "YouTube quality", ic::SMART_DISPLAY, x0, w, y);
        choice_row(g, "jf_q", JF_QUALITY, "Jellyfin quality", ic::VIDEO_LIBRARY, x0, w, y);
        choice_row(g, "tw_q", TW_QUALITY, "Twitch quality", ic::LIVE_TV, x0, w, y);
        bool_row(g, "60fps", "allow_60fps", false, "Allow 60 fps streams", "Smoother but harder for the Wii U; may drop frames", x0, w, y);
        bool_row(g, "subs", "subs_default_on", true, "Subtitles on by default", "When a video comes with subtitles", x0, w, y);
        bool_row(g, "ytcc", "yt_captions", false, "YouTube captions", "Turn captions on automatically (your language first)", x0, w, y);
        bool_row(g, "ytsb", "yt_sponsorblock", true, "Skip sponsors on YouTube",
                 "SponsorBlock: jumps over sponsor segments, self-promotion and subscribe reminders", x0, w, y);

        header("REGION");
        {
            Id iid = id(g, "country");
            float top = row_h(64);
            std::string cc = store::get_str("radio_country", "US");
            if (value_row(iid, Rect(x0, top, w, 64), "Country", cc.c_str(), ic::PUBLIC, g)) {
                size_t n = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]), i = 0;
                while (i < n && cc != COUNTRIES[i]) i++;
                cc = COUNTRIES[(i + 1) % n];
                store::set_str("radio_country", cc);
                store::set_str("yt_region", cc);
            }
            track(iid, top, 64);
        }

        header("NETWORK");
        {
            Id iid = id(g, "tls");
            float top = row_h(82);
            bool v = store::get_bool("verify_tls", true);
            if (toggle_row(iid, Rect(x0, top, w, 82), "Verify secure connections",
                           "Turn off only if everything fails because the console clock is wrong", &v, g)) {
                store::set_bool("verify_tls", v);
                http::set_verify_tls(v);
            }
            track(iid, top, 82);
        }
        {
            Id iid = id(g, "ip");
            float top = row_h(64);
            std::string ip = platform::ip_address();
            value_row(iid, Rect(x0, top, w, 64), "IP address", ip.empty() ? "Not connected" : ip.c_str(), ic::WIFI, g);
            track(iid, top, 64);
        }

        header("ACCOUNTS & DATA");
        {
            Id iid = id(g, "jf");
            float top = row_h(64);
            const jellyfin::Account& a = jellyfin::account();
            std::string v = a.valid() ? a.user_name + " on " + (a.server_name.empty() ? a.server : a.server_name) : "Not connected";
            if (value_row(iid, Rect(x0, top, w, 64), a.valid() ? "Jellyfin \xC2\xB7 Sign out" : "Jellyfin", v.c_str(),
                          ic::VIDEO_LIBRARY, g)) {
                if (a.valid()) {
                    jellyfin::sign_out();
                    toast("Signed out of Jellyfin", ic::LOGOUT);
                } else {
                    app::open_section(app::SEC_JELLYFIN);
                }
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "history");
            float top = row_h(64);
            size_t n = store::resume_list(100).size();
            std::string v = n ? util::fmt("%zu items", n) : "Empty";
            if (value_row(iid, Rect(x0, top, w, 64), "Clear watch history", v.c_str(), ic::HISTORY, g) && n) {
                store::resume_clear();
                toast("Watch history cleared", ic::DELETE);
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "recs");
            float top = row_h(64);
            bool learned = yt_recs::has_profile();
            if (value_row(iid, Rect(x0, top, w, 64), "Reset YouTube recommendations", learned ? "Learning" : "Nothing yet",
                          ic::AUTO_AWESOME, g) && learned) {
                yt_recs::reset();
                toast("YouTube recommendations reset", ic::DELETE);
            }
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "ytimport");
            float top = row_h(64);
            if (value_row(iid, Rect(x0, top, w, 64), "Import YouTube subscriptions", "Takeout or NewPipe file", ic::FILE_DOWNLOAD, g))
                toast(yt::import_subscriptions(), ic::SUBSCRIPTIONS);
            track(iid, top, 64);
        }
        {
            Id iid = id(g, "ytexport");
            float top = row_h(64);
            size_t n = yt::subscriptions().size();
            std::string v = n ? util::fmt("%zu channels", n) : "None yet";
            if (value_row(iid, Rect(x0, top, w, 64), "Export YouTube subscriptions", v.c_str(), ic::FILE_UPLOAD, g) && n)
                toast(yt::export_subscriptions(), ic::SUBSCRIPTIONS);
            track(iid, top, 64);
        }

        header("ABOUT");
        {
            float top = row_h(150);
            Rect r(x0, top, w, 150);
            gfx::fill_rrect(r, 16, t.surface);
            gfx::fill_rrect_hgrad(Rect(r.x + 24, r.y + 26, 56, 56), 16, t.accent, t.accent2);
            text::icon(ic::LOCAL_CAFE, 32, r.x + 52, r.y + 54, gfx::rgb(0x1A1016));
            text::draw(font::title, r.x + 100, r.y + 24, "CoffeeFlix 2.0", t.text);
            text::draw(font::small, r.x + 100, r.y + 60, util::fmt("%s \xC2\xB7 built %s", platform::name(), __DATE__), t.text3);
            text::draw_wrapped(font::small, Rect(r.x + 24, r.y + 96, r.w - 48, 50),
                               "Free for noncommercial use (PolyForm Noncommercial). Not affiliated with Nintendo, "
                               "YouTube, Twitch or Jellyfin.",
                               t.text3, 2);
            Id iid = id(g, "about");
            Item it = focusable(iid, r, g);
            focus_ring(r, 16, it.f);
            track(iid, top, 150);
        }
        {
            Id iid = id(g, "licenses");
            float top = row_h(64);
            if (value_row(iid, Rect(x0, top, w, 64), "Licenses", "CoffeeFlix and the software it includes", ic::DESCRIPTION, g))
                app::push(make_licenses());
            track(iid, top, 64);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Change"}, {"B", "Back"}});
    }

private:
    void choice_row(Id g, const char* key, const Choice& c, const char* label, int icon, float x0, float w, float& y) {
        Id iid = id(g, key);
        float top = y;
        int cur = (int)store::get_int(c.key, c.def);
        size_t idx = 0;
        while (idx < c.values.size() && c.values[idx] != cur) idx++;
        if (idx >= c.values.size()) idx = 0;
        if (value_row(iid, Rect(x0, top, w, 64), label, c.labels[idx], icon, g)) {
            idx = (idx + 1) % c.values.size();
            store::set_int(c.key, c.values[idx]);
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
