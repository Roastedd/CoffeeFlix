// YouTube: home shelves, recommendations (on-device, or the account's when signed in), topics,
// subscriptions, Discover, search.
#include <algorithm>
#include <set>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/youtube_common.hpp"
#include "services/youtube.hpp"
#include "services/yt_account.hpp"
#include "services/yt_recs.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using yt::Feed;

// --- results grid (search / topic) ---------------------------------------------------------

class VideoGridScreen : public app::Screen {
public:
    // With a `channel_query`, matching channels are shown above the videos (search results).
    VideoGridScreen(std::string title, std::string subtitle, std::function<youtube::Results(const std::string&)> fetch,
                    std::string channel_query = "")
        : title_(std::move(title)), subtitle_(std::move(subtitle)) {
        feed_.fetch = std::move(fetch);
        feed_.load();
        if (!channel_query.empty()) {
            channels_loading_ = true;
            scope_.run<youtube::ChannelResults>([channel_query] { return youtube::search_channels(channel_query); },
                                                [this](youtube::ChannelResults r) {
                channels_ = std::move(r.items);
                if (channels_.size() > 12) channels_.resize(12);
                channels_loading_ = false;
            });
        }
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

        if (!channels_.empty()) {
            ShelfSpec s;
            s.title = tr("Channels");
            s.count = (int)channels_.size();
            s.shape = CARD_CIRCLE;
            s.item_w = 130;
            s.item = [this](int i) {
                CardInfo c;
                c.title = channels_[i].name;
                c.subtitle = channels_[i].subscribers;
                c.image = channels_[i].avatar;
                c.icon = ic::PERSON;
                return c;
            };
            s.on_click = [this](int i) { app::push(yt::make_channel(channels_[i].id, channels_[i].name, channels_[i].avatar)); };
            s.on_x = [this](int i) {
                youtube::Channel c = channels_[i];
                bool sub = yt::subscribed(c.id);
                show_menu(c.name, c.subscribers, {
                    {tr("Open channel"), ic::ACCOUNT_CIRCLE, [c] { app::push(yt::make_channel(c.id, c.name, c.avatar)); }},
                    {sub ? tr("Unsubscribe") : tr("Subscribe"), sub ? ic::REMOVE : ic::PERSON_ADD,
                     [c, sub] { yt::set_subscribed(c.id, c.name, c.avatar, !sub); }},
                });
            };
            y += shelf(id(g, "channels"), x0, y, s, &page_) + 12;
            text::draw(font::title, x0, y, tr("Videos"), t.text);
            y += 52;
        }

        if (feed_.loaded && feed_.res.items.empty()) {
            if (feed_.res.error.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 280), ic::SEARCH, tr("No videos found"), tr("Try a different search."));
            } else if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF,
                                          tr("Couldn't reach YouTube"), feed_.res.error.c_str())) {
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
            gs.item = [this](int i) { return yt::video_card(feed_.res.items[i]); };
            gs.on_click = [this](int i) { yt::play(feed_.res.items[i]); };
            gs.on_focus = [this](int i) { set_backdrop(youtube::thumbnail_hq(feed_.res.items[i].id)); };
            gs.on_x = [this](int i) {
                yt::MenuOptions o;
                std::string vid = feed_.res.items[i].id;
                o.removed = [this, vid] { feed_.remove(vid); };
                yt::video_menu(feed_.res.items[i], o);
            };
            gs.on_reach_end = [this] { feed_.more(); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"X", tr("More")}, {"B", tr("Back")}});
    }

private:
    std::string title_, subtitle_;
    Feed feed_;
    std::vector<youtube::Channel> channels_;
    bool channels_loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

std::unique_ptr<app::Screen> search_results(const std::string& q) {
    return std::make_unique<VideoGridScreen>(q, tr("Search results"), [q](const std::string& c) {
        return youtube::search(q, youtube::PARAMS_VIDEOS, c);
    }, q);
}

