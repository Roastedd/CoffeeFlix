// Podcasts: subscriptions, charts, search and show pages.
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/images.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/podcasts.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

CardInfo show_card(const podcasts::Show& s) {
    CardInfo c;
    c.image = s.artwork;
    c.image_w = 300;
    c.title = s.title;
    c.subtitle = s.author;
    c.icon = ic::PODCASTS;
    return c;
}

std::unique_ptr<app::Screen> make_show(const podcasts::Show& s);

struct ShowsLoader {
    podcasts::Shows shows;
    bool loading = false, loaded = false;
    tasks::Scope scope;
    std::function<podcasts::Shows()> fn;
    void load(std::function<podcasts::Shows()> f) {
        fn = f;
        scope.reset();
        loading = true;
        scope.run<podcasts::Shows>(std::move(f), [this](podcasts::Shows s) {
            shows = std::move(s);
            loading = false;
            loaded = true;
        });
    }
};

class ShowGridScreen : public app::Screen {
public:
    ShowGridScreen(std::string title, std::function<podcasts::Shows()> fn) : title_(std::move(title)) { loader_.load(std::move(fn)); }
    app::Section section() const override { return app::SEC_PODCASTS; }
    void frame() override {
        float x0 = content_x();
        Id g = id(id("pod_grid"), title_);
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw_fit(font::headline, x0, y, W - x0 - 60, title_, theme().text);
        y += 80;
        if (loader_.loaded && loader_.shows.items.empty()) {
            if (loader_.shows.error.empty()) empty_state(Rect(x0, y, W - x0 - 60, 280), ic::PODCASTS, "No podcasts found", "");
            else if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, "Couldn't search podcasts",
                                        loader_.shows.error.c_str()))
                loader_.load(loader_.fn);
            y += 320;
        } else {
            GridSpec gs;
            gs.count = (int)loader_.shows.items.size();
            gs.cols = 6;
            gs.item_w = 160;
            gs.shape = CARD_SQUARE;
            gs.loading = !loader_.loaded;
            gs.item = [this](int i) { return show_card(loader_.shows.items[i]); };
            gs.on_click = [this](int i) { app::push(make_show(loader_.shows.items[i])); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
    }

private:
    std::string title_;
    ShowsLoader loader_;
    Page page_;
};

class ShowScreen : public app::Screen {
public:
    explicit ShowScreen(podcasts::Show s) : show_(std::move(s)) { load(); }
    app::Section section() const override { return app::SEC_PODCASTS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(id("pod_show"), show_.feed_url);
        set_backdrop(show_.artwork);
        page_.begin(id(g, "page"));
        float y = page_.y(56);

        Rect art(x0, y, 220, 220);
        gfx::shadow(art, 26, Color(0, 0, 0, 170));
        const images::Image* img = images::get(show_.artwork, 440, 440);
        if (img && img->ready) gfx::image_cover(img->tex, img->w, img->h, art, 18);
        else {
            gfx::fill_rrect_vgrad(art, 18, t.accent, t.accent2);
            text::icon(ic::PODCASTS, 80, art.cx(), art.cy(), gfx::WHITE);
        }
        float tx = art.r() + 36, tw = W - tx - 60;
        text::draw_wrapped(font::headline, Rect(tx, y, tw, 90), show_.title, t.text, 2);
        float ty = y + text::measure_wrapped(font::headline, tw, show_.title, 2) + 6;
        text::draw_fit(font::body_bold, tx, ty, tw, show_.author, t.text2);
        ty += 36;
        if (!feed_.description.empty()) ty += text::draw_wrapped(font::small, Rect(tx, ty, tw, 70), feed_.description, t.text3, 3) + 12;
        Id ag = id(g, "actions");
        bool sub = podcasts::is_subscribed(show_.feed_url);
        float bw = measure_button(sub ? "Subscribed" : "Subscribe", sub ? ic::CHECK : ic::ADD);
        if (button(id(ag, "sub"), Rect(tx, std::max(ty, y + 160), bw, 52), sub ? "Subscribed" : "Subscribe", sub ? ic::CHECK : ic::ADD,
                   sub ? BTN_NORMAL : BTN_PRIMARY, ag, F_DEFAULT)) {
            bool on = podcasts::toggle_subscription(show_);
            toast(on ? "Subscribed to " + show_.title : "Unsubscribed", on ? ic::CHECK_CIRCLE : ic::REMOVE);
        }
        y += 256;

        text::draw(font::title, x0, y, "Episodes", t.text);
        y += 52;
        if (loading_) {
            loading_indicator(x0 + 40, y + 20, nullptr);
            y += 80;
        } else if (!feed_.ok) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::ERROR_OUTLINE, "Couldn't load episodes",
                                   feed_.error.c_str()))
                load();
            y += 320;
        } else {
            Id eg = id(g, "eps");
            for (size_t i = 0; i < feed_.episodes.size(); i++) {
                const podcasts::Episode& e = feed_.episodes[i];
                bool focused_prev = focused_ == (int)i;
                float rh = focused_prev && !e.description.empty() ? 128 : 76;
                Rect r(x0, y, W - x0 - 60, rh - 8);
                bool visible = r.b() > -20 && r.y < H + 20;
                Id iid = id(eg, (int64_t)i);
                Item it = focusable(iid, r, eg);
                if (it.focused) {
                    focused_ = (int)i;
                    page_.focus_range(r.y + page_.scroll() - 20, r.b() + page_.scroll() + 20);
                }
                if (visible) draw_episode(e, r, it, iid);
                if (it.clicked) play_audio(podcasts::make_source(show_, e));
                y += rh;
            }
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"B", "Back"}});
    }

