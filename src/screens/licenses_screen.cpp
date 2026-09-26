// Settings → Licenses: CoffeeFlix's own license and the notices for everything it includes,
// read from content/licenses (bundled with every copy of the app).
#include <algorithm>
#include <cmath>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

// title and subtitle: English (descriptions marked N_, names of licenses and libraries not), tr() them
// where they're shown.
struct Document {
    const char* file;
    const char* title;
    const char* subtitle;
};

const Document DOCUMENTS[] = {
    {"NOTICES.txt", N_("Notices"), N_("Everything CoffeeFlix includes, with credits and license texts")},
    {"PolyForm-Noncommercial-1.0.0.txt", N_("CoffeeFlix license"), "PolyForm Noncommercial 1.0.0"},
    {"LGPL-2.1.txt", "GNU LGPL 2.1", "libsmb2"},
    {"LGPL-3.0.txt", "GNU LGPL 3.0", "FFmpeg"},
    {"GPL-3.0.txt", "GNU GPL 3.0", N_("The terms LGPL 3.0 builds on")},
    {"Apache-2.0.txt", "Apache License 2.0", "Mbed TLS, Material Icons"},
    {"MPL-2.0.txt", "Mozilla Public License 2.0", N_("Certificate authority list")},
    {"FreeType-FTL.txt", "FreeType License", "FreeType"},
    {"Inter-OFL.txt", "SIL Open Font License 1.1", N_("Inter font")},
    {"MaterialIcons.txt", "Material Icons", N_("Icon font details")},
};

// Scrolling plain-text viewer.
class TextScreen : public app::Screen {
public:
    TextScreen(std::string title, const std::string& path) : title_(std::move(title)) {
        std::string text;
        if (!util::read_file(path, text)) text = tr("This file is missing from this copy of CoffeeFlix.");
        for (const std::string& line : util::split(text, '\n')) {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (util::trim(l).empty()) {
                lines_.emplace_back();
                continue;
            }
            for (std::string& w : text::wrap(font::small, l, WIDTH)) lines_.push_back(std::move(w));
        }
    }
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        suspend_nav();
        float x0 = content_x();
        text::draw_fit(font::headline, x0, 48, W - x0 - 60, title_, t.text);

        const float top = 112, bottom = H - 70, lh = text::line_height(font::small) * 1.25f;
        float content = lines_.size() * lh, view = bottom - top;
        float max_scroll = std::max(0.0f, content - view);
        Input& in = input();
        float step = 0;
        if (in.rep(BTN_DOWN)) step += lh * 3;
        if (in.rep(BTN_UP)) step -= lh * 3;
        if (in.rep(BTN_R) || in.rep(BTN_ZR)) step += view * 0.9f;
        if (in.rep(BTN_L) || in.rep(BTN_ZL)) step -= view * 0.9f;
        if (std::fabs(in.ly) > 0.2f) step -= in.ly * 1400 * ui::dt();
        if (in.touching && in.dragging) step -= in.tdy;
        target_ = std::clamp(target_ + step, 0.0f, max_scroll);
        float y0 = top - tween(id("licscroll"), target_, 16);

        // Holds focus so it doesn't land on the sidebar (the text itself isn't selectable).
        focusable(id("lictext"), Rect(x0, top, W - x0 - 40, view), 0, F_DEFAULT | F_SILENT);

        gfx::push_clip(Rect(x0, top, W - x0, view));
        int first = std::max(0, (int)((top - y0) / lh) - 1);
        for (int i = first; i < (int)lines_.size(); i++) {
            float y = y0 + i * lh;
            if (y > bottom) break;
            text::draw(font::small, x0, y, lines_[i], t.text2);
        }
        gfx::pop_clip();
        if (max_scroll > 0) {
            float bar_h = std::max(40.0f, view * view / content);
            float bar_y = top + (view - bar_h) * (target_ / max_scroll);
            gfx::fill_rrect(Rect(W - 30, bar_y, 4, bar_h), 2, t.text3);
        }
        hint_bar({{"L/R", tr("Page")}, {"B", tr("Back")}});
    }

private:
    static constexpr float WIDTH = 1040;
    std::string title_;
    std::vector<std::string> lines_;
    float target_ = 0;
};

class LicensesScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SETTINGS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), w = W - x0 - 60;
        Id g = id("licenses");
        page_.begin(id(g, "page"));
        float y = page_.y(48);
        text::draw(font::headline, x0, y, tr("Licenses"), t.text);
        text::draw_wrapped(font::body, Rect(x0, y + 50, w, 60),
                           tr("CoffeeFlix is source-available for noncommercial use. It includes open-source libraries "
                              "and fonts, each under its own license."),
                           t.text2, 2);
        y += 124;
        for (size_t i = 0; i < sizeof(DOCUMENTS) / sizeof(DOCUMENTS[0]); i++) {
            const Document& d = DOCUMENTS[i];
            Id iid = id(g, (int64_t)i);
            if (value_row(iid, Rect(x0, y, w, 64), tr(d.title), tr(d.subtitle), ic::DESCRIPTION, g, i == 0 ? F_DEFAULT : 0))
                app::push(std::make_unique<TextScreen>(tr(d.title), platform::content_dir() + "/licenses/" + d.file));
            if (focused() == iid) page_.focus_range(y + page_.scroll() - 60, y + page_.scroll() + 84);
            y += 72;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_licenses() { return std::make_unique<LicensesScreen>(); }

}  // namespace screens