// A shelf of a feed's videos. Nothing when it loaded empty, a line when it failed.
float video_shelf(Id sid, float x, float y, const std::string& title, Feed& f, Page& page) {
    if (f.loaded && f.res.items.empty()) {
        if (!f.res.error.empty()) {
            text::draw(font::title, x, y, title, theme().text);
            text::draw(font::body, x, y + 46, util::fmt(tr("Couldn't load: %s"), f.res.error.c_str()), theme().text3);
            return 90;
        }
        return 0;
    }
    ShelfSpec s;
    s.title_str = title;
    s.count = (int)f.res.items.size();
    s.loading = f.loading || !f.loaded;
    s.item_w = 300;
    s.item = [&f](int i) { return yt::video_card(f.res.items[i]); };
    s.on_click = [&f](int i) { yt::play(f.res.items[i]); };
    s.on_focus = [&f](int i) {
        set_backdrop(youtube::thumbnail_hq(f.res.items[i].id));
        if (i >= (int)f.res.items.size() - 4) f.more();
    };
    s.on_x = [&f](int i) {
        yt::MenuOptions o;
        std::string vid = f.res.items[i].id;
        o.removed = [&f, vid] { f.remove(vid); };
        yt::video_menu(f.res.items[i], o);
    };
    s.on_y = [&f](int i) {
        yt_recs::not_interested(f.res.items[i]);
        f.res.items.erase(f.res.items.begin() + i);
        toast(tr("Got it, you'll see less like this"), ic::CHECK_CIRCLE);
    };
    return shelf(sid, x, y, s, &page);
}

void prompt_search() {
    prompt_text(tr("Search YouTube"), "", tr("Search YouTube"), [](std::string q) {
        if (q.empty()) return;
        store::add_recent_search("youtube", q);
        yt_recs::on_search(q);
        app::push(search_results(q));
    });
}

// --- discover -------------------------------------------------------------------------------
// Shelves picked from what was watched and searched on this console: videos like the ones you
// enjoyed, more from the channels you watch most, your searches and your interests.

class DiscoverScreen : public app::Screen {
public:
    DiscoverScreen() {
        yt_recs::Profile p = yt_recs::profile();
        interests_ = p.interests;
        // Interleaved, so the first few shelves aren't all the same kind.
        std::vector<std::vector<std::unique_ptr<Row>>> kinds(4);
        for (auto& v : p.watched) {
            std::string id = v.id;
            kinds[0].push_back(row(util::fmt(tr("Because you watched \xE2\x80\x9C%s\xE2\x80\x9D"), v.title.c_str()),
                                   [id](const std::string& c) { return c.empty() ? youtube::related(id) : youtube::Results(); }));
        }
        for (auto& ch : p.channels) {
            std::string id = ch.id;
            kinds[1].push_back(row(util::fmt(tr("More from %s"), ch.name.c_str()),
                                   [id](const std::string& c) { return youtube::channel_videos(id, c); }));
        }
        for (size_t i = 0; i < p.searches.size() && i < 2; i++) {
            std::string q = p.searches[i];
            kinds[2].push_back(row(util::fmt(tr("Because you searched \xE2\x80\x9C%s\xE2\x80\x9D"), q.c_str()),
                                   [q](const std::string& c) { return youtube::search(q, youtube::PARAMS_VIDEOS, c); }));
        }
        for (size_t i = 0; i < p.interests.size() && kinds[3].size() < 2; i++) {
            std::string q = p.interests[i];
            bool searched = std::any_of(p.searches.begin(), p.searches.end(),
                                        [&](const std::string& s) { return util::lower(s) == q; });
            if (searched) continue;
            kinds[3].push_back(row(util::fmt(tr("Top %s videos this week"), label(q).c_str()), [q](const std::string& c) {
                return youtube::search(q, youtube::PARAMS_POPULAR_WEEK, c);
            }));
        }
        for (size_t i = 0; i < 3; i++)
            for (auto& k : kinds)
                if (i < k.size()) rows_.push_back(std::move(k[i]));
        version_ = yt_recs::version();
    }

    app::Section section() const override { return app::SEC_YOUTUBE; }

