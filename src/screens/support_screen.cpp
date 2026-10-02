// Settings > Support CoffeeFlix: where to say thanks, as a QR code to scan with a phone (a Wii U can't
// open a link), and the one-time question that points here.
#include "core/i18n.hpp"
#include "core/qr.hpp"
#include "core/store.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"

namespace screens {

namespace {

using namespace ui;

constexpr const char* KOFI_URL = "https://ko-fi.com/ubecatstudio";
constexpr const char* KOFI_SHORT = "ko-fi.com/ubecatstudio";

class SupportScreen : public app::Screen {
public:
    SupportScreen() : code_(qr::encode(KOFI_URL)) {}
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        const float x = content_x();
        const Id g = id("support");
        section_title(x, 52, tr("Support CoffeeFlix"), tr("Free, with no ads and no accounts, and made in spare time."));

        Rect left(x, 154, 640, 400), right(x + 665, 154, W - x - 725, 400);
        gfx::fill_rrect(left, 22, t.surface);
        gfx::fill_rrect(right, 22, t.surface);

        // A heart on the logo's colours, with a little steam, as warm as the rest of the app.
        const float hx = left.x + 70, hy = left.y + 76;
        gfx::fill_rrect_hgrad(Rect(hx - 40, hy - 40, 80, 80), 40, t.accent, t.accent2);
        text::icon(ic::FAVORITE, 42, hx, hy, gfx::rgb(0x1A1016));
        float pulse = 0.5f + 0.5f * std::sin((float)ui::time() * 2.2f);
        gfx::stroke_circle(hx, hy, 46 + pulse * 8, 2, t.accent.alpha(0.35f * (1 - pulse)));

        text::draw(font::title, left.x + 132, left.y + 44, tr("Buy me a coffee"), t.text);
        text::draw_wrapped(font::body, Rect(left.x + 32, left.y + 142, left.w - 64, 150),
                           tr("If you enjoy CoffeeFlix, a coffee on Ko-fi helps keep the updates coming. Thank you!"),
                           t.text2, 4);
        text::draw_wrapped(font::small, Rect(left.x + 32, left.y + 232, left.w - 64, 60),
                           tr("Ideas and bug reports help just as much: open an issue on GitHub."), t.text3, 2);

        const float q = std::min(250.0f, right.w - 40);
        qr_code(code_, right.cx() - q / 2, right.y + 28, q);
        text::draw(font::small, right.cx(), right.y + q + 44, tr("Scan with your phone"), t.text2, text::CENTER);
        text::draw(font::body_bold, right.cx(), right.y + q + 72, KOFI_SHORT, t.accent, text::CENTER);

        if (button(id(g, "back"), Rect(x, 580, 220, 50), tr("Back"), ic::ARROW_BACK, BTN_PRIMARY, g, F_DEFAULT)) app::pop();
        hint_bar({{"B", tr("Back")}});
    }

private:
    qr::Code code_;
};

// How many starts before the question, and the stored keys. It is asked once, ever.
constexpr int64_t ASK_AFTER_STARTS = 10;

}  // namespace

std::unique_ptr<app::Screen> make_support() { return std::make_unique<SupportScreen>(); }

void count_start() { store::set_int("starts", store::get_int("starts", 0) + 1); }

// Asked on Home, once: after the tenth start, and never again whatever the answer (or no answer).
void maybe_ask_for_support() {
    if (store::get_bool("support_asked", false) || store::get_int("starts", 0) < ASK_AFTER_STARTS) return;
    if (prompt_active() || menu_active()) return;
    store::set_bool("support_asked", true);
    show_menu(tr("Enjoying CoffeeFlix?"), tr("A coffee keeps the updates coming."),
              {{tr("Support on Ko-fi"), ic::FAVORITE, [] { app::push(make_support()); }},
               {tr("Maybe later"), ic::CLOSE, [] {}}});
}

}  // namespace screens