private:
    void draw_episode(const podcasts::Episode& e, const Rect& r, const Item& it, Id iid) {
        const Theme& t = theme();
        Rect br = r.offset(bump_x(iid), bump_y(iid));
        if (it.f > 0.01f) gfx::shadow(br, 16, Color(0, 0, 0, (uint8_t)(90 * it.f)));
        gfx::fill_rrect(br, 14, gfx::lerp(t.surface, t.surface_focus, it.f));
        Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f), fg2 = gfx::lerp(t.text3, Color(21, 18, 26, 160), it.f);
        text::icon(ic::PLAY_CIRCLE, 34, br.x + 34, br.y + 34, gfx::lerp(t.accent, gfx::rgb(0x15121A), it.f));
        text::draw_fit(font::body_bold, br.x + 66, br.y + 12, br.w - 90, e.title, fg);
        std::string meta = e.published;
        if (e.duration > 0) meta += (meta.empty() ? "" : " \xC2\xB7 ") + util::format_duration(e.duration);
        double pos = store::resume_position("podcast", e.guid);
        if (pos > 0) meta += " \xC2\xB7 " + util::format_duration(std::max(0.0, e.duration - pos)) + " left";
        text::draw_fit(font::small, br.x + 66, br.y + 40, br.w - 90, meta, fg2);
        if (pos > 0 && e.duration > 0) {
            Rect pb(br.x + 66, br.y + 64, 160, 3);
            gfx::fill_rrect(pb, 1.5f, Color(128, 128, 128, 90));
            gfx::fill_rrect(Rect(pb.x, pb.y, pb.w * (float)std::min(1.0, pos / e.duration), 3), 1.5f, t.accent);
        }
        if (br.h > 100 && it.f > 0.3f)
            text::draw_wrapped(font::small, Rect(br.x + 66, br.y + 70, br.w - 90, 50), e.description, fg2, 2);
    }

    void load() {
        loading_ = true;
        std::string url = show_.feed_url;
        scope_.run<podcasts::Feed>([url] { return podcasts::load(url); }, [this](podcasts::Feed f) {
            loading_ = false;
            if (f.ok && show_.artwork.empty()) show_.artwork = f.show.artwork;
            if (f.ok && show_.author.empty()) show_.author = f.show.author;
            feed_ = std::move(f);
        });
    }

    podcasts::Show show_;
    podcasts::Feed feed_;
    bool loading_ = true;
    int focused_ = -1;
    tasks::Scope scope_;
    Page page_;
};

