// YouTube: home shelves, on-device recommendations, topics, subscriptions (local, no account), search.
#include <algorithm>

#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/youtube.hpp"
#include "services/yt_recs.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

// A lazily loaded list of videos with pagination.
struct Feed {
    youtube::Results res;
    bool loading = false, loaded = false;
    tasks::Scope scope;
    std::function<youtube::Results(const std::string& continuation)> fetch;

    void load() {
        if (loading || loaded) return;
        loading = true;
        auto f = fetch;
        scope.run<youtube::Results>([f] { return f(""); }, [this](youtube::Results r) {
            res = std::move(r);
            loading = false;
            loaded = true;
        });
    }
    void more() {
        if (loading || res.continuation.empty()) return;
        loading = true;
        auto f = fetch;
        std::string c = res.continuation;
        scope.run<youtube::Results>([f, c] { return f(c); }, [this](youtube::Results r) {
            loading = false;
            res.continuation = r.continuation;
            for (auto& v : r.items) {
                bool dup = std::any_of(res.items.begin(), res.items.end(), [&](const youtube::Video& o) { return o.id == v.id; });
                if (!dup) res.items.push_back(std::move(v));
            }
        });
    }
    void reload() {
        scope.reset();
        loading = loaded = false;
        res = youtube::Results();
        load();
    }
};

CardInfo video_card(const youtube::Video& v) {
    CardInfo c;
    c.image = youtube::thumbnail(v.id);
    c.image_w = 360;
    c.title = v.title;
    std::string sub = v.channel;
    if (!v.views.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + v.views;
    if (!v.published.empty()) sub += " \xC2\xB7 " + v.published;
    c.subtitle = sub;
    c.badge = v.live ? "" : v.duration;
    c.live = v.live;
    c.icon = ic::SMART_DISPLAY;
    double pos = store::resume_position("youtube", v.id);
    if (pos > 0) c.progress = 0.35f;
    return c;
}

void play(const youtube::Video& v) {
    store::add_recent_search("yt_history", v.id + "\x1f" + v.title + "\x1f" + v.channel);
    play_video(youtube::make_source(v));
}

void toggle_subscription(const youtube::Video& v) {
    if (v.channel_id.empty()) {
        toast("Channel unavailable for this video", ic::INFO);
        return;
    }
    store::Fav f{v.channel_id, v.channel, "", youtube::thumbnail(v.id), ""};
    bool on = store::fav_toggle("yt_channel", f);
    yt_recs::on_subscribe(v.channel_id, on);
    toast(on ? "Subscribed to " + v.channel : "Unsubscribed from " + v.channel, on ? ic::CHECK_CIRCLE : ic::REMOVE);
}

std::unique_ptr<app::Screen> make_channel(const std::string& id, const std::string& name);

// --- results grid (search / topic / channel) -----------------------------------

class VideoGridScreen : public app::Screen {
public:
    VideoGridScreen(std::string title, std::string subtitle, std::function<youtube::Results(const std::string&)> fetch)
        : title_(std::move(title)), subtitle_(std::move(subtitle)) {
        feed_.fetch = std::move(fetch);
        feed_.load();
    }
    app::Section section() const override { return app::SEC_YOUTUBE; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(id("ytgrid"), title_);
        page_.begin(id(g, "page"));
        float y = page_.y(56);
        text::draw_fit(font::headline, x0, y, W - x0 - 200, title_, t.text);
        if (!subtitle_.empty()) text::draw(font::body, x0, y + 46, subtitle_, t.text2);
        y += 100;

        if (feed_.loaded && feed_.res.items.empty()) {
            if (feed_.res.error.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 280), ic::SEARCH, "No videos found", "Try a different search.");
            } else if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF,
                                          "Couldn't reach YouTube", feed_.res.error.c_str())) {
                feed_.reload();
            }
            y += 330;
        } else {
            GridSpec gs;
            gs.count = (int)feed_.res.items.size();
            gs.cols = 3;
            gs.item_w = 336;
            gs.gap_x = 26;
            gs.loading = feed_.loading;
            gs.item = [this](int i) { return video_card(feed_.res.items[i]); };
            gs.on_click = [this](int i) { play(feed_.res.items[i]); };
            gs.on_focus = [this](int i) { set_backdrop(youtube::thumbnail_hq(feed_.res.items[i].id)); };
            gs.on_x = [this](int i) { toggle_subscription(feed_.res.items[i]); };
            gs.on_reach_end = [this] { feed_.more(); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "Subscribe"}, {"B", "Back"}});
    }

