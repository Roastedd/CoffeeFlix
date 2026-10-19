#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/game_library.hpp"

#include <memory>
#include <string>
#include <vector>

#include "core/tasks.hpp"
#include "gfx/text.hpp"
#include "ui/ui.hpp"

namespace screens {
namespace {
using namespace ui;

#ifdef __WIIU__
using game_library::Game;

class GamesScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_GAMES; }
    void on_enter() override { if (!loading_) Refresh(false); }
    void frame() override {
        const float x = content_x();
        const Id base = id("games-library");
        page_.begin(id(base, "page"));
        float y = page_.y(49);
        section_title(x, y, "Games", "Your installed Wii U and Wii Virtual Console games");
        y += 104;
        Id controls = id(base, "filters");
        if (chip(id(controls, "all"), Rect(x, y, 115, 42), "All games", filter_ == 0, controls)) filter_ = 0;
        if (chip(id(controls, "mmc"), Rect(x + 128, y, 141, 42), "Internal", filter_ == 1, controls, ic::STORAGE)) filter_ = 1;
        if (chip(id(controls, "usb"), Rect(x + 283, y, 120, 42), "USB", filter_ == 2, controls, ic::USB)) filter_ = 2;
        if (button(id(controls, "refresh"), Rect(W - 210, y, 144, 42), "Refresh", ic::REFRESH, BTN_GHOST, controls)) Refresh(true);
        if (input().pressed_(BTN_Y) && !menu_active()) { input().eat(BTN_Y); Refresh(true); }
        if (loading_ && !games_.empty()) {
            const std::string checking = std::to_string(progress_->checked.load()) + " titles checked";
            text::draw(font::caption, W - 210, y + 49, checking, theme().text2);
        }
        y += 79;

        std::vector<size_t> visible;
        for (size_t i = 0; i < games_.size(); ++i) {
            if (filter_ == 0 || (filter_ == 1 && !games_[i].usb) || (filter_ == 2 && games_[i].usb)) visible.push_back(i);
        }
        if (loading_ && visible.empty()) {
            const std::string label = progress_->phase.load() == 2 ? "Scanning USB games" : "Scanning internal games";
            loading_indicator(x + (W - x - 64) * 0.5f, y + 85, label.c_str());
            const std::string checked = std::to_string(progress_->checked.load()) + " titles checked";
            text::draw(font::caption, x + (W - x - 64) * 0.5f, y + 147, checked, theme().text2, text::CENTER);
            GridSpec placeholders;
            placeholders.cols = 5;
            placeholders.item_w = 186;
            placeholders.gap_x = 24;
            placeholders.shape = CARD_SQUARE;
            placeholders.loading = true;
            y += 195 + grid(id(base, "loading-tiles"), x, y + 195, placeholders, &page_);
        } else if (visible.empty()) {
            empty_state(Rect(x, y, W - x - 64, 320), ic::GAMEPAD,
                        failed_ ? "Could not read games" : "No installed games found",
                        failed_ ? "Press Y to retry reading the Wii U library." : "Choose another storage device or refresh the library.");
            y += 340;
        } else {
            GridSpec gridSpec;
            gridSpec.count = static_cast<int>(visible.size());
            gridSpec.cols = 5;
            gridSpec.item_w = 186;
            gridSpec.gap_x = 24;
            gridSpec.shape = CARD_SQUARE;
            gridSpec.item = [this, &visible](int i) {
                const Game& game = games_[visible[i]];
                CardInfo card;
                card.image = game.icon;
                card.image_inset = 12;
                card.title = game.name;
                card.subtitle = game.usb ? "USB storage" : "Internal memory";
                card.tag = game.wii ? "Wii VC" : "Wii U";
                card.icon = ic::GAMEPAD;
                return card;
            };
            gridSpec.on_click = [this, &visible](int i) { Open(games_[visible[i]]); };
            y += grid(id(base, "tiles"), x, y, gridSpec, &page_);
        }
        page_.end(y + page_.scroll() + 10);
        hint_bar({{"A", "Launch"}, {"Y", "Refresh"}, {"B", "Back"}});
    }
private:
    void Refresh(bool full) {
        if (loading_) return;
        if (!loaded_) { games_ = game_library::cached(); loaded_ = true; }
        scope_.reset();
        progress_ = std::make_shared<game_library::Progress>();
        loading_ = true;
        failed_ = false;
        auto progress = progress_;
        scope_.run<game_library::Result>([progress, full] { return game_library::scan(progress, full); },
            [this](game_library::Result result) {
                games_ = std::move(result.games);
                failed_ = result.failed;
                loaded_ = true;
                loading_ = false;
            });
    }
    void Open(const Game& game) {
        if (!game.wii) {
            show_menu(game.name, game.usb ? "Installed on USB" : "Installed on internal memory",
                {{"Start game", ic::GAMEPAD, [game] { game_library::launch(game, 1); }}});
            return;
        }
        std::vector<MenuItem> choices;
        if (game.gamepad) choices.push_back({"Use GamePad as controller", ic::GAMEPAD, [game] { game_library::launch(game, 0); }});
        choices.push_back({"TV only", ic::TV, [game] { game_library::launch(game, 1); }});
        choices.push_back({"TV and GamePad screen", ic::GAMEPAD, [game] { game_library::launch(game, 2); }});
        choices.push_back({"GamePad screen only", ic::GAMEPAD, [game] { game_library::launch(game, 3); }});
        show_menu(game.name, "Wii VC - choose your play mode", std::move(choices));
    }
    bool loaded_ = false;
    bool loading_ = false;
    bool failed_ = false;
    int filter_ = 0;
    Page page_;
    tasks::Scope scope_;
    std::shared_ptr<game_library::Progress> progress_ = std::make_shared<game_library::Progress>();
    std::vector<Game> games_;
};
#else
class GamesScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_GAMES; }
    void frame() override {
        empty_state(Rect(content_x(), 200, W - content_x() - 60, 320), ic::GAMEPAD,
                    "Wii U games", "The games library is available on Wii U.");
    }
};
#endif
}
std::unique_ptr<app::Screen> make_games() { return std::make_unique<GamesScreen>(); }
}
