// Search everything at once: YouTube, Jellyfin, Twitch, radio and podcasts.
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/jellyfin.hpp"
#include "services/podcasts.hpp"
#include "services/radio.hpp"
#include "services/twitch.hpp"
#include "services/youtube.hpp"
#include "services/yt_recs.hpp"
#include "screens/youtube_common.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

class SearchScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_SEARCH; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("search");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Search", t.text);
        y += 84;
        Id top = id(g, "top");
        if (search_bar(id(top, "bar"), Rect(x0, y, W - x0 - 60, 64), query_, "Movies, videos, channels, stations, podcasts\xE2\x80\xA6",
                       top, F_DEFAULT))
            open_keyboard();
        y += 90;

        if (query_.empty()) {
            auto recent = store::recent_searches("global");
            if (!recent.empty()) {
                text::draw(font::title, x0, y, "Recent searches", t.text);
                y += 50;
                float cx = x0;
                Id rg = id(g, "recent");
                for (size_t i = 0; i < recent.size() && i < 10; i++) {
                    float w = text::measure(font::small_bold, recent[i]) + 58;
                    if (cx + w > W - 60) {
                        cx = x0;
                        y += 54;
                    }
                    if (chip(id(rg, (int64_t)i), Rect(cx, y, w, 42), recent[i].c_str(), false, rg, ic::HISTORY)) run(recent[i]);
                    cx += w + 12;
                }
                y += 70;
            } else {
                empty_state(Rect(x0, y, W - x0 - 60, 260), ic::SEARCH, "Find anything",
                            "One search looks through YouTube, your Jellyfin library, Twitch, radio stations and podcasts.");
                y += 300;
            }
            page_.end(y + page_.scroll());
            return;
        }

        bool any = false, loading = false;
        if (!jf_.empty() || jf_loading_) {
            any = true;
            ShelfSpec s;
            s.title = "In your Jellyfin library";
            s.count = (int)jf_.size();
            s.loading = jf_loading_;
            s.shape = CARD_POSTER;
            s.item_w = 150;
            s.item = [this](int i) {
                CardInfo c;
                c.image = jellyfin::poster(jf_[i], 300);
                c.title = jf_[i].name;
                c.subtitle = jf_[i].type == "Episode" ? jf_[i].series_name : jf_[i].year;
                c.icon = ic::VIDEO_LIBRARY;
                return c;
            };
            s.on_click = [this](int i) {
                const jellyfin::Item& it = jf_[i];
                if (it.type == "Episode") play_video(jellyfin::make_source(it));
                else if (it.is_audio()) play_audio(jellyfin::make_source(it));
                else app::push(make_jellyfin_item(it));
            };
            y += shelf(id(g, "jf"), x0, y, s, &page_) + 10;
        }
        if (!yt_.empty() || yt_loading_) {
            any = true;
            ShelfSpec s;
            s.title = "YouTube";
            s.count = (int)yt_.size();
            s.loading = yt_loading_;
            s.item_w = 300;
            s.item = [this](int i) {
                CardInfo c;
                c.image = youtube::thumbnail(yt_[i].id);
                c.title = yt_[i].title;
                c.subtitle = yt_[i].channel;
                c.badge = yt_[i].duration;
                c.live = yt_[i].live;
                c.icon = ic::SMART_DISPLAY;
                return c;
            };
            s.on_click = [this](int i) { yt::play(yt_[i]); };
            s.on_focus = [this](int i) { set_backdrop(youtube::thumbnail_hq(yt_[i].id)); };
            s.on_x = [this](int i) { yt::video_menu(yt_[i]); };
            y += shelf(id(g, "yt"), x0, y, s, &page_) + 10;
        }
        if (!ytc_.empty()) {
            any = true;
            ShelfSpec s;
            s.title = "YouTube channels";
            s.count = (int)ytc_.size();
            s.shape = CARD_CIRCLE;
            s.item_w = 130;
            s.item = [this](int i) {
                CardInfo c;
                c.image = ytc_[i].avatar;
                c.title = ytc_[i].name;
                c.subtitle = ytc_[i].subscribers;
                c.icon = ic::PERSON;
                return c;
            };
            s.on_click = [this](int i) { app::push(yt::make_channel(ytc_[i].id, ytc_[i].name, ytc_[i].avatar)); };
            y += shelf(id(g, "ytc"), x0, y, s, &page_) + 10;
        }
        if (!tw_.empty() || tw_loading_) {
            any = true;
            ShelfSpec s;
            s.title = "Twitch channels";
            s.count = (int)tw_.size();
            s.loading = tw_loading_;
            s.shape = CARD_CIRCLE;
            s.item_w = 130;
            s.item = [this](int i) {
                CardInfo c;
                c.image = tw_[i].avatar;
                c.title = tw_[i].name;
                c.live = tw_[i].live;
                c.icon = ic::LIVE_TV;
                return c;
            };
            s.on_click = [this](int i) {
                if (tw_[i].live) play_video(twitch::make_source(tw_[i]));
                else toast(tw_[i].name + " is offline", ic::INFO);
            };
            y += shelf(id(g, "tw"), x0, y, s, &page_) + 10;
        }
        if (!radio_.empty() || radio_loading_) {
            any = true;
            ShelfSpec s;
            s.title = "Radio stations";
            s.count = (int)radio_.size();
            s.loading = radio_loading_;
            s.shape = CARD_SQUARE;
            s.item_w = 160;
            s.item = [this](int i) {
                CardInfo c;
                c.image = radio_[i].favicon;
                c.title = radio_[i].name;
                c.subtitle = radio_[i].country;
                c.icon = ic::RADIO;
                return c;
            };
            s.on_click = [this](int i) { play_audio(radio::make_source(radio_[i])); };
            y += shelf(id(g, "radio"), x0, y, s, &page_) + 10;
        }
        if (!pods_.empty() || pods_loading_) {
            any = true;
            ShelfSpec s;
            s.title = "Podcasts";
            s.count = (int)pods_.size();
            s.loading = pods_loading_;
            s.shape = CARD_SQUARE;
            s.item_w = 160;
            s.item = [this](int i) {
                CardInfo c;
                c.image = pods_[i].artwork;
                c.title = pods_[i].title;
                c.subtitle = pods_[i].author;
                c.icon = ic::PODCASTS;
                return c;
            };
            s.on_click = [this](int i) {
                const podcasts::Show& p = pods_[i];
                app::push(make_podcast_show(p.feed_url, p.title, p.author, p.artwork));
            };
            y += shelf(id(g, "pods"), x0, y, s, &page_) + 10;
        }
        loading = jf_loading_ || yt_loading_ || tw_loading_ || radio_loading_ || pods_loading_;
        if (!any && !loading) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::SEARCH, "No results",
                        errors_.empty() ? "Try different words." : errors_.c_str());
            y += 300;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Open"}, {"B", "Back"}});
    }