private:
    std::string title_, subtitle_;
    Feed feed_;
    Page page_;
};

// --- home -------------------------------------------------------------------------

class YouTubeScreen : public app::Screen {
public:
    YouTubeScreen() {
        trending_.fetch = [](const std::string& c) {
            return c.empty() ? youtube::trending() : youtube::Results();
        };
        trending_.load();
        for (auto& tp : youtube::topics()) {
            auto f = std::make_unique<Feed>();
            std::string q = tp.query;
            f->fetch = [q](const std::string& c) { return youtube::search(q, youtube::PARAMS_POPULAR_WEEK, c); };
            topic_feeds_.push_back(std::move(f));
        }
        load_subscriptions();
        foryou_.fetch = [](const std::string& c) { return c.empty() ? yt_recs::for_you() : youtube::Results(); };
        load_for_you();
    }

    app::Section section() const override { return app::SEC_YOUTUBE; }

    void on_enter() override {
        if (subs_count_ != store::favs("yt_channel").size()) load_subscriptions();
        // Rebuild once something new was learned (a watch, search, subscription...).
        if (yt_recs::version() != foryou_version_ && !foryou_.loading) load_for_you();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("youtube");
        page_.begin(id(g, "page"));

        float y = page_.y(52);
        text::draw(font::display, x0, y, "YouTube", t.text);
        Id top = id(g, "top");
        if (search_bar(id(top, "search"), Rect(x0 + 300, y + 4, W - x0 - 360, 56), "", "Search YouTube", top, F_DEFAULT))
            open_search();
        y += 84;

        // Topic chips
        float cx = x0;
        auto& tps = youtube::topics();
        for (size_t i = 0; i < tps.size(); i++) {
            float w = text::measure(font::small_bold, tps[i].name) + 58;
            if (chip(id(top, (int64_t)i), Rect(cx, y, w, 42), tps[i].name, false, top, tps[i].icon)) {
                std::string q = tps[i].query;
                app::push(std::make_unique<VideoGridScreen>(tps[i].name, "Popular this week", [q](const std::string& c) {
                    return youtube::search(q, youtube::PARAMS_POPULAR_WEEK, c);
                }));
            }
            cx += w + 12;
        }
        y += 70;

        // Continue watching (YouTube only)
        std::vector<store::Resume> resume;
        for (auto& r : store::resume_list(30))
            if (r.service == "youtube") resume.push_back(r);
        if (!resume.empty()) {
            ShelfSpec s;
            s.title = "Continue watching";
            s.count = (int)resume.size();
            s.item_w = 300;
            s.item = [resume](int i) {
                CardInfo c;
                c.image = youtube::thumbnail(resume[i].id);
                c.title = resume[i].title;
                c.subtitle = resume[i].subtitle;
                c.progress = resume[i].duration > 0 ? (float)(resume[i].position / resume[i].duration) : 0;
                c.badge = util::format_duration(resume[i].duration - resume[i].position) + " left";
                return c;
            };
            s.on_click = [resume](int i) {
                youtube::Video v;
                v.id = resume[i].id;
                v.title = resume[i].title;
                v.channel = resume[i].subtitle;
                play(v);
            };
            s.on_focus = [resume](int i) { set_backdrop(youtube::thumbnail_hq(resume[i].id)); };
            y += shelf(id(g, "resume"), x0, y, s, &page_) + 12;
        }

        if (trending_.loaded && trending_.res.items.empty() && !trending_.res.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, "Couldn't reach YouTube",
                                   trending_.res.error.c_str())) {
                trending_.reload();
                for (auto& f : topic_feeds_) f->reload();
            }
            page_.end(y + 330 + page_.scroll());
            hint_bar({{"A", "Select"}});
            return;
        }
        if (personal_) y += video_shelf(id(g, "foryou"), x0, y, "For you", foryou_) + 12;
        y += video_shelf(id(g, "trending"), x0, y, "Popular this week", trending_) + 12;

        if (!subs_.res.items.empty() || subs_.loading) {
            y += video_shelf(id(g, "subs"), x0, y, "From your subscriptions", subs_) + 12;
            auto chans = store::favs("yt_channel");
            ShelfSpec s;
            s.title = "Channels";
            s.count = (int)chans.size();
            s.shape = CARD_CIRCLE;
            s.item_w = 130;
            s.item = [chans](int i) {
                CardInfo c;
                c.title = chans[i].title;
                c.image = chans[i].image;
                c.icon = ic::PERSON;
                return c;
            };
            s.on_click = [chans](int i) { app::push(make_channel(chans[i].id, chans[i].title)); };
            y += shelf(id(g, "chans"), x0, y, s, &page_) + 12;
        }

        for (size_t i = 0; i < topic_feeds_.size(); i++) {
            Feed& f = *topic_feeds_[i];
            if (y < H + 300) f.load();  // lazy: only when about to scroll into view
            y += video_shelf(id(g, (int64_t)(100 + i)), x0, y, tps[i].name, f) + 12;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "Subscribe"}, {"Y", "Not interested"}});
    }

