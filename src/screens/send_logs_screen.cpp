// Settings > Send logs: says what gets sent, uploads this run's log and the one before without
// private data once the user chooses Upload (app/bug_report), then shows a QR code that opens a
// new GitHub issue with the link filled in.
#include <sys/stat.h>

#include "app/bug_report.hpp"
#include "core/i18n.hpp"
#include "core/qr.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/send_logs.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

class SendLogsScreen : public app::Screen {
public:
    SendLogsScreen() {
        std::string dir = platform::data_dir();
        for (int i = 0; i < 2; i++) {
            struct stat st;
            size_[i] = stat((dir + "/" + bug_report::LOG_FILES[i]).c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
        }
    }
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("sendlogs");
        if (state_ != shown_) {
            shown_ = state_;
            reset_focus();
        }
        text::draw_fit(font::display, x0, 60, W - x0 - 60, state_ == SENT ? tr("Your logs are uploaded") : tr("Send logs"),
                       t.text);
        text::draw_fit(font::body, x0 + 2, 122, W - x0 - 62,
                       state_ == SENT ? tr("Now say what happened in a new issue on GitHub.")
                                      : tr("What CoffeeFlix logged, to report a problem on GitHub."),
                       t.text2);

        Rect panel(x0, 176, W - x0 - 60, 400);
        gfx::shadow(panel, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(panel, 26, Color(255, 255, 255, 12));
        gfx::stroke_rrect(panel, 26, 1, Color(255, 255, 255, 18));
        Id bg = id(g, "buttons");
        float by = panel.b() + 22;

        switch (state_) {
            case ASK:
                draw_ask(panel, bg, by);
                break;
            case SENDING: {
                loading_indicator(panel.cx(), panel.cy() - 10, tr("Uploading the logs\xE2\x80\xA6"));
                // Leaving stops the upload (the scope goes, and with it the request).
                float cw = measure_button(tr("Cancel"), ic::CLOSE);
                if (button(id(bg, "cancel"), Rect(panel.r() - cw, by, cw, 46), tr("Cancel"), ic::CLOSE, BTN_NORMAL, bg,
                           F_DEFAULT)) {
                    app::pop();
                    return;
                }
                hint_bar({{"A", tr("Select")}, {"B", tr("Cancel")}});
                break;
            }
            case FAILED:
                if (empty_state_action(id(g, "retry"), panel, ic::WIFI_OFF, tr("Couldn't upload the logs"), error_.c_str()))
                    upload();
                hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
                break;
            case SENT:
                draw_sent(panel, bg, by);
                break;
        }
    }

private:
    enum State { ASK, SENDING, SENT, FAILED };

    void draw_ask(const Rect& panel, Id bg, float by) {
        const Theme& t = theme();
        float px = panel.x + 48, pw = panel.w - 96;
        text::draw(font::small_bold, px, panel.y + 40, tr("WHAT GETS SENT"), t.accent);
        text::draw_wrapped(font::body, Rect(px, panel.y + 74, pw, 90),
                           util::fmt(tr("The logs of this run and the one before go to dpaste.com, without sign-in tokens, "
                                        "device IDs, signed links or email addresses, but with the titles of what you "
                                        "played. Anyone with the link can read them until they're deleted after %d days."),
                                     bug_report::EXPIRY_DAYS),
                           t.text, 3);

        // The two files, and how big they are.
        const char* what[2] = {tr("This run"), tr("The run before")};
        float ry = panel.y + 176;
        for (int i = 0; i < 2; i++) {
            Rect row(px, ry, pw, 52);
            gfx::fill_rrect(row, 12, Color(0, 0, 0, 50));
            text::icon(ic::DESCRIPTION, 24, row.x + 30, row.cy(), size_[i] >= 0 ? t.text2 : t.text3);
            float nx = row.x + 56;
            nx += text::draw(font::body_bold, nx, row.y + 14, bug_report::LOG_FILES[i], size_[i] >= 0 ? t.text : t.text3);
            text::draw(font::small, nx + 14, row.y + 17, what[i], t.text3);
            std::string size = size_[i] >= 0 ? util::format_bytes((uint64_t)size_[i]) : std::string(tr("None"));
            text::draw(font::small_bold, row.r() - 22, row.y + 17, size, t.text2, text::RIGHT);
            ry += 62;
        }

        // Along the bottom: what comes after.
        float sy = panel.b() - 70;
        gfx::fill_rect(Rect(px, sy - 18, pw, 1), Color(255, 255, 255, 18));
        text::icon(ic::FORUM, 24, px + 12, sy + 16, t.text3);
        text::draw_wrapped(font::small, Rect(px + 34, sy + 6, pw - 34, 56),
                           tr("Then scan a QR code with your phone: it opens a new issue on GitHub with the link filled "
                              "in, for you to say what happened."),
                           t.text3, 2);

        // Below the panel: a note on the left, the buttons on the right.
        float right = panel.r();
        float uw = measure_button(tr("Upload"), ic::CLOUD_UPLOAD);
        right -= uw;
        if (button(id(bg, "upload"), Rect(right, by, uw, 46), tr("Upload"), ic::CLOUD_UPLOAD, BTN_PRIMARY, bg, F_DEFAULT)) {
            upload();
            return;
        }
        right -= 14;
        float nw = measure_button(tr("Not now"));
        right -= nw;
        if (button(id(bg, "later"), Rect(right, by, nw, 46), tr("Not now"), 0, BTN_GHOST, bg)) {
            app::pop();
            return;
        }
        text::draw_wrapped(font::small, Rect(panel.x + 4, by + 2, right - panel.x - 30, 46),
                           tr("Nothing is sent until you choose Upload."), t.text3, 2);
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

    void draw_sent(const Rect& panel, Id bg, float by) {
        const Theme& t = theme();
        // Left: the QR code of the new issue. Right: the same in three steps.
        float qx = panel.x + 52, qy = panel.y + 50, qside = 256;
        float side = qr_code(qr_, qx, qy, qside);
        text::draw(font::small_bold, qx + qside * 0.5f, qy + side + 16, tr("Scan with your phone"), t.text2, text::CENTER);

        float div_x = qx + qside + 60;
        gfx::fill_rect(Rect(div_x, panel.y + 48, 1, panel.h - 96), Color(255, 255, 255, 22));
        gfx::fill_circle(div_x, panel.cy(), 20, gfx::rgb(0x1C1A20));
        gfx::stroke_circle(div_x, panel.cy(), 20, 1, Color(255, 255, 255, 30));
        text::draw(font::small_bold, div_x, panel.cy() - 11, tr("or"), t.text3, text::CENTER);

        float rx = div_x + 56, ry = panel.y + 44, rw = panel.r() - 44 - (rx + 50);
        step(rx, ry, 1);
        text::draw_fit(font::body, rx + 50, ry, rw, tr("On a phone or computer, go to"), t.text2);
        text::draw_fit(font::title, rx + 50, ry + 28, rw, "github.com/Roastedd/CoffeeFlix/issues", t.text);

        ry += 100;
        step(rx, ry, 2);
        text::draw_fit(font::body, rx + 50, ry, rw, tr("Open a new issue with the link to your logs"), t.text2);
        text::draw_fit(font::title, rx + 50, ry + 28, rw, bare(paste_url_), t.text);

        ry += 100;
        step(rx, ry, 3);
        text::draw_wrapped(font::body, Rect(rx + 50, ry, rw, 56),
                           tr("Say what happened, and what you did just before. You need a GitHub account."), t.text2, 2);

        // Status along the bottom of the panel.
        float sy = panel.b() - 52;
        text::icon(ic::CHECK_CIRCLE, 22, rx + 60, sy + 10, t.good);
        text::draw_fit(font::small_bold, rx + 80, sy, panel.r() - 44 - (rx + 80),
                       util::fmt(tr("Uploaded. dpaste.com deletes the logs after %d days."), bug_report::EXPIRY_DAYS), t.text2);

        // Below the panel: a note, and Done.
        float dw = measure_button(tr("Done"), ic::CHECK);
        if (button(id(bg, "done"), Rect(panel.r() - dw, by, dw, 46), tr("Done"), ic::CHECK, BTN_PRIMARY, bg, F_DEFAULT)) {
            app::pop();
            return;
        }
        text::draw_wrapped(font::small, Rect(panel.x + 4, by + 2, panel.w - dw - 40, 46),
                           tr("The link isn't saved, so open the issue before you leave this screen."), t.text3, 2);
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

    // "https://dpaste.com/ABC" -> "dpaste.com/ABC"
    static std::string bare(std::string s) {
        for (const char* p : {"https://", "http://", "www."})
            if (util::starts_with(s, p)) s.erase(0, std::char_traits<char>::length(p));
        return s;
    }

    void step(float x, float y, int n) {
        const Theme& t = theme();
        gfx::fill_circle(x + 17, y + 13, 17, Color(t.accent.r, t.accent.g, t.accent.b, 40));
        text::draw(font::small_bold, x + 17, y + 2, std::to_string(n), t.accent, text::CENTER);
    }

    // Only when the user chooses Upload (or Try again).
    void upload() {
        state_ = SENDING;
        error_.clear();
        bug_report::Info info = bug_report::gather();
        std::shared_ptr<std::atomic<bool>> alive = scope_.token();
        struct Sent {
            bug_report::Result result;
            std::string issue;
            qr::Code code;
        };
        scope_.run<Sent>([info, alive] {
            Sent s;
            s.result = bug_report::send(info, [alive] { return alive->load(); });
            if (!s.result.url.empty()) {
                s.issue = bug_report::issue_url(info, s.result.url);
                s.code = qr::encode(s.issue);
            }
            return s;
        }, [this](Sent s) {
            if (s.result.url.empty()) {
                error_ = s.result.error;
                state_ = FAILED;
                return;
            }
            paste_url_ = s.result.url;
            qr_ = std::move(s.code);
            state_ = SENT;
        });
    }

    State state_ = ASK, shown_ = ASK;
    int64_t size_[2] = {-1, -1};  // of the log files; -1 when there's none
    std::string error_, paste_url_;
    qr::Code qr_;
    tasks::Scope scope_;
};

}  // namespace

std::unique_ptr<app::Screen> make_send_logs() { return std::make_unique<SendLogsScreen>(); }

}  // namespace screens
