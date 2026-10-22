// A kids profile's Home: the channels a grown-up allowed, and their newest videos. No search, no feed of
// suggestions, nothing that leads to a channel that wasn't chosen.
#include <algorithm>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/youtube_common.hpp"
#include "player/player.hpp"
#include "services/kids.hpp"
#include "services/podcasts.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

constexpr size_t LATEST_CHANNELS = 10;   // the newest videos come from the first ten channels
constexpr size_t LATEST_PER_CHANNEL = 3;
constexpr size_t LATEST_MAX = 30;
constexpr double LATEST_KEEP = 600;      // seconds before asking again

struct Latest {
    std::vector<std::string> key;  // the channels it was made for
    std::vector<youtube::Video> videos;
    double at = -1e9;
};
Latest g_latest;  // the main thread only

class KidsHome : public app::Screen {
public:
    KidsHome() { refresh(); }
    app::Section section() const override { return app::SEC_HOME; }
    void on_enter() override { refresh(); }

    void frame() override {
        const Theme& t = theme();
        const float x0 = content_x();
        Id g = id("kidshome");
        const store::Profile me = store::profile(store::active_profile());
        page_.begin(id(g, "page"));
        float y = page_.y(34);
        text::draw_fit(font::display, x0, y, W - x0 - 64, util::fmt(tr("Hi, %s!"), me.name.c_str()), t.text);
        y += 82;

        if (channels_.empty() && pods_.empty() && radio_.empty()) {
            empty_state(Rect(x0, y + 40, W - x0 - 64, 300), ic::LOCK, tr("No channels yet"),
                        tr("Ask a grown-up to choose some channels."));
            page_.end(y + 400 + page_.scroll());
            hint_bar({{"A", tr("Open")}});
            return;
        }

        if (!channels_.empty()) {
            ShelfSpec ch;
            ch.title = tr("Channels");
            ch.count = (int)channels_.size();
            ch.shape = CARD_CIRCLE;
            ch.item_w = 124;
            ch.first_default = true;
            ch.item = [this](int i) {
                CardInfo c;
                c.image = channels_[i].avatar;
                c.title = channels_[i].name;
                c.icon = ic::SMART_DISPLAY;
                c.image_w = 200;
                return c;
            };
            ch.on_click = [this](int i) { app::push(yt::make_channel(channels_[i].id, channels_[i].name, channels_[i].avatar)); };
            y += shelf(id(g, "channels"), x0, y, ch, &page_) + 6;

            if (g_latest.videos.empty() && !loading_) {  // nothing came: say so, and offer to try again
                text::draw(font::title, x0, y, tr("New videos"), theme().text);
                y += 56;
                Id rg = id(g, "retry");
                float bw = measure_button(tr("Try again"), ic::REFRESH);
                text::draw_fit(font::body, x0, y + 10, W - x0 - bw - 120, tr("Couldn't reach YouTube"), theme().text2);
                if (button(id(rg, "again"), Rect(W - 64 - bw, y, bw, 52), tr("Try again"), ic::REFRESH, BTN_NORMAL, rg)) {
                    g_latest.at = -1e9;
                    refresh();
                }
                y += 80;
            } else {
            ShelfSpec nv;
            nv.title = tr("New videos");
            nv.count = (int)g_latest.videos.size();
            nv.loading = loading_ && g_latest.videos.empty();
            nv.shape = CARD_WIDE;
            nv.item_w = 300;
            nv.item = [](int i) { return yt::video_card(g_latest.videos[i]); };
            nv.on_click = [](int i) { yt::play(g_latest.videos[i]); };
            nv.on_focus = [](int i) { set_backdrop(youtube::thumbnail_hq(g_latest.videos[i].id)); };
            y += shelf(id(g, "latest"), x0, y, nv, &page_) + 16;
            }

        }

        if (!pods_.empty()) {
            ShelfSpec ps;
            ps.title = tr("Podcasts");
            ps.count = (int)pods_.size();
            ps.shape = CARD_SQUARE;
            ps.first_default = channels_.empty();
            ps.item_w = 180;
            ps.item = [this](int i) {
                CardInfo c;
                c.image = pods_[i].artwork;
                c.title = pods_[i].name;
                c.icon = ic::PODCASTS;
                c.image_w = 300;
                return c;
            };
            ps.on_click = [this](int i) {
                podcasts::Show s;
                s.title = pods_[i].name;
                s.artwork = pods_[i].artwork;
                s.feed_url = pods_[i].feed_url;
                app::push(make_podcast_show(s));
            };
            y += shelf(id(g, "podcasts"), x0, y, ps, &page_) + 16;
        }
        if (!radio_.empty()) {
            ShelfSpec rs;
            rs.title = tr("Radio");
            rs.count = (int)radio_.size();
            rs.shape = CARD_SQUARE;
            rs.first_default = channels_.empty() && pods_.empty();
            rs.item_w = 180;
            rs.item = [this](int i) {
                CardInfo c;
                c.title = radio_[i].name;
                c.icon = ic::RADIO;
                return c;
            };
            rs.on_click = [this](int i) {
                player::Source src;  // a station from the list: its own source, not the open directory's
                src.url = radio_[i].url;
                src.title = radio_[i].name;
                src.subtitle = tr("Radio");
                src.live = true;
                src.service = "radio";
                src.id = radio_[i].url;
                src.remember_position = false;
                play_audio(src);
            };
            y += shelf(id(g, "radio"), x0, y, rs, &page_) + 16;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}});
    }