private:
    void open_keyboard() {
        prompt_text("Search", query_, "Search everything", [this](std::string q) {
            if (!q.empty()) run(q);
        });
    }

    void run(const std::string& q) {
        query_ = q;
        store::add_recent_search("global", q);
        yt_recs::on_search(q);
        scope_.reset();
        errors_.clear();
        jf_.clear();
        yt_.clear();
        ytc_.clear();
        tw_.clear();
        radio_.clear();
        pods_.clear();
        jf_loading_ = jellyfin::account().valid();
        yt_loading_ = tw_loading_ = radio_loading_ = pods_loading_ = true;
        if (jf_loading_)
            scope_.run<jellyfin::List>([q] { return jellyfin::search(q); }, [this](jellyfin::List l) {
                jf_ = std::move(l.items);
                jf_loading_ = false;
            });
        scope_.run<youtube::Results>([q] { return youtube::search(q); }, [this](youtube::Results r) {
            yt_ = std::move(r.items);
            if (!r.error.empty()) errors_ = r.error;
            yt_loading_ = false;
        });
        scope_.run<youtube::ChannelResults>([q] { return youtube::search_channels(q); }, [this](youtube::ChannelResults r) {
            ytc_ = std::move(r.items);
            if (ytc_.size() > 8) ytc_.resize(8);
        });
        scope_.run<twitch::Streams>([q] { return twitch::search(q); }, [this](twitch::Streams s) {
            tw_ = std::move(s.items);
            tw_loading_ = false;
        });
        scope_.run<radio::List>([q] { return radio::search(q, 30); }, [this](radio::List l) {
            radio_ = std::move(l.items);
            radio_loading_ = false;
        });
        scope_.run<podcasts::Shows>([q] { return podcasts::search(q); }, [this](podcasts::Shows s) {
            pods_ = std::move(s.items);
            pods_loading_ = false;
        });
    }

    std::string query_, errors_;
    std::vector<jellyfin::Item> jf_;
    std::vector<youtube::Video> yt_;
    std::vector<youtube::Channel> ytc_;
    std::vector<twitch::Stream> tw_;
    std::vector<radio::Station> radio_;
    std::vector<podcasts::Show> pods_;
    bool jf_loading_ = false, yt_loading_ = false, tw_loading_ = false, radio_loading_ = false, pods_loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_search() { return std::make_unique<SearchScreen>(); }

}  // namespace screens