    void on_enter() override {
        // Back from a video: what was just watched (or turned down) leaves the shelves; they
        // aren't rebuilt, so nothing moves under you.
        if (yt_recs::version() == version_) return;
        version_ = yt_recs::version();
        for (auto& r : rows_) {
            auto& v = r->feed.res.items;
            v.erase(std::remove_if(v.begin(), v.end(), [](const youtube::Video& x) { return yt_recs::hidden(x); }), v.end());
        }
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ytdiscover");
        page_.begin(id(g, "page"));
        float y = page_.y(56);
        text::draw(font::headline, x0, y, tr("Discover"), t.text);
        text::draw(font::body, x0, y + 46, tr("Picked from what you watch and search on this console"), t.text2);
        y += 100;

        if (rows_.empty() && interests_.empty()) {
            if (empty_state_action(id(g, "empty"), Rect(x0, y, W - x0 - 60, 300), ic::EXPLORE, tr("Nothing to discover yet"),
                                   tr("Watch a few videos or search for something, and this fills up with picks based on them."),
                                   tr("Search YouTube"), ic::SEARCH))
                prompt_search();
            page_.end(y + 340 + page_.scroll());
            hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
            return;
        }

        // Interests: each one opens a search.
        Id chips = id(g, "chips");
        if (!interests_.empty()) {
            float cx = x0;
            for (size_t i = 0; i < interests_.size(); i++) {
                std::string name = label(interests_[i]);
                float w = text::measure(font::small_bold, name) + 58;
                if (cx + w > W - 60) break;
                if (chip(id(chips, (int64_t)i), Rect(cx, y, w, 42), name.c_str(), false, chips, ic::SEARCH))
                    app::push(search_results(interests_[i]));
                cx += w + 12;
            }
            y += 70;
            if (focus_in_group(chips)) page_.focus_range(0, y + page_.scroll());
        }

        for (size_t i = 0; i < rows_.size(); i++) {
            Row& r = *rows_[i];
            if (y < H + 300) r.feed.load();  // lazy: only when about to scroll into view
            y += video_shelf(id(g, (int64_t)i), x0, y, text::ellipsize(font::title, r.title, W - x0 - 60), r.feed, page_);
            if (r.feed.loaded && !r.feed.res.items.empty()) y += 12;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"X", tr("More")}, {"Y", tr("Not interested")}, {"B", tr("Back")}});
    }

private:
    struct Row {
        std::string title;
        Feed feed;
    };

    // Without what was watched already or turned down, and without repeats (a stream's replays
    // often come up several times under the same title).
    static std::unique_ptr<Row> row(std::string title, std::function<youtube::Results(const std::string&)> fetch) {
        auto r = std::make_unique<Row>();
        r->title = std::move(title);
        r->feed.fetch = [fetch](const std::string& c) {
            youtube::Results res = fetch(c);
            std::set<std::string> titles;
            res.items.erase(std::remove_if(res.items.begin(), res.items.end(), [&](const youtube::Video& v) {
                return !titles.insert(v.title).second || yt_recs::hidden(v);
            }), res.items.end());
            return res;
        };
        return r;
    }

    // Interests are lowercase words from titles: "minecraft" shows as "Minecraft".
    static std::string label(std::string word) {
        if (!word.empty() && word[0] >= 'a' && word[0] <= 'z') word[0] = (char)(word[0] - 'a' + 'A');
        return word;
    }

    std::vector<std::string> interests_;
    std::vector<std::unique_ptr<Row>> rows_;
    int version_ = -1;
    Page page_;
};