class PodcastsScreen : public app::Screen {
public:
    PodcastsScreen() {
        std::string cc = util::lower(store::get_str("radio_country", "US"));
        top_.load([cc] { return podcasts::top(cc); });
    }
    app::Section section() const override { return app::SEC_PODCASTS; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("podcasts");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Podcasts", t.text);
        Id top = id(g, "top");
        if (search_bar(id(top, "search"), Rect(x0 + 290, y + 4, W - x0 - 350, 56), "", "Search podcasts", top, F_DEFAULT))
            prompt_text("Search podcasts", "", "Show or topic", [](std::string q) {
                if (!q.empty()) app::push(make_podcast_search(q));
            });
        y += 96;

        // Continue listening
        std::vector<store::Resume> resume;
        for (auto& r : store::resume_list(30))
            if (r.service == "podcast") resume.push_back(r);
        if (!resume.empty()) {
            ShelfSpec s;
            s.title = "Continue listening";
            s.count = (int)resume.size();
            s.shape = CARD_SQUARE;
            s.item_w = 170;
            s.item = [resume](int i) {
                CardInfo c;
                c.image = resume[i].image;
                c.title = resume[i].title;
                c.subtitle = resume[i].subtitle;
                c.icon = ic::PODCASTS;
                c.progress = resume[i].duration > 0 ? (float)(resume[i].position / resume[i].duration) : 0;
                return c;
            };
            s.on_click = [resume](int i) {
                const store::Resume& r = resume[i];
                player::Source src = podcasts::source_from_resume(r.id, r.title, r.subtitle, r.image, r.extra);
                if (!src.url.empty()) play_audio(src);
                else app::push(make_show(podcasts::Show{r.subtitle, "", r.image, podcasts::feed_of_resume(r.extra), ""}));
            };
            y += shelf(id(g, "resume"), x0, y, s, &page_) + 10;
        }

        auto subs = podcasts::subscriptions();
        if (!subs.empty()) {
            ShelfSpec s;
            s.title = "Your subscriptions";
            s.count = (int)subs.size();
            s.shape = CARD_SQUARE;
            s.item_w = 170;
            s.item = [subs](int i) { return show_card(subs[i]); };
            s.on_click = [subs](int i) { app::push(make_show(subs[i])); };
            y += shelf(id(g, "subs"), x0, y, s, &page_) + 10;
        }

        if (top_.loaded && top_.shows.items.empty() && !top_.shows.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF, "Couldn't load top podcasts",
                                   top_.shows.error.c_str()))
                top_.load(top_.fn);
            y += 320;
        } else {
            ShelfSpec s;
            s.title = "Top podcasts";
            s.count = (int)top_.shows.items.size();
            s.loading = !top_.loaded;
            s.shape = CARD_SQUARE;
            s.item_w = 170;
            s.item = [this](int i) { return show_card(top_.shows.items[i]); };
            s.on_click = [this](int i) { app::push(make_show(top_.shows.items[i])); };
            y += shelf(id(g, "top"), x0, y, s, &page_) + 10;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Open"}});
    }

private:
    ShowsLoader top_;
    Page page_;
};

std::unique_ptr<app::Screen> make_show(const podcasts::Show& s) { return std::make_unique<ShowScreen>(s); }

}  // namespace

std::unique_ptr<app::Screen> make_podcasts() { return std::make_unique<PodcastsScreen>(); }

std::unique_ptr<app::Screen> make_podcast_show(const std::string& feed_url, const std::string& title,
                                               const std::string& author, const std::string& artwork) {
    return make_show(podcasts::Show{title, author, artwork, feed_url, ""});
}

std::unique_ptr<app::Screen> make_podcast_search(const std::string& q) {
    return std::make_unique<ShowGridScreen>("\xE2\x80\x9C" + q + "\xE2\x80\x9D", [q] { return podcasts::search(q); });
}

}  // namespace screens
