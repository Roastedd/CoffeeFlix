// Internet radio: favorites, local and worldwide top stations, genres, search.
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/radio.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

struct StationLoader {
    radio::List list;
    bool loading = false, loaded = false;
    tasks::Scope scope;
    void load(std::function<radio::List()> fn) {
        scope.reset();
        loading = true;
        scope.run<radio::List>(std::move(fn), [this](radio::List l) {
            list = std::move(l);
            loading = false;
            loaded = true;
        });
    }
};

CardInfo station_card(const radio::Station& s) {
    CardInfo c;
    c.image = s.favicon;
    c.image_w = 240;
    c.title = s.name;
    std::string meta = s.country;
    if (s.bitrate > 0) meta += (meta.empty() ? "" : " \xC2\xB7 ") + util::fmt("%d kbps", s.bitrate);
    c.subtitle = meta;
    c.icon = ic::RADIO;
    c.favorite = radio::is_favorite(s);
    return c;
}

void play_station(const radio::Station& s) {
    play_audio(radio::make_source(s));
}

void fav_station(const radio::Station& s) {
    bool on = radio::toggle_favorite(s);
    toast(on ? "Added " + s.name + " to favorites" : "Removed from favorites", on ? ic::FAVORITE : ic::FAVORITE_BORDER);
}

class StationGridScreen : public app::Screen {
public:
    StationGridScreen(std::string title, std::function<radio::List()> fn) : title_(std::move(title)), fn_(std::move(fn)) {
        loader_.load(fn_);
    }
    app::Section section() const override { return app::SEC_RADIO; }

    void frame() override {
        float x0 = content_x();
        Id g = id(id("radio_grid"), title_);
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw_fit(font::headline, x0, y, W - x0 - 60, title_, theme().text);
        y += 80;
        if (loader_.loaded && loader_.list.items.empty()) {
            if (loader_.list.error.empty()) empty_state(Rect(x0, y, W - x0 - 60, 280), ic::RADIO, "No stations found", "");
            else if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, "Couldn't load stations",
                                        loader_.list.error.c_str()))
                loader_.load(fn_);
            y += 320;
        } else {
            GridSpec gs;
            gs.count = (int)loader_.list.items.size();
            gs.cols = 6;
            gs.item_w = 160;
            gs.shape = CARD_SQUARE;
            gs.loading = !loader_.loaded;
            gs.item = [this](int i) { return station_card(loader_.list.items[i]); };
            gs.on_click = [this](int i) { play_station(loader_.list.items[i]); };
            gs.on_x = [this](int i) { fav_station(loader_.list.items[i]); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "Favorite"}, {"B", "Back"}});
    }

private:
    std::string title_;
    std::function<radio::List()> fn_;
    StationLoader loader_;
    Page page_;
};

class RadioScreen : public app::Screen {
public:
    RadioScreen() {
        country_ = store::get_str("radio_country", "US");
        std::string cc = country_;
        local_.load([cc] { return radio::by_country(cc, 40); });
        top_.load([] { return radio::top(40); });
    }
    app::Section section() const override { return app::SEC_RADIO; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("radio");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Radio", t.text);
        Id top = id(g, "top");
        if (search_bar(id(top, "search"), Rect(x0 + 230, y + 4, W - x0 - 290, 56), "", "Search 40,000+ stations", top, F_DEFAULT))
            prompt_text("Search radio stations", "", "Station name", [](std::string q) {
                if (q.empty()) return;
                app::push(std::make_unique<StationGridScreen>("\xE2\x80\x9C" + q + "\xE2\x80\x9D", [q] { return radio::search(q); }));
            });
        y += 84;

        float cx = x0;
        auto& gs = radio::genres();
        for (size_t i = 0; i < gs.size(); i++) {
            float w = text::measure(font::small_bold, gs[i].name) + 58;
            if (chip(id(top, (int64_t)i), Rect(cx, y, w, 42), gs[i].name, false, top, gs[i].icon)) {
                std::string tag = gs[i].tag;
                app::push(std::make_unique<StationGridScreen>(gs[i].name, [tag] { return radio::by_tag(tag, 60); }));
            }
            cx += w + 12;
        }
        y += 72;

        auto favs = radio::favorites();
        if (!favs.empty()) y += station_shelf(id(g, "favs"), x0, y, "Your favorites", favs, false) + 10;
        if (top_.loaded && top_.list.items.empty() && local_.loaded && local_.list.items.empty() && !top_.list.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, "Couldn't reach the radio directory",
                                   top_.list.error.c_str())) {
                std::string cc = country_;
                local_.load([cc] { return radio::by_country(cc, 40); });
                top_.load([] { return radio::top(40); });
            }
            y += 330;
        } else {
            y += station_shelf(id(g, "local"), x0, y, ("Popular in " + country_).c_str(), local_.list.items, !local_.loaded) + 10;
            y += station_shelf(id(g, "top"), x0, y, "Top stations worldwide", top_.list.items, !top_.loaded) + 10;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "Favorite"}});
    }

private:
    float station_shelf(Id sid, float x, float y, const char* title, const std::vector<radio::Station>& items, bool loading) {
        ShelfSpec s;
        s.title_str = title;
        s.count = (int)items.size();
        s.loading = loading;
        s.shape = CARD_SQUARE;
        s.item_w = 170;
        s.item = [&items](int i) { return station_card(items[i]); };
        s.on_click = [&items](int i) { play_station(items[i]); };
        s.on_x = [&items](int i) { fav_station(items[i]); };
        return shelf(sid, x, y, s, &page_);
    }

    std::string country_;
    StationLoader local_, top_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_radio() { return std::make_unique<RadioScreen>(); }

std::unique_ptr<app::Screen> make_radio_search(const std::string& q) {
    return std::make_unique<StationGridScreen>("\xE2\x80\x9C" + q + "\xE2\x80\x9D", [q] { return radio::search(q); });
}

}  // namespace screens