// --- home -------------------------------------------------------------------------------------

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
        foryou_.fetch = [](const std::string& c) { return yt::for_you(c); };
        load_for_you();
        later_ = yt::watch_later();
    }

    app::Section section() const override { return app::SEC_YOUTUBE; }

    void on_enter() override {
        refresh_library();
        // Rebuild once something new was learned (a watch, search, subscription...) or on signing in or out.
        if ((yt_recs::version() != foryou_version_ || yt_account::version() != account_version_) && !foryou_.loading)
            load_for_you();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("youtube");
        // Menu actions can change subscriptions and Watch later (For you waits for the next visit,
        // unless the account menu signed out).
        bool menu = menu_active();
        if (menu_open_ && !menu) {
            refresh_library();
            if (yt_account::version() != account_version_) load_for_you();
        }
        menu_open_ = menu;
        page_.begin(id(g, "page"));

        float y = page_.y(52);
        text::draw(font::display, x0, y, "YouTube", t.text);
        Id top = id(g, "top");
        float bx = W - 60 - 28;  // library buttons at the right of the search bar
        if (icon_button(id(top, "library"), bx, y + 32, 28, ic::VIDEO_LIBRARY, top)) app::push(yt::make_library());
        if (icon_button(id(top, "subs"), bx - 70, y + 32, 28, ic::SUBSCRIPTIONS, top)) app::push(yt::make_subscriptions());
        if (icon_button(id(top, "account"), bx - 140, y + 32, 28, ic::ACCOUNT_CIRCLE, top, 0, yt_account::signed_in()))
            yt::account_menu();
        if (search_bar(id(top, "search"), Rect(x0 + 300, y + 4, bx - 140 - 28 - 24 - x0 - 300, 56), "", tr("Search YouTube"),
                       top, F_DEFAULT))
            prompt_search();
        y += 84;

        // Discover, then the topics. The row slides sideways when it doesn't fit.
        auto& tps = youtube::topics();
        std::vector<float> cw{text::measure(font::small_bold, tr("Discover")) + 58};
        for (auto& tp : tps) cw.push_back(text::measure(font::small_bold, tr(tp.name)) + 58);
        Id discover = id(top, "discover");
        float row_w = 0, fx0 = 0, fx1 = 0;
        bool chip_focused = false;
        for (size_t i = 0; i < cw.size(); i++) {
            if (focused() == (i == 0 ? discover : id(top, (int64_t)(i - 1)))) {
                fx0 = row_w;
                fx1 = row_w + cw[i];
                chip_focused = true;
            }
            row_w += cw[i] + 12;
        }
        float view_w = W - x0 - 30;
        Id sid = id(top, "chips");
        scroll_drag(sid, Rect(x0, y - 6, view_w, 54), false, row_w, view_w);
        float cx = x0 - scroll_follow(sid, fx0, fx1, view_w, row_w, 80, chip_focused);
        gfx::push_clip(Rect(x0 - 12, y - 12, W - x0 + 12, 66));  // not under the rail
        if (chip(discover, Rect(cx, y, cw[0], 42), tr("Discover"), true, top, ic::EXPLORE))
            app::push(std::make_unique<DiscoverScreen>());
        cx += cw[0] + 12;
        for (size_t i = 0; i < tps.size(); i++) {
            const char* name = tr(tps[i].name);
            float w = cw[i + 1];
            if (chip(id(top, (int64_t)i), Rect(cx, y, w, 42), name, false, top, tps[i].icon)) {
                std::string q = tps[i].query;
                app::push(std::make_unique<VideoGridScreen>(name, tr("Popular this week"), [q](const std::string& c) {
                    return youtube::search(q, youtube::PARAMS_POPULAR_WEEK, c);
                }));
            }
            cx += w + 12;
        }
        gfx::pop_clip();
        y += 70;
        if (focus_in_group(top)) page_.focus_range(0, y + page_.scroll());

        // Continue watching (YouTube only)
        std::vector<store::Resume> resume;
        for (auto& r : store::resume_list(30))
            if (r.service == "youtube") resume.push_back(r);
        if (!resume.empty()) {
            ShelfSpec s;
            s.title = tr("Continue watching");
            s.count = (int)resume.size();
            s.item_w = 300;
            s.item = [resume](int i) {
                CardInfo c;
                c.image = youtube::thumbnail(resume[i].id);
                c.title = resume[i].title;
                c.subtitle = resume[i].subtitle;
                c.progress = resume[i].duration > 0 ? (float)(resume[i].position / resume[i].duration) : 0;
                c.badge = util::fmt(tr("%s left"), util::format_duration(resume[i].duration - resume[i].position).c_str());
                return c;
            };
            s.on_click = [resume](int i) { yt::play(resume_video(resume[i])); };
            s.on_focus = [resume](int i) { set_backdrop(youtube::thumbnail_hq(resume[i].id)); };
            s.on_x = [resume](int i) {
                youtube::Video v = resume_video(resume[i]);
                yt::MenuOptions o;
                o.extra.push_back({tr("Remove from Continue watching"), ic::REMOVE,
                                   [v] { store::resume_remove("youtube", v.id); }});
                yt::video_menu(v, o);
            };
            y += shelf(id(g, "resume"), x0, y, s, &page_) + 12;
        }

        if (trending_.loaded && trending_.res.items.empty() && !trending_.res.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, tr("Couldn't reach YouTube"),
                                   trending_.res.error.c_str())) {
                trending_.reload();
                for (auto& f : topic_feeds_) f->reload();
            }
            page_.end(y + 330 + page_.scroll());
            hint_bar({{"A", tr("Select")}});
            return;
        }
        if (personal_) y += video_shelf(id(g, "foryou"), x0, y, tr("For you"), foryou_, page_) + 12;

        if (!subs_.res.items.empty() || subs_.loading) {
            y += video_shelf(id(g, "subs"), x0, y, tr("From your subscriptions"), subs_, page_) + 12;
            ShelfSpec s;
            s.title = tr("Channels");
            s.count = (int)chans_.size();
            s.shape = CARD_CIRCLE;
            s.item_w = 130;
            s.item = [this](int i) {
                CardInfo c;
                c.title = chans_[i].name;
                c.image = chans_[i].avatar;
                c.icon = ic::PERSON;
                return c;
            };
            s.on_click = [this](int i) { app::push(yt::make_channel(chans_[i].id, chans_[i].name, chans_[i].avatar)); };
            y += shelf(id(g, "chans"), x0, y, s, &page_) + 12;
        }

        if (!later_.empty()) {
            ShelfSpec s;
            s.title = tr("Watch later");
            s.count = (int)later_.size();
            s.item_w = 300;
            s.item = [this](int i) { return yt::video_card(later_[i]); };
            s.on_click = [this](int i) { yt::play(later_[i]); };
            s.on_focus = [this](int i) { set_backdrop(youtube::thumbnail_hq(later_[i].id)); };
            s.on_x = [this](int i) { yt::video_menu(later_[i]); };
            y += shelf(id(g, "later"), x0, y, s, &page_) + 12;
        }

        y += video_shelf(id(g, "trending"), x0, y, tr("Popular this week"), trending_, page_) + 12;

        for (size_t i = 0; i < topic_feeds_.size(); i++) {
            Feed& f = *topic_feeds_[i];
            if (y < H + 300) f.load();  // lazy: only when about to scroll into view
            y += video_shelf(id(g, (int64_t)(100 + i)), x0, y, tr(tps[i].name), f, page_) + 12;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"X", tr("More")}, {"Y", tr("Not interested")}});
    }

