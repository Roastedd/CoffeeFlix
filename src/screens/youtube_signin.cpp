// Signing in to a YouTube account with a code entered on another device (services/yt_account):
// scan the QR code, or go to the address and type the code.
#include <cmath>

#include "core/i18n.hpp"
#include "core/qr.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/images.hpp"
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
        text::draw(font::display, x0, 60, done_ ? tr("You're signed in") : tr("Sign in to YouTube"), t.text);
        text::draw(font::body, x0 + 2, 122,
                   done_ ? tr("Signed in with Google.") : tr("Bring your subscriptions and recommendations to CoffeeFlix."),
                   t.text2);

        Rect panel(x0, 176, W - x0 - 60, 400);
        gfx::shadow(panel, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(panel, 26, Color(255, 255, 255, 12));
        gfx::stroke_rrect(panel, 26, 1, Color(255, 255, 255, 18));

        if (done_) {
            draw_done(g, panel);
            return;
        }
        if (code_.user_code.empty()) {
            if (error_.empty()) {
                loading_indicator(panel.cx(), panel.cy() - 10, tr("Getting a code\xE2\x80\xA6"));
            } else if (empty_state_action(id(g, "retry"), panel, ic::WIFI_OFF, tr("Couldn't start signing in"),
                                          error_.c_str())) {
                request();
            }
            hint_bar({{"B", tr("Back")}});
            return;
        }

        // Left: the QR code. Right: the same in three steps.
        float qr_side = draw_qr(panel.x + 52, panel.y + 48, 236);
        text::draw(font::small_bold, panel.x + 52 + qr_side * 0.5f, panel.y + 48 + qr_side + 18,
                   tr("Scan with your phone"), t.text2, text::CENTER);

        float div_x = panel.x + 356;
        gfx::fill_rect(Rect(div_x, panel.y + 48, 1, panel.h - 96), Color(255, 255, 255, 22));
        gfx::fill_circle(div_x, panel.cy(), 20, gfx::rgb(0x1C1A20));
        gfx::stroke_circle(div_x, panel.cy(), 20, 1, Color(255, 255, 255, 30));
        text::draw(font::small_bold, div_x, panel.cy() - 11, tr("or"), t.text3, text::CENTER);

        float rx = div_x + 56, ry = panel.y + 44;
        step(rx, ry, 1);
        text::draw(font::body, rx + 50, ry, tr("On a phone or computer, go to"), t.text2);
        text::draw(font::title, rx + 50, ry + 28, site(), t.text);

        ry += 102;
        step(rx, ry, 2);
        text::draw(font::body, rx + 50, ry, tr("Enter this code"), t.text2);
        draw_code(rx + 50, ry + 34);

        ry += 142;
        step(rx, ry, 3);
        text::draw(font::body, rx + 50, ry, tr("Choose your Google account, then Allow"), t.text2);

        // Status along the bottom of the panel.
        float sy = panel.b() - 52;
        if (error_.empty()) {
            spinner(rx + 60, sy + 10, 9, t.accent, 3);
            text::draw(font::small_bold, rx + 80, sy, tr("Waiting for you to allow it\xE2\x80\xA6"), t.text2);
            int left = std::max(0, (int)(expires_ - ui::time()));
            text::draw(font::small, panel.r() - 44, sy, util::fmt(tr("Code works for %d:%02d"), left / 60, left % 60),
                       t.text3, text::RIGHT);
        } else {
            text::icon(ic::ERROR_OUTLINE, 22, rx + 60, sy + 10, t.bad);
            text::draw(font::small_bold, rx + 80, sy, error_, t.bad);
        }

        // Below the panel: what Google will show, and the buttons.
        float by = panel.b() + 22;
        Id bg = id(g, "buttons");
        float cw = measure_button(tr("Cancel"), ic::CLOSE), nw = measure_button(tr("New code"), ic::REFRESH);
        if (button(id(bg, "cancel"), Rect(panel.r() - cw, by, cw, 46), tr("Cancel"), ic::CLOSE, BTN_NORMAL, bg,
                   error_.empty() ? F_DEFAULT : 0)) {
            app::pop();
            return;
        }
        if (button(id(bg, "new"), Rect(panel.r() - cw - 14 - nw, by, nw, 46), tr("New code"), ic::REFRESH,
                   error_.empty() ? BTN_GHOST : BTN_PRIMARY, bg, error_.empty() ? 0 : F_DEFAULT)) {
            request();
            return;
        }
        // The code belongs to the OAuth client of YouTube's VR app (see yt_account.cpp), so that's
        // the app Google's page names.
        text::draw_wrapped(font::small, Rect(x0 + 4, by + 2, panel.w - cw - nw - 60, 46),
                           tr("Google's page will name YouTube's VR app: that's how CoffeeFlix signs in. Your password "
                              "stays with Google, and you can sign out in Settings."),
                           t.text3, 2);
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
        if (error_.empty()) poll();
    }

