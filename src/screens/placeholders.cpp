// Temporary screens replaced as features land.
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {
class Placeholder : public app::Screen {
public:
    Placeholder(const char* title, int icon) : title_(title), icon_(icon) {}
    app::Section section() const override { return app::SEC_MEDIA; }
    void frame() override {
        using namespace ui;
        float x = content_x();
        text::draw(font::display, x, 64, title_, theme().text);
        empty_state(Rect(x, 200, W - x - 60, 300), icon_, "Coming soon", "");
        focusable(id(title_), Rect(x, 200, 10, 10), 0, F_DEFAULT);
    }
private:
    const char* title_;
    int icon_;
};
}  // namespace

std::unique_ptr<app::Screen> make_reader(const std::string&) { return std::make_unique<Placeholder>("Reader", ic::BOOK); }

}  // namespace screens
