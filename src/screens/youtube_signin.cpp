// Signing in to a YouTube account with a code entered on another device (services/yt_account).
#include <cmath>

#include "core/tasks.hpp"
#include "core/util.hpp"
#include "screens/youtube_common.hpp"
#include "services/yt_account.hpp"
#include "ui/ui.hpp"

namespace screens {
namespace yt {

namespace {

using namespace ui;

class SignInScreen : public app::Screen {
public:
    SignInScreen() { request(); }
    app::Section section() const override { return app::SEC_YOUTUBE; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ytsignin");
        float y = 60;
        text::draw(font::display, x0, y, "Sign in to YouTube", t.text);
        text::draw(font::body, x0 + 2, y + 62, "See your subscriptions and what YouTube recommends for you.", t.text2);
        y += 130;

        Rect card(x0, y, 720, 450);
        gfx::shadow(card, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(card, 26, Color(255, 255, 255, 14));
        float cx = card.x + 40, cw = card.w - 80, cy = card.y + 36;

        if (code_.user_code.empty()) {
            if (error_.empty()) {
                loading_indicator(card.cx(), card.cy() - 10, "Getting a code\xE2\x80\xA6");
            } else if (empty_state_action(id(g, "retry"), card, ic::WIFI_OFF, "Couldn't start signing in", error_.c_str())) {
                request();
            }
            hint_bar({{"B", "Back"}});
            return;
        }

        std::string site = code_.url;
        for (const char* p : {"https://", "http://", "www."})
            if (util::starts_with(site, p)) site.erase(0, std::char_traits<char>::length(p));
        text::draw(font::title, cx, cy, "Enter this code", t.text);
        text::draw_wrapped(font::body, Rect(cx, cy + 44, cw, 60),
                           "On your phone or computer, go to " + site + " and type in:", t.text2, 2);
        draw_code(card.cx(), cy + 120);

        if (error_.empty()) {
            spinner(card.cx() - 120, cy + 256, 10, t.accent, 3);
            text::draw(font::small_bold, card.cx() - 100, cy + 245, "Waiting for you to allow it\xE2\x80\xA6", t.text2);
        } else {
            text::draw(font::small_bold, card.cx(), cy + 245, error_, t.bad, text::CENTER);
        }
        // The code is for the OAuth client of YouTube's TV app (see yt_account.cpp), so that's the
        // name Google shows.
        text::draw_wrapped(font::small, Rect(cx, cy + 284, cw, 50),
                           "Google will name YouTube's TV app, which CoffeeFlix signs in as. Your password stays with Google.",
                           t.text3, 2);
        if (button(id(g, "cancel"), Rect(card.cx() - 90, cy + 352, 180, 46), error_.empty() ? "Cancel" : "Back",
                   error_.empty() ? ic::CLOSE : ic::ARROW_BACK, BTN_NORMAL, g, F_DEFAULT)) {
            app::pop();
            return;
        }
        hint_bar({{"B", "Back"}});
        if (error_.empty()) poll();
    }

private:
    // "ABC-DEF-GHIJ" as letter tiles, the groups set apart.
    void draw_code(float center_x, float top) {
        const Theme& t = theme();
        const std::string& c = code_.user_code;
        float tile = 48, gap = 8, dash = 26, total = 0;
        for (size_t i = 0; i < c.size(); i++) total += c[i] == '-' ? dash : tile + (i + 1 < c.size() && c[i + 1] != '-' ? gap : 0);
        float x = center_x - total * 0.5f;
        for (size_t i = 0; i < c.size(); i++) {
            if (c[i] == '-') {
                gfx::fill_rrect(Rect(x + 7, top + 34, dash - 14, 4), 2, t.text3);
                x += dash;
                continue;
            }
            float bob = std::sin((float)ui::time() * 3 + i * 0.5f) * 2;
            Rect r(x, top + bob, tile, 72);
            gfx::shadow(r, 12, Color(0, 0, 0, 110));
            gfx::fill_rrect_vgrad(r, 12, t.accent, t.accent2);
            text::draw(text::font(text::BOLD, 40), r.cx(), r.y + 12, std::string(1, c[i]), gfx::rgb(0x1A1016), text::CENTER);
            x += tile + (i + 1 < c.size() && c[i + 1] != '-' ? gap : 0);
        }
    }

    void request() {
        error_.clear();
        code_ = yt_account::Code();
        scope_.run<yt_account::Code>([] { return yt_account::request_code(); }, [this](yt_account::Code c) {
            if (!c.ok) {
                error_ = c.error;
                return;
            }
            code_ = c;
            interval_ = std::max(5, c.interval);
            expires_ = ui::time() + c.expires_in;
            next_poll_ = ui::time() + interval_;
            reset_focus();
        });
    }

    void poll() {
        if (polling_ || ui::time() < next_poll_) return;
        if (ui::time() > expires_) {
            toast("The code ran out of time, here's a new one", ic::REFRESH);
            request();
            return;
        }
        polling_ = true;
        std::string device_code = code_.device_code;
        int interval = interval_;
        struct Answer {
            yt_account::Poll result;
            int interval;
            std::string error;
        };
        poll_.run<Answer>([device_code, interval] {
            Answer a{yt_account::WAITING, interval, ""};
            a.result = yt_account::poll(device_code, a.interval, a.error);
            return a;
        }, [this](Answer a) {
            polling_ = false;
            interval_ = a.interval;
            next_poll_ = ui::time() + interval_;
            if (a.result == yt_account::SIGNED_IN) {
                toast("Signed in as " + yt_account::name(), ic::CHECK_CIRCLE, theme().good);
                app::pop();
            } else if (a.result == yt_account::FAILED) {
                error_ = a.error;
            }
        });
    }

    yt_account::Code code_;
    std::string error_;
    int interval_ = 5;
    double expires_ = 0, next_poll_ = 0;
    bool polling_ = false;
    tasks::Scope scope_, poll_;
};

}  // namespace

std::unique_ptr<app::Screen> make_sign_in() { return std::make_unique<SignInScreen>(); }

void account_menu() {
    if (!yt_account::signed_in()) {
        app::push(make_sign_in());
        return;
    }
    show_menu(yt_account::name(), "Signed in to YouTube", {
        {"Sign out", ic::LOGOUT, [] {
             yt_account::sign_out();
             toast("Signed out of YouTube", ic::LOGOUT);
         }},
    });
}

}  // namespace yt
}  // namespace screens