private:
    std::string site() const {
        std::string s = code_.url;
        for (const char* p : {"https://", "http://", "www."})
            if (util::starts_with(s, p)) s.erase(0, std::char_traits<char>::length(p));
        return s;
    }

    void step(float x, float y, int n) {
        const Theme& t = theme();
        gfx::fill_circle(x + 17, y + 13, 17, Color(t.accent.r, t.accent.g, t.accent.b, 40));
        text::draw(font::small_bold, x + 17, y + 2, std::to_string(n), t.accent, text::CENTER);
    }

    // The code in fixed-width cells so it reads like a code, the groups set apart.
    void draw_code(float x, float y) {
        const Theme& t = theme();
        const std::string& c = code_.user_code;
        text::Font f = text::font(text::BOLD, 40);
        float cell = 36, dash = 26, w = 40;
        for (char ch : c) w += ch == '-' ? dash : cell;
        Rect pill(x, y, w, 74);
        gfx::fill_rrect(pill, 16, Color(0, 0, 0, 70));
        float pulse = 0.5f + 0.5f * std::sin((float)ui::time() * 2.4f);
        gfx::stroke_rrect(pill, 16, 2, gfx::lerp(t.accent2, t.accent, pulse));
        float cx = x + 20;
        for (char ch : c) {
            if (ch == '-') {
                gfx::fill_rrect(Rect(cx + 7, y + 36, dash - 14, 4), 2, t.text3);
                cx += dash;
                continue;
            }
            text::draw(f, cx + cell * 0.5f, y + 12, std::string(1, ch), t.text, text::CENTER);
            cx += cell;
        }
    }

    // Dark modules on a white tile at whole-pixel sizes so phones read it cleanly; returns the side.
    float draw_qr(float x, float y, float max_side) {
        std::string text = code_.url + "?user_code=" + code_.user_code;  // Google fills the code in
        if (text != qr_text_) {
            qr_text_ = text;
            qr_ = qr::encode(text);
        }
        if (qr_.size == 0) return 0;
        int quiet = 3;
        int m = std::max(2, (int)(max_side / (qr_.size + quiet * 2)));
        float side = (float)(m * (qr_.size + quiet * 2));
        x = std::floor(x + (max_side - side) * 0.5f);
        y = std::floor(y);
        gfx::fill_rrect(Rect(x, y, side, side), 14, gfx::WHITE);
        Color ink = gfx::rgb(0x111111);
        float ox = x + m * quiet, oy = y + m * quiet;
        for (int r = 0; r < qr_.size; r++) {
            for (int c = 0; c < qr_.size;) {
                if (!qr_.at(c, r)) {
                    c++;
                    continue;
                }
                int run = c;
                while (run < qr_.size && qr_.at(run, r)) run++;
                gfx::fill_rect(Rect(ox + c * m, oy + r * m, (float)((run - c) * m), (float)m), ink);
                c = run;
            }
        }
        return max_side;
    }

    void draw_done(Id g, const Rect& panel) {
        const Theme& t = theme();
        float cx = panel.cx(), top = panel.y + 56;
        float r = 56;
        const images::Image* img = images::get(yt_account::photo(), 176, 176);
        if (img && img->ready && img->tex) {
            gfx::image(img->tex, Rect(cx - r, top, r * 2, r * 2), gfx::WHITE, r);
        } else {
            gfx::fill_circle(cx, top + r, r, Color(255, 255, 255, 20));
            text::icon(ic::PERSON, 60, cx, top + r, t.text2);
        }
        gfx::fill_circle(cx + r * 0.72f, top + r * 1.72f, 17, gfx::rgb(0x1C1A20));
        text::icon(ic::CHECK_CIRCLE, 32, cx + r * 0.72f, top + r * 1.72f, t.good);

        text::draw(font::headline, cx, top + r * 2 + 24, yt_account::name(), t.text, text::CENTER);
        text::draw_wrapped(font::body, Rect(cx - 330, top + r * 2 + 76, 660, 60),
                           tr("Your subscriptions and YouTube's recommendations now show in CoffeeFlix, and subscribing "
                              "here updates your account."),
                           t.text2, 2, text::CENTER);
        Id bg = id(g, "done");
        float bw = std::max(200.0f, measure_button(tr("Continue"), ic::ARROW_FORWARD));
        if (button(id(bg, "continue"), Rect(cx - bw * 0.5f, panel.b() - 82, bw, 50), tr("Continue"), ic::ARROW_FORWARD,
                   BTN_PRIMARY, bg, F_DEFAULT)) {
            app::pop();
            return;
        }
        hint_bar({{"A", tr("Continue")}, {"B", tr("Back")}});
    }

    void request() {
        error_.clear();
        code_ = yt_account::Code();
        poll_.reset();
        polling_ = false;
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
            toast(tr("The code ran out of time, here's a new one"), ic::REFRESH);
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
                done_ = true;
                reset_focus();
            } else if (a.result == yt_account::FAILED) {
                error_ = a.error;
                reset_focus();
            }
        });
    }

    yt_account::Code code_;
    std::string error_, qr_text_;
    qr::Code qr_;
    int interval_ = 5;
    double expires_ = 0, next_poll_ = 0;
    bool polling_ = false, done_ = false;
    tasks::Scope scope_, poll_;
};

}  // namespace

std::unique_ptr<app::Screen> make_sign_in() { return std::make_unique<SignInScreen>(); }

void account_menu() {
    if (!yt_account::signed_in()) {
        app::push(make_sign_in());
        return;
    }
    show_menu(yt_account::name(), tr("Signed in to YouTube"), {
        {tr("Sign out"), ic::LOGOUT, [] {
             yt_account::sign_out();
             toast(tr("Signed out of YouTube"), ic::LOGOUT);
         }},
    });
}

}  // namespace yt
}  // namespace screens
