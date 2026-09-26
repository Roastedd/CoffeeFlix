// YouTube: home shelves, recommendations (on-device, or the account's when signed in), topics,
// subscriptions, search.
#include <algorithm>

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
            open_search();
        y += 84;

        // Topic chips
        float cx = x0;
        auto& tps = youtube::topics();
        for (size_t i = 0; i < tps.size(); i++) {
            const char* name = tr(tps[i].name);
            float w = text::measure(font::small_bold, name) + 58;
            if (chip(id(top, (int64_t)i), Rect(cx, y, w, 42), name, false, top, tps[i].icon)) {
                std::string q = tps[i].query;
                app::push(std::make_unique<VideoGridScreen>(name, tr("Popular this week"), [q](const std::string& c) {
                    return youtube::search(q, youtube::PARAMS_POPULAR_WEEK, c);
                }));
            }
            cx += w + 12;
        }
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
        if (personal_) y += video_shelf(id(g, "foryou"), x0, y, tr("For you"), foryou_) + 12;

        if (!subs_.res.items.empty() || subs_.loading) {
            y += video_shelf(id(g, "subs"), x0, y, tr("From your subscriptions"), subs_) + 12;
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

        y += video_shelf(id(g, "trending"), x0, y, tr("Popular this week"), trending_) + 12;

        for (size_t i = 0; i < topic_feeds_.size(); i++) {
            Feed& f = *topic_feeds_[i];
            if (y < H + 300) f.load();  // lazy: only when about to scroll into view
            y += video_shelf(id(g, (int64_t)(100 + i)), x0, y, tr(tps[i].name), f) + 12;
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

    float video_shelf(Id sid, float x, float y, const char* title, Feed& f) {
        if (f.loaded && f.res.items.empty()) {
            if (!f.res.error.empty()) {
                text::draw(font::title, x, y, title, theme().text);
                text::draw(font::body, x, y + 46, util::fmt(tr("Couldn't load: %s"), f.res.error.c_str()), theme().text3);
                return 90;
            }
            return 0;
        }
        ShelfSpec s;
        s.title = title;
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
        return shelf(sid, x, y, s, &page_);
    }

    void open_search() {
        prompt_text(tr("Search YouTube"), "", tr("Search YouTube"), [](std::string q) {
            if (q.empty()) return;
            store::add_recent_search("youtube", q);
            yt_recs::on_search(q);
            app::push(search_results(q));
        });
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
