// The grown-ups' lock: asking for the code, and setting it (a 4-digit PIN with an on-screen pad, or a button code
// pressed on the controller or on the touch screen's buttons). See core/lock.
#include <algorithm>
#include <cstring>

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/input.hpp"
#include "core/lock.hpp"
#include "core/util.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

constexpr float SAFE_X = 64;  // 5% of the width, where a TV may cut the picture off

struct Press {
    char symbol;
    Button button;
};
constexpr Press PRESSES[] = {{'U', BTN_UP}, {'D', BTN_DOWN}, {'L', BTN_LEFT}, {'R', BTN_RIGHT},
                             {'A', BTN_A},  {'B', BTN_B},    {'X', BTN_X},    {'Y', BTN_Y}};

class LockScreen : public app::Screen {
public:
    LockScreen(bool setup, std::function<void()> granted, std::function<void(bool)> done)
        : setup_(setup), granted_(std::move(granted)), done_(std::move(done)) {
        if (setup_) stage_ = CHOOSE;
        else kind_ = lock::kind();
    }

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }  // plain: no picture of the page behind it shows through
    app::Section section() const override { return app::SEC_NONE; }

    bool on_back() override {
        finish(false);
        return false;  // the screen closes
    }

    void frame() override {
        const Theme& t = theme();
        Input& in = input();
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        // Where the code is typed on the touch screen or pad, B is a press of the code, never "back".
        const bool pressing = stage_ != CHOOSE && kind_ == lock::Kind::BUTTONS;
        text::draw_fit(font::display, W * 0.5f, 84, W - 2 * SAFE_X, title(), t.text, text::CENTER);
        text::draw_fit(font::body, W * 0.5f, 150, W - 2 * SAFE_X, subtitle(), t.text2, text::CENTER);

        if (stage_ == CHOOSE) {
            choose();
            return;
        }

        // The dots: how many of the code have been given.
        const size_t slots = slots_count();
        const float shake = ui::time() - shake_t_ < 0.35 ? std::sin((float)(ui::time() - shake_t_) * 60.0f) * 12.0f * (1.0f - (float)(ui::time() - shake_t_) / 0.35f) : 0;
        const float dx = W * 0.5f - (slots - 1) * 28.0f + shake;
        for (size_t i = 0; i < slots; i++) {
            const float cx = dx + i * 56.0f, cy = 236;
            if (i < code_.size()) gfx::fill_circle(cx, cy, 14, t.text);
            else gfx::fill_circle(cx, cy, 14, Color(255, 255, 255, 40));
        }

        const int wait = setup_ ? 0 : lock::wait_seconds();
        if (wait > 0) {
            message_ = util::fmt(tr("Too many wrong tries. Try again in %d:%02d"), wait / 60, wait % 60);
            bad_ = true;
            wait_message_ = true;
        } else if (wait_message_) {  // the wait is over: its message goes
            message_.clear();
            wait_message_ = false;
        }
        if (!message_.empty())
            text::draw_fit(font::body_bold, W * 0.5f, 282, W - 2 * SAFE_X, message_, bad_ ? t.bad : t.text2, text::CENTER);

        if (wait > 0) {
            if (pressing) {
                for (const Press& p : PRESSES) in.eat(p.button);
            }
            cancel_hint(pressing);
            if (!pressing) hint_bar({{"B", tr("Cancel")}});
            if (pressing && (in.pressed_(BTN_MINUS))) finish(false);
            return;
        }
        if (kind_ == lock::Kind::PIN) pin_pad();
        else button_pad(in);
        cancel_hint(pressing);
    }