private:
    static youtube::Video resume_video(const store::Resume& r) {
        youtube::Video v;
        v.id = r.id;
        v.title = r.title;
        v.channel = r.subtitle;
        return v;
    }

    void refresh_library() {
        if (subs_key_ != subs_key()) load_subscriptions();
        later_ = yt::watch_later();
    }

    void load_for_you() {
        personal_ = yt::has_for_you();
        foryou_version_ = yt_recs::version();
        account_version_ = yt_account::version();
        if (personal_) foryou_.reload();
    }

    static std::string subs_key() {
        std::string k = std::to_string(yt_account::version());
        for (const youtube::Channel& c : yt::subscriptions()) k += c.id;
        return k;
    }

    void load_subscriptions() {
        chans_ = yt::subscriptions();
        subs_key_ = subs_key();
        chans_scope_.reset();
        if (yt_account::signed_in()) {
            // What was stored shows at once; the list is loaded again every few minutes.
            if (!yt::account_channels_fresh())
                chans_scope_.run<youtube::ChannelResults>([] { return yt::load_account_channels(); },
                                                          [this](youtube::ChannelResults r) {
                    if (!r.ok) return;
                    chans_ = std::move(r.items);
                    subs_key_ = subs_key();
                });
            subs_.fetch = [](const std::string& c) { return youtube::account_subscriptions(c); };
            subs_.reload();
            return;
        }
        if (chans_.empty()) {
            subs_.scope.reset();
            subs_.res = youtube::Results();
            subs_.loaded = true;
            return;
        }
        std::vector<std::string> ids;
        for (size_t i = 0; i < chans_.size() && i < 20; i++) ids.push_back(chans_[i].id);
        subs_.fetch = [ids](const std::string& c) {
            if (!c.empty()) return youtube::Results();
            std::vector<youtube::Channel> headers;
            youtube::Results r = youtube::subscription_feed(ids, 4, &headers);
            yt::update_channels(headers);  // avatars for channels subscribed from a video
            if (r.items.size() > 40) r.items.resize(40);
            return r;
        };
        subs_.reload();
    }

    Feed trending_, subs_, foryou_;
    bool personal_ = false, menu_open_ = false;
    int foryou_version_ = -1, account_version_ = -1;
    tasks::Scope chans_scope_;
    std::string subs_key_;
    std::vector<youtube::Channel> chans_;
    std::vector<youtube::Video> later_;
    std::vector<std::unique_ptr<Feed>> topic_feeds_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_youtube() { return std::make_unique<YouTubeScreen>(); }

std::unique_ptr<app::Screen> make_youtube_search(const std::string& q) { return search_results(q); }

}  // namespace screens