private:
    // The channels as they are now, and their newest videos (kept ten minutes while the list is the same).
    void refresh() {
        kids::seed_defaults_once();  // a kids profile made before there were suggestions gets them now
        pods_ = store::get_bool("kids_show_podcasts", true) ? kids::podcasts() : std::vector<kids::Podcast>{};
        radio_ = store::get_bool("kids_show_radio", true) ? kids::stations() : std::vector<kids::Station>{};
        fetch_artwork();
        channels_ = kids::channels();
        std::vector<std::string> key;
        for (const auto& c : channels_) key.push_back(c.id);
        if (channels_.empty()) {
            g_latest = {};
            return;
        }
        if (key == g_latest.key && util::now_seconds() - g_latest.at < LATEST_KEEP) return;
        if (loading_) return;
        loading_ = true;
        auto channels = channels_;
        scope_.run<Latest>(
            [channels, key] {
                Latest out;
                out.key = key;
                std::vector<std::vector<youtube::Video>> per;
                for (size_t i = 0; i < channels.size() && i < LATEST_CHANNELS; i++) {
                    youtube::Channel header;
                    youtube::Results r = youtube::channel_tab(channels[i].id, youtube::Tab::VIDEOS, "", &header);
                    // A channel's picture, for the circle, the first time it's seen.
                    if (channels[i].avatar.empty() && !header.avatar.empty())
                        kids::add({channels[i].id, channels[i].name, header.avatar});
                    std::vector<youtube::Video> list;
                    for (youtube::Video& v : r.items) {
                        if (v.live || v.id.empty()) continue;
                        v.channel = channels[i].name;
                        v.channel_id = channels[i].id;
                        list.push_back(std::move(v));
                        if (list.size() == LATEST_PER_CHANNEL) break;
                    }
                    per.push_back(std::move(list));
                }
                // One from each channel in turn, so the row isn't one channel's.
                for (size_t round = 0; round < LATEST_PER_CHANNEL && out.videos.size() < LATEST_MAX; round++)
                    for (auto& list : per)
                        if (round < list.size() && out.videos.size() < LATEST_MAX) out.videos.push_back(list[round]);
                out.at = util::now_seconds();
                return out;
            },
            [this](Latest l) {
                loading_ = false;
                g_latest = std::move(l);
                channels_ = kids::channels();  // with the pictures just found
            });
    }

    // A show's cover, the first time it's seen: a few at a time, quietly.
    void fetch_artwork() {
        std::vector<kids::Podcast> missing;
        for (const auto& p : pods_)
            if (p.artwork.empty() && missing.size() < 4) missing.push_back(p);
        if (missing.empty() || art_loading_) return;
        art_loading_ = true;
        art_scope_.run<int>(
            [missing] {
                int found = 0;
                for (const auto& p : missing) {
                    podcasts::Feed f = podcasts::load(p.feed_url);
                    if (f.ok && !f.show.artwork.empty()) {
                        kids::add_podcast({p.feed_url, p.name, f.show.artwork});
                        found++;
                    }
                }
                return found;
            },
            [this](int) {
                art_loading_ = false;
                if (store::get_bool("kids_show_podcasts", true)) pods_ = kids::podcasts();
            });
    }

    std::vector<kids::Channel> channels_;
    std::vector<kids::Podcast> pods_;
    std::vector<kids::Station> radio_;
    bool loading_ = false, art_loading_ = false;
    tasks::Scope scope_, art_scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_kids_home() { return std::make_unique<KidsHome>(); }

}  // namespace screens