private:
    enum Stage { CHOOSE, ENTRY, CONFIRM };

    const char* title() const {
        if (!setup_) return tr("Grown-ups only");
        return stage_ == CHOOSE ? tr("Set a code") : stage_ == ENTRY ? tr("Make your code") : tr("Once more");
    }
    std::string subtitle() const {
        if (!setup_) return kind_ == lock::Kind::BUTTONS ? tr("Press your code on the controller") : tr("Enter your PIN");
        if (stage_ == CHOOSE) return tr("It keeps children out of grown-up places.");
        if (kind_ == lock::Kind::PIN) return stage_ == ENTRY ? tr("Choose four digits") : tr("Enter the same four digits");
        return stage_ == ENTRY ? util::fmt(tr("Press %d to %d buttons, then +"), (int)lock::MIN_BUTTONS, (int)lock::MAX_BUTTONS)
                               : tr("Press the same buttons, then +");
    }
    size_t slots_count() const {
        if (kind_ == lock::Kind::PIN) return lock::PIN_LENGTH;
        return setup_ ? lock::MAX_BUTTONS : std::max<size_t>(lock::length(), lock::MIN_BUTTONS);
    }

    void finish(bool ok) {
        if (finished_) return;
        finished_ = true;
        auto done = done_;
        if (setup_ && done) done(ok);
    }

    // Setup, first step: which kind.
    void choose() {
        const Theme& t = theme();
        Id g = id("lock_choose");
        text::draw_fit(font::small, W * 0.5f, 214, W - 2 * SAFE_X,
                       tr("A button code is quicker with a controller; a PIN is easier to tell someone."), t.text3, text::CENTER);
        const float bw = 440;
        if (button(id(g, "pin"), Rect(W * 0.5f - bw - 16, 290, bw, 72), tr("Number PIN (4 digits)"), ic::LOCK, BTN_PRIMARY, g, F_DEFAULT)) {
            kind_ = lock::Kind::PIN;
            stage_ = ENTRY;
            reset_focus();
        }
        if (button(id(g, "buttons"), Rect(W * 0.5f + 16, 290, bw, 72), tr("Button code (4 to 6)"), ic::GAMEPAD, BTN_NORMAL, g)) {
            kind_ = lock::Kind::BUTTONS;
            stage_ = ENTRY;
            reset_focus();
        }
        // Said now, while whoever sets the code can write it down: how to start over if it's forgotten.
        text::draw_wrapped(font::small, Rect(W * 0.5f - 380, 500, 760, 60),
                           tr("If you forget it: put an empty file named reset-lock.txt in the CoffeeFlix folder on the SD card."),
                           t.text2, 2, text::CENTER);
        const char* cancel = tr("Cancel");
        const float cw = measure_button(cancel, ic::CLOSE);
        if (button(id(g, "cancel"), Rect((W - cw) * 0.5f, 410, cw, 52), cancel, ic::CLOSE, BTN_GHOST, g)) {
            finish(false);
            app::pop();
            return;
        }
        hint_bar({{"A", tr("Select")}, {"B", tr("Cancel")}});
    }

    void entered(const std::string& code) {
        if (setup_) {
            if (stage_ == ENTRY) {
                first_ = code;
                code_.clear();
                stage_ = CONFIRM;
                message_.clear();
                return;
            }
            if (code != first_) {
                audio::play(audio::SFX_ERROR);
                shake_t_ = ui::time();
                message_ = tr("Those didn't match. Try again.");
                bad_ = true;
                code_.clear();
                first_.clear();
                stage_ = ENTRY;
                return;
            }
            lock::set(kind_, code);
            auto done = done_;
            finished_ = true;
            app::pop();
            toast(tr("Code set"), ic::CHECK_CIRCLE);
            if (done) done(true);
            return;
        }
        switch (lock::check(code)) {
            case lock::Result::OK: {
                auto granted = granted_;
                finished_ = true;
                app::pop();
                if (granted) granted();
                return;
            }
            case lock::Result::WRONG:
                audio::play(audio::SFX_ERROR);
                shake_t_ = ui::time();
                message_ = tr("That isn't the code");
                bad_ = true;
                code_.clear();
                return;
            case lock::Result::LOCKED_OUT:
                code_.clear();
                return;
        }
    }

    // Typing: the right number of presses ends a code, except a new button code, which ends with +.
    void add(char c) {
        const size_t want = setup_ ? (kind_ == lock::Kind::PIN ? lock::PIN_LENGTH : lock::MAX_BUTTONS) : lock::length();
        if (code_.size() >= want) return;
        audio::play(audio::SFX_MOVE, 0.6f);
        code_ += c;
        message_.clear();
        const bool auto_end = kind_ == lock::Kind::PIN || !setup_;
        if (auto_end && code_.size() == want) {
            std::string code = code_;
            entered(code);
        }
    }
    void end_new_button_code() {  // + ends a new button code of 4 to 6 presses
        if (!lock::valid(lock::Kind::BUTTONS, code_)) {
            audio::play(audio::SFX_ERROR);
            message_ = util::fmt(tr("Press %d to %d buttons"), (int)lock::MIN_BUTTONS, (int)lock::MAX_BUTTONS);
            bad_ = true;
            shake_t_ = ui::time();
            return;
        }
        std::string code = code_;
        entered(code);
    }

    void pin_pad() {
        const Theme& t = theme();
        Id g = id("lock_pad");
        constexpr float KW = 96, KH = 64, KG = 14, TOP = 332;
        const float left = (W - (3 * KW + 2 * KG)) * 0.5f;
        const char* keys = "123456789<0x";
        for (int i = 0; i < 12; i++) {
            const Rect r(left + (i % 3) * (KW + KG), TOP + (i / 3) * (KH + KG), KW, KH);
            const char k = keys[i];
            Item it = focusable(id(g, std::string(1, k).c_str()), r, g, i == 0 ? F_DEFAULT : 0);
            gfx::fill_rrect(r, 16, gfx::lerp(Color(255, 255, 255, 26), t.surface_focus, it.f));
            if (it.focused) focus_ring(r, 16, it.f);
            const Color fg = gfx::lerp(t.text2, gfx::rgb(0x15121A), it.f);  // dark on the light focus
            if (k == '<') text::icon(ic::BACKSPACE, 30, r.cx(), r.cy(), fg);
            else if (k == 'x') text::icon(ic::CLOSE, 30, r.cx(), r.cy(), fg);
            else text::draw(font::headline, r.cx(), r.cy() - text::line_height(font::headline) * 0.5f, std::string(1, k), fg, text::CENTER);
            if (!it.clicked) continue;
            if (k == '<') {
                if (!code_.empty()) code_.pop_back();
            } else if (k == 'x') {
                finish(false);
                app::pop();
                return;
            } else {
                add(k);
            }
        }
        hint_bar({{"A", tr("Select")}, {"B", tr("Cancel")}});
    }

    // The buttons of a code on the controller (D-pad, A B X Y) and on the screen for touch.
    void button_pad(Input& in) {
        const Theme& t = theme();
        char got = 0;
        for (const Press& p : PRESSES)
            if (in.pressed_(p.button)) {
                got = p.symbol;
                in.eat(p.button);
            }
        // On the screen: a D-pad and A B X Y, as touch targets (not focusable, so the code's own presses don't move focus).
        struct Spot { char symbol; float x, y; };
        const float lx = W * 0.5f - 250, rx = W * 0.5f + 250, cy = 470, d = 76;
        const Spot spots[] = {{'U', lx, cy - d}, {'D', lx, cy + d}, {'L', lx - d, cy}, {'R', lx + d, cy},
                              {'X', rx, cy - d}, {'B', rx, cy + d}, {'Y', rx - d, cy}, {'A', rx + d, cy}};
        for (const Spot& s : spots) {
            const bool hit = in.tap && std::hypot(in.tx - s.x, in.ty - s.y) < 40;
            if (hit) {
                got = s.symbol;
                in.tap = false;
            }
            gfx::fill_circle(s.x, s.y, 36, Color(255, 255, 255, (uint8_t)(hit ? 70 : 28)));
            if (strchr("UDLR", s.symbol)) {
                const int icon = s.symbol == 'U' ? ic::EXPAND_LESS : s.symbol == 'D' ? ic::EXPAND_MORE : s.symbol == 'L' ? ic::ARROW_BACK : ic::ARROW_FORWARD;
                text::icon(icon, 34, s.x, s.y, t.text2);
            } else {
                const char name[2] = {s.symbol, 0};
                button_glyph(s.x, s.y, name, 44);
            }
        }
        if (got) {
            if (setup_ && code_.size() >= lock::MAX_BUTTONS) {
                audio::play(audio::SFX_BUMP);
            } else {
                add(got);
            }
        }
        if (setup_ && in.pressed_(BTN_PLUS)) {
            in.eat(BTN_PLUS);
            end_new_button_code();
        }
        if (setup_) {  // touch: the same as +
            const char* done = tr("Done");
            const float dw = measure_button(done, ic::CHECK);
            const Rect r(W * 0.5f - dw * 0.5f, 590, dw, 52);
            const bool hit = in.tap && r.contains(in.tx, in.ty);
            if (hit) {
                in.tap = false;
                end_new_button_code();
            }
            gfx::fill_rrect(r, 26, Color(255, 255, 255, hit ? 70 : 34));
            text::icon(ic::CHECK, 24, r.x + 30, r.cy(), t.text);
            text::draw(font::body_bold, r.x + 52, r.cy() - text::line_height(font::body_bold) * 0.5f, done, t.text);
        }
        if (in.pressed_(BTN_MINUS)) {
            in.eat(BTN_MINUS);
            finish(false);
            app::pop();
        }
    }

    void cancel_hint(bool pressing) {
        const Theme& t = theme();
        Input& in = input();
        if (!pressing) return;
        const char* label = tr("Cancel");
        const float lw = text::measure(font::small_bold, label) + 56;
        const Rect r(SAFE_X, H - 70, lw, 40);
        const bool hit = in.tap && r.contains(in.tx, in.ty);
        text::draw(font::small_bold, r.x + 12, r.cy() - text::line_height(font::small_bold) * 0.5f, std::string("\xE2\x88\x92  ") + label, t.text2);
        if (hit) {
            in.tap = false;
            finish(false);
            app::pop();
        }
    }

    bool setup_;
    std::function<void()> granted_;
    std::function<void(bool)> done_;
    Stage stage_ = ENTRY;
    lock::Kind kind_ = lock::Kind::PIN;
    std::string code_, first_, message_;
    bool bad_ = false, finished_ = false, wait_message_ = false;
    double shake_t_ = -10;
};

}  // namespace

std::unique_ptr<app::Screen> make_lock_prompt(std::function<void()> granted) {
    return std::make_unique<LockScreen>(false, std::move(granted), nullptr);
}
std::unique_ptr<app::Screen> make_lock_setup(std::function<void(bool)> done) {
    return std::make_unique<LockScreen>(true, nullptr, std::move(done));
}

}  // namespace screens
