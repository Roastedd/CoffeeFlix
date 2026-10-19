#include "screens/screens.hpp"
#include "screens/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/tasks.hpp"

#include "gfx/text.hpp"
#include "platform/platform.hpp"
#include "ui/ui.hpp"

#ifdef __WIIU__
#include <coreinit/mcp.h>
#include <nn/acp/title.h>
#include <nn/cmpt/cmpt.h>
#include <sysapp/launch.h>

extern "C" int32_t CMPTAcctSetDrcCtrlEnabled(int32_t enabled);
#endif

namespace screens {
namespace {
using namespace ui;

#ifdef __WIIU__
struct Game {
    uint64_t titleId = 0;
    std::string name;
    std::string path;
    std::string icon;
    bool usb = false;
    bool wii = false;
    bool gamepad = false;
};

std::string Field(const char* value, size_t limit) {
    size_t size = 0;
    while (size < limit && value[size]) ++size;
    std::string result(value, size);
    for (auto& ch : result) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    return result;
}

std::string TitleId(uint64_t id) {
    char value[20];
    std::snprintf(value, sizeof(value), "%016llX", static_cast<unsigned long long>(id));
    return value;
}

struct ScanProgress {
    std::atomic<int> phase{0};
    std::atomic<int> checked{0};
};

struct ScanResult {
    std::vector<Game> games;
    bool failed = false;
};

ScanResult ScanGames(const std::shared_ptr<ScanProgress>& progress,
                     const std::weak_ptr<std::atomic<bool>>& token) {
    ScanResult result;
        const int handle = MCP_Open();
        if (handle < 0) { result.failed = true; return result; }
        const MCPError count = MCP_TitleCount(handle);
        const uint32_t capacity = std::min<uint32_t>(2048, count > 0 ? static_cast<uint32_t>(count) + 32 : 512);
        std::set<std::pair<uint64_t, bool>> encountered;
        for (int disk = 0; disk < 2; ++disk) {
            progress->phase.store(disk + 1);
            std::vector<MCPTitleListType> list(capacity);
            uint32_t returned = 0;
            const MCPDeviceType device = disk ? MCP_DEVICE_TYPE_USB : MCP_DEVICE_TYPE_MLC;
            if (MCP_TitleListByDeviceType(handle, device, &returned, list.data(),
                    static_cast<uint32_t>(list.size() * sizeof(MCPTitleListType))) != 0) continue;
            for (uint32_t i = 0; i < std::min(returned, capacity); ++i) {
                const auto alive = token.lock();
                if (!alive || !alive->load()) { MCP_Close(handle); return result; }
                progress->checked.fetch_add(1);
                const MCPTitleListType& item = list[i];
                const bool wii = item.appType == MCP_APP_TYPE_GAME_WII || (item.titleId >> 32) == 0x00050002;
                if (!wii && item.appType != MCP_APP_TYPE_GAME) continue;
                if (!encountered.insert({item.titleId, disk != 0}).second) continue;
                Game game;
                game.titleId = item.titleId;
                game.name = TitleId(item.titleId);
                game.path = Field(item.path, sizeof(item.path));
                game.usb = disk != 0;
                game.wii = wii;
                alignas(0x40) ACPMetaXml meta{};
                if (ACPGetTitleMetaXml(item.titleId, &meta) == 0) {
                    game.gamepad = meta.drc_use == 65537;
                    std::string title = Field(meta.longname_en, sizeof(meta.longname_en));
                    if (title.empty()) title = Field(meta.shortname_en, sizeof(meta.shortname_en));
                    if (!title.empty()) game.name = std::move(title);
                }
                char directory[1024] = {};
                if (ACPGetTitleMetaDirByDevice(item.titleId, directory, sizeof(directory),
                        disk ? ACP_DEVICE_TYPE_USB : ACP_DEVICE_TYPE_MLC) == 0 && directory[0]) {
                    game.icon = std::string(directory) + "/iconTex.tga";
                } else if (!game.path.empty()) {
                    game.icon = game.path + "/meta/iconTex.tga";
                }
                result.games.push_back(std::move(game));
            }
        }
        MCP_Close(handle);
        std::sort(result.games.begin(), result.games.end(), [](const Game& a, const Game& b) {
            if (a.name == b.name) return a.usb < b.usb;
            return a.name < b.name;
        });
        return result;
    }

class GamesScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_GAMES; }
    void on_enter() override { if (!loaded_ && !loading_) Refresh(); }
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
        if (button(id(controls, "refresh"), Rect(W - 210, y, 144, 42), "Refresh", ic::REFRESH, BTN_GHOST, controls)) Refresh();
        if (input().pressed_(BTN_Y) && !menu_active()) { input().eat(BTN_Y); Refresh(); }
        y += 79;

