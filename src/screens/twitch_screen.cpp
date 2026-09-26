// Twitch: followed channels, top streams, categories, search.
#include "core/i18n.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/twitch.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

template <typename T>
struct Async {
    T value;
    bool loading = false, loaded = false;
    tasks::Scope scope;
    std::function<T()> fn;
    void load(std::function<T()> f) {
        fn = std::move(f);
        scope.reset();
        loading = true;
        scope.run<T>(fn, [this](T v) {
            value = std::move(v);
            loading = false;
            loaded = true;
        });
    }
    void reload() {
        if (fn) load(fn);
    }
};

CardInfo stream_card(const twitch::Stream& s) {
    CardInfo c;
    c.image = s.live ? s.preview : s.avatar;
    c.image_w = 400;
    c.title = s.live ? s.title : s.name;
    c.subtitle = s.live ? s.name + (s.game.empty() ? "" : " \xC2\xB7 " + s.game) : tr("Offline");
    c.live = s.live;
    if (s.live) c.badge = util::fmt(tr("%s watching"), util::format_count(s.viewers).c_str());
    c.icon = ic::LIVE_TV;
    c.favorite = twitch::is_followed(s.login);
    return c;
}

void watch(const twitch::Stream& s) {
    if (!s.live) {
        toast(util::fmt(tr("%s is offline right now"), s.name.c_str()), ic::INFO);
        return;
    }
    play_video(twitch::make_source(s));
}

void follow(const twitch::Stream& s) {
    bool on = twitch::toggle_follow(s);
    toast(on ? util::fmt(tr("Following %s"), s.name.c_str()) : util::fmt(tr("Unfollowed %s"), s.name.c_str()),
          on ? ic::FAVORITE : ic::FAVORITE_BORDER);
}

class StreamGridScreen : public app::Screen {
public:
    StreamGridScreen(std::string title, std::function<twitch::Streams()> fn) : title_(std::move(title)) { data_.load(std::move(fn)); }
    app::Section section() const override { return app::SEC_TWITCH; }
    void frame() override {
        float x0 = content_x();
        Id g = id(id("tw_grid"), title_);
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw_fit(font::headline, x0, y, W - x0 - 60, title_, theme().text);
        y += 80;
        const auto& items = data_.value.items;
        if (data_.loaded && items.empty()) {
            if (data_.value.error.empty()) empty_state(Rect(x0, y, W - x0 - 60, 280), ic::LIVE_TV, tr("Nobody's live here"), "");
            else if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, tr("Couldn't reach Twitch"),
                                        data_.value.error.c_str()))
                data_.reload();
            y += 320;
        } else {
            GridSpec gs;
            gs.count = (int)items.size();
            gs.cols = 3;
            gs.item_w = 336;
            gs.gap_x = 26;
            gs.loading = !data_.loaded;
            gs.item = [&items](int i) { return stream_card(items[i]); };
            gs.on_click = [&items](int i) { watch(items[i]); };
            gs.on_x = [&items](int i) { follow(items[i]); };
            gs.on_focus = [&items](int i) { set_backdrop(items[i].preview); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Watch")}, {"X", tr("Follow")}, {"B", tr("Back")}});
    }

private:
    std::string title_;
    Async<twitch::Streams> data_;
    Page page_;
};

class TwitchScreen : public app::Screen {
public:
    TwitchScreen() {
        top_.load([] { return twitch::top_streams(30); });
        cats_.load([] { return twitch::top_categories(30); });
        followed_.load([] { return twitch::followed(); });
    }
    app::Section section() const override { return app::SEC_TWITCH; }
    void on_enter() override { followed_.reload(); }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("twitch");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Twitch", t.text);
        Id top = id(g, "top");
        if (search_bar(id(top, "search"), Rect(x0 + 220, y + 4, W - x0 - 280, 56), "", tr("Search channels"), top, F_DEFAULT))
            prompt_text(tr("Search Twitch"), "", tr("Channel name"), [](std::string q) {
                if (!q.empty()) app::push(make_twitch_search(q));
            });
        y += 96;
        if (focus_in_group(top)) page_.focus_range(0, y + page_.scroll());

        if (top_.loaded && top_.value.items.empty() && !top_.value.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, tr("Couldn't reach Twitch"),
                                   top_.value.error.c_str())) {
                top_.reload();
                cats_.reload();
                followed_.reload();
            }
            page_.end(y + 330 + page_.scroll());
            return;
        }

        const auto& fol = followed_.value.items;
        if (!fol.empty()) {
            ShelfSpec s;
            s.title = tr("Followed channels");
            s.count = (int)fol.size();
            s.item_w = 300;
            s.item = [&fol](int i) { return stream_card(fol[i]); };
            s.on_click = [&fol](int i) { watch(fol[i]); };
            s.on_x = [&fol](int i) { follow(fol[i]); };
            s.on_focus = [&fol](int i) { set_backdrop(fol[i].live ? fol[i].preview : fol[i].avatar); };
            y += shelf(id(g, "followed"), x0, y, s, &page_) + 10;
        }

        const auto& live = top_.value.items;
        ShelfSpec s;
        s.title = tr("Live now");
        s.count = (int)live.size();
        s.loading = !top_.loaded;
        s.item_w = 300;
        s.item = [&live](int i) { return stream_card(live[i]); };
        s.on_click = [&live](int i) { watch(live[i]); };
        s.on_x = [&live](int i) { follow(live[i]); };
        s.on_focus = [&live](int i) { set_backdrop(live[i].preview); };
        y += shelf(id(g, "live"), x0, y, s, &page_) + 10;

        const auto& cats = cats_.value.items;
        ShelfSpec c;
        c.title = tr("Top categories");
        c.count = (int)cats.size();
        c.loading = !cats_.loaded;
        c.shape = CARD_POSTER;
        c.item_w = 150;
        c.item = [&cats](int i) {
            CardInfo ci;
            ci.image = cats[i].box_art;
            ci.image_w = 285;
            ci.title = cats[i].name;
            ci.subtitle = util::fmt(tr("%s watching"), util::format_count(cats[i].viewers).c_str());
            ci.icon = ic::SPORTS_ESPORTS;
            return ci;
        };
        c.on_click = [&cats](int i) {
            std::string name = cats[i].name;
            app::push(std::make_unique<StreamGridScreen>(name, [name] { return twitch::game_streams(name); }));
        };
        y += shelf(id(g, "cats"), x0, y, c, &page_) + 10;
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Watch")}, {"X", tr("Follow")}});
    }

private:
    Async<twitch::Streams> top_, followed_;
    Async<twitch::Categories> cats_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_twitch() { return std::make_unique<TwitchScreen>(); }

std::unique_ptr<app::Screen> make_twitch_search(const std::string& q) {
    return std::make_unique<StreamGridScreen>("\xE2\x80\x9C" + q + "\xE2\x80\x9D", [q] { return twitch::search(q); });
}

}  // namespace screens