private:
    void load_for_you() {
        personal_ = yt_recs::has_profile();
        foryou_version_ = yt_recs::version();
        if (personal_) foryou_.reload();
    }

    float video_shelf(Id sid, float x, float y, const char* title, Feed& f) {
        if (f.loaded && f.res.items.empty()) {
            if (!f.res.error.empty()) {
                text::draw(font::title, x, y, title, theme().text);
                text::draw(font::body, x, y + 46, "Couldn't load: " + f.res.error, theme().text3);
                return 90;
            }
            return 0;
        }
        ShelfSpec s;
        s.title = title;
        s.count = (int)f.res.items.size();
        s.loading = f.loading || !f.loaded;
        s.item_w = 300;
        s.item = [&f](int i) { return video_card(f.res.items[i]); };
        s.on_click = [&f](int i) { play(f.res.items[i]); };
        s.on_focus = [&f](int i) {
            set_backdrop(youtube::thumbnail_hq(f.res.items[i].id));
            if (i >= (int)f.res.items.size() - 4) f.more();
        };
        s.on_x = [&f](int i) { toggle_subscription(f.res.items[i]); };
        s.on_y = [&f](int i) {
            yt_recs::not_interested(f.res.items[i]);
            f.res.items.erase(f.res.items.begin() + i);
            toast("Got it, you'll see less like this", ic::CHECK_CIRCLE);
        };
        return shelf(sid, x, y, s, &page_);
    }

    void open_search() {
        prompt_text("Search YouTube", "", "Search YouTube", [](std::string q) {
            if (q.empty()) return;
            store::add_recent_search("youtube", q);
            yt_recs::on_search(q);
            app::push(std::make_unique<VideoGridScreen>(q, "Search results", [q](const std::string& c) {
                return youtube::search(q, youtube::PARAMS_VIDEOS, c);
            }));
        });
    }

    void load_subscriptions() {
        auto chans = store::favs("yt_channel");
        subs_count_ = chans.size();
        if (chans.empty()) {
            subs_.scope.reset();
            subs_.res = youtube::Results();
            subs_.loaded = true;
            return;
        }
        std::vector<std::string> ids;
        for (auto& c : chans) ids.push_back(c.id);
        subs_.fetch = [ids](const std::string&) {
            // Newest few from each channel, interleaved.
            std::vector<youtube::Results> per;
            for (size_t i = 0; i < ids.size() && i < 8; i++) per.push_back(youtube::channel_videos(ids[i]));
            youtube::Results out;
            out.ok = true;
            for (size_t k = 0; k < 6; k++)
                for (auto& r : per)
                    if (k < r.items.size()) out.items.push_back(r.items[k]);
            return out;
        };
        subs_.reload();
    }

    Feed trending_, subs_, foryou_;
    bool personal_ = false;
    int foryou_version_ = -1;
    size_t subs_count_ = 0;
    std::vector<std::unique_ptr<Feed>> topic_feeds_;
    Page page_;
};

std::unique_ptr<app::Screen> make_channel(const std::string& id, const std::string& name) {
    return std::make_unique<VideoGridScreen>(name, "Latest videos", [id](const std::string& c) {
        return youtube::channel_videos(id, c);
    });
}

}  // namespace

std::unique_ptr<app::Screen> make_youtube() { return std::make_unique<YouTubeScreen>(); }

std::unique_ptr<app::Screen> make_youtube_search(const std::string& q) {
    return std::make_unique<VideoGridScreen>(q, "Search results", [q](const std::string& c) {
        return youtube::search(q, youtube::PARAMS_VIDEOS, c);
    });
}

}  // namespace screens