        std::vector<size_t> visible;
        for (size_t i = 0; i < games_.size(); ++i) {
            if (filter_ == 0 || (filter_ == 1 && !games_[i].usb) || (filter_ == 2 && games_[i].usb)) visible.push_back(i);
        }
        if (loading_) {
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
    void Refresh() {
        if (loading_) return;
        scope_.reset();
        progress_ = std::make_shared<ScanProgress>();
        loading_ = true;
        loaded_ = false;
        failed_ = false;
        games_.clear();
        auto progress = progress_;
        auto token = std::weak_ptr<std::atomic<bool>>(scope_.token());
        scope_.run<ScanResult>([progress, token] { return ScanGames(progress, token); },
            [this](ScanResult result) {
                games_ = std::move(result.games);
                failed_ = result.failed;
                loaded_ = true;
                loading_ = false;
            });
    }
    static void Launch(const Game& game, int option) {
        if (game.wii) {
            const bool useGamePad = option == 0 && game.gamepad;
            const CmptScreenType screen = useGamePad ? CMPT_SCREEN_TYPE_BOTH :
                option == 1 ? CMPT_SCREEN_TYPE_TV :
                option == 2 ? CMPT_SCREEN_TYPE_BOTH : CMPT_SCREEN_TYPE_DRC;
            const int32_t controller = CMPTAcctSetDrcCtrlEnabled(useGamePad ? 1 : 0);
            const int32_t display = CMPTAcctSetScreenType(screen);
            if (controller < 0 || display < 0) {
                toast("Could not configure Wii VC controller mode", ic::ERROR_OUTLINE);
                return;
            }
            if (CMPTCheckScreenState() < 0 && screen != CMPT_SCREEN_TYPE_DRC) CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_DRC);
            if (!game.path.empty()) {
                _SYSLaunchTitleByPathFromLauncher(game.path.c_str(), static_cast<uint32_t>(game.path.size()));
                return;
            }
        }
        SYSLaunchTitle(game.titleId);
    }
    void Open(const Game& game) {
        if (!game.wii) {
            show_menu(game.name, game.usb ? "Installed on USB" : "Installed on internal memory",
                {{"Start game", ic::GAMEPAD, [game] { Launch(game, 1); }}});
            return;
        }
        std::vector<MenuItem> choices;
        if (game.gamepad) choices.push_back({"Use GamePad as controller", ic::GAMEPAD, [game] { Launch(game, 0); }});
        choices.push_back({"TV only", ic::TV, [game] { Launch(game, 1); }});
        choices.push_back({"TV and GamePad screen", ic::GAMEPAD, [game] { Launch(game, 2); }});
        choices.push_back({"GamePad screen only", ic::GAMEPAD, [game] { Launch(game, 3); }});
        show_menu(game.name, "Wii VC - choose your play mode", std::move(choices));
    }
    bool loaded_ = false;
    bool loading_ = false;
    bool failed_ = false;
    int filter_ = 0;
    Page page_;
    tasks::Scope scope_;
    std::shared_ptr<ScanProgress> progress_ = std::make_shared<ScanProgress>();
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
