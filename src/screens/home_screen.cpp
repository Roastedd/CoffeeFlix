// Home: a hero for whatever is focused, and shelves pulled from every service.
#include <ctime>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
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

struct HomeItem {
    int icon = ic::MOVIE;
    std::string service, title, subtitle, description, image, hero;
    float progress = -1;
    bool live = false;
    std::string badge;
    std::function<void()> open;
    std::function<void()> more;  // X
};

struct Row {
    std::string title;  // English, also the shelf's id: tr() it where it's shown
    CardShape shape = CARD_WIDE;
    float w = 300;
    std::vector<HomeItem> items;
    bool loading = false;
};

const char* greeting() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_hour < 5) return tr("Up late?");
    if (lt.tm_hour < 12) return tr("Good morning");
    if (lt.tm_hour < 18) return tr("Good afternoon");
    return tr("Good evening");
}

HomeItem from_resume(const store::Resume& r) {
    HomeItem h;
    h.title = r.title;
    h.subtitle = r.subtitle;
    h.progress = r.duration > 0 ? (float)(r.position / r.duration) : 0;
    h.badge = r.duration > 0 ? util::fmt(tr("%s left"), util::format_duration(r.duration - r.position).c_str()) : "";
    if (r.service == "youtube") {
        h.icon = ic::SMART_DISPLAY;
        h.service = "YouTube";
        h.image = youtube::thumbnail(r.id);
        h.hero = youtube::thumbnail_hq(r.id);
        youtube::Video v;
        v.id = r.id;
        v.title = r.title;
        v.channel = r.subtitle;
        h.open = [v] { yt::play(v); };
        h.more = [v] { yt::video_menu(v); };
    } else if (r.service == "podcast") {
        h.icon = ic::PODCASTS;
        h.service = tr("Podcast");
        h.image = h.hero = r.image;
        h.open = [r] { play_audio(podcasts::source_from_resume(r.id, r.title, r.subtitle, r.image, r.extra)); };
    } else {
        h.icon = r.video ? ic::MOVIE : ic::MUSIC;
        h.service = r.service == "smb" ? tr("Network") : r.service == "dlna" ? tr("Media server") : tr("My Media");
        h.image = h.hero = r.image;
        if (h.image.empty() && r.video && r.service == "local") h.image = h.hero = "thumb://" + r.id;
        h.open = [r] {
            player::Source s;
            s.url = r.id;
            s.title = r.title;
            s.subtitle = r.subtitle;
            s.service = r.service;
            s.id = r.id;
            s.start = r.position;
            if (r.video) play_video(s);
            else play_audio(s);
        };
    }
    return h;
}

HomeItem from_jellyfin(const jellyfin::Item& it) {
    HomeItem h;
    h.icon = ic::VIDEO_LIBRARY;
    h.service = "Jellyfin";
    h.title = it.type == "Episode" ? it.series_name : it.name;
    h.subtitle = it.type == "Episode" ? util::fmt(tr("S%d E%d \xC2\xB7 %s"), it.parent_index, it.index, it.name.c_str()) : it.year;
    h.description = it.overview;
    h.image = jellyfin::thumb(it, 480);
    h.hero = jellyfin::backdrop(it, 1280);
    if (it.position > 0 && it.runtime > 0) {
        h.progress = (float)(it.position / it.runtime);
        h.badge = util::fmt(tr("%s left"), util::format_duration(it.runtime - it.position).c_str());
    }
    jellyfin::Item copy = it;
    h.open = [copy] { play_video(jellyfin::make_source(copy)); };
    return h;
}

HomeItem from_twitch(const twitch::Stream& s) {
    HomeItem h;
    h.icon = ic::LIVE_TV;
    h.service = "Twitch";
    h.title = s.name;
    h.subtitle = s.game;
    h.description = s.title;
    h.image = h.hero = s.preview;
    h.live = true;
    h.badge = util::fmt(tr("%s watching"), util::format_count(s.viewers).c_str());
    h.open = [s] { play_video(twitch::make_source(s)); };
    return h;
}

HomeItem from_youtube(const youtube::Video& v) {
    HomeItem h;
    h.icon = ic::SMART_DISPLAY;
    h.service = "YouTube";
    h.title = v.title;
    h.subtitle = v.channel + (v.views.empty() ? "" : " \xC2\xB7 " + v.views);
    h.image = youtube::thumbnail(v.id);
    h.hero = youtube::thumbnail_hq(v.id);
    h.badge = v.live ? "" : youtube::duration_label(v);
    h.live = v.live;
    h.open = [v] { yt::play(v); };
    h.more = [v] { yt::video_menu(v); };
    return h;
}

HomeItem from_station(const radio::Station& s) {
    HomeItem h;
    h.icon = ic::RADIO;
    h.service = tr("Radio");
    h.title = s.name;
    h.subtitle = s.country;
    h.description = s.tags;
    h.image = h.hero = s.favicon;
    h.live = false;
    h.open = [s] { play_audio(radio::make_source(s)); };
    return h;
}

class HomeScreen : public app::Screen {
public:
    HomeScreen() { refresh(); }
    app::Section section() const override { return app::SEC_HOME; }
    void on_enter() override {
        dirty_ = true;
        if (ui::time() - refreshed_ > 3) refresh();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("home");
        build_rows();

        // --- hero -------------------------------------------------------------------
        const float hero_h = 350;
        const HomeItem* f = focused_item();
        if (f && f->hero != hero_url_) {
            prev_hero_ = hero_url_;
            hero_url_ = f->hero;
            hero_t_ = ui::time();
            set_backdrop(f->hero);
        }
        float ht = anim::smoothstep((float)(ui::time() - hero_t_) / 0.5f);
        draw_hero_image(prev_hero_, 1 - ht);
        draw_hero_image(hero_url_, ht);

        float ty = 64;
        text::draw(font::label, x0, ty, greeting(), t.accent);
        ty += 38;
        if (f) {
            gfx::push_alpha(0.4f + 0.6f * ht);
            float slide = (1 - anim::ease_out_cubic(ht)) * 16;
            auto lines = text::wrap(font::display, f->title, 640, 2);
            for (auto& l : lines) {
                text::draw_shadowed(font::display, x0 - slide, ty, l, t.text);
                ty += text::line_height(font::display);
            }
            float mx = x0;
            // Service badge
            float bw = text::measure(font::caption, f->service) + 40;
            Rect badge(mx, ty + 8, bw, 28);
            gfx::fill_rrect(badge, 14, Color(255, 255, 255, 36));
            text::icon(f->icon, 16, badge.x + 16, badge.cy(), t.accent);
            text::draw(font::caption, badge.x + 28, badge.y + 6, f->service, t.text);
            mx += bw + 12;
            if (f->live) {
                Rect lb(mx, ty + 8, std::max(52.0f, text::measure(font::caption, tr("LIVE")) + 20), 28);
                gfx::fill_rrect(lb, 7, t.bad);
                text::draw(font::caption, lb.cx(), lb.y + 6, tr("LIVE"), gfx::WHITE, text::CENTER);
                mx += lb.w + 12;
            }
            text::draw_fit(font::body_bold, mx, ty + 10, 640 - (mx - x0), f->subtitle, t.text2);
            ty += 48;
            if (!f->description.empty()) text::draw_wrapped(font::body, Rect(x0, ty, 620, 80), f->description, t.text2, 2);
            gfx::pop_alpha();
        } else {
            text::draw(font::display, x0, ty, "CoffeeFlix", t.text);
            text::draw(font::body, x0, ty + 64, tr("YouTube, Twitch, Jellyfin, radio, podcasts and your own files."), t.text2);
        }

        // --- shelves --------------------------------------------------------------------
        page_.begin(id(g, "page"), hero_h, H - hero_h);
        gfx::push_clip(Rect(0, hero_h - 30, W, H - hero_h + 30));
        float y = page_.y(hero_h);
        for (size_t r = 0; r < rows_.size(); r++) {
            Row& row = rows_[r];
            if (row.items.empty() && !row.loading) continue;
            ShelfSpec s;
            s.title_str = tr(row.title.c_str());
            s.count = (int)row.items.size();
            s.loading = row.loading;
            s.shape = row.shape;
            s.item_w = row.w;
            s.item = [&row](int i) {
                const HomeItem& h = row.items[i];
                CardInfo c;
                c.image = h.image;
                c.title = h.title;
                c.subtitle = h.subtitle;
                c.progress = h.progress;
                c.badge = h.badge;
                c.live = h.live;
                c.icon = h.icon;
                return c;
            };
            s.on_click = [&row](int i) {
                if (row.items[i].open) row.items[i].open();
            };
            s.on_x = [&row](int i) {
                if (row.items[i].more) row.items[i].more();
            };
            s.on_focus = [this, r](int i) {
                focus_row_ = (int)r;
                focus_col_ = i;
            };
            y += shelf(id(g, row.title), x0, y, s, &page_) + 8;
        }
        y += start_tiles(g, x0, y);
        gfx::pop_clip();
        page_.end(y + page_.scroll());
        bool more = focus_row_ >= 0 && focus_row_ < (int)rows_.size() && focus_col_ >= 0 &&
                    focus_col_ < (int)rows_[focus_row_].items.size() && rows_[focus_row_].items[focus_col_].more;
        if (more) hint_bar({{"A", tr("Open")}, {"X", tr("More")}});
        else hint_bar({{"A", tr("Open")}});
    }

private:
    void draw_hero_image(const std::string& url, float a) {
        if (url.empty() || a <= 0.01f) return;
        const images::Image* img = images::get(url, 1280, 720);
        if (!img || !img->ready) return;
        float fa = a * images::fade(img, 0.4f);
        Rect r(W * 0.38f, 0, W * 0.62f, 400);
        float zoom = 1.0f + 0.04f * anim::smoothstep((float)(ui::time() - hero_t_) / 8.0f);
        gfx::image_cover(img->tex, img->w, img->h, r.scaled(zoom), 0, Color(255, 255, 255, (uint8_t)(235 * fa)), 0.3f);
        // Fade into the page on the left and bottom.
        const Theme& t = theme();
        gfx::fill_rect_hgrad(Rect(r.x - 2, 0, 360, 400), t.bg_top, t.bg_top.with_a(0));
        gfx::fill_rect_vgrad(Rect(r.x, 250, r.w, 152), t.bg_top.with_a(0), Color(12, 9, 16, 255));
        gfx::fill_rect_vgrad(Rect(r.x, 0, r.w, 60), Color(12, 9, 16, 120), Color(12, 9, 16, 0));
    }

    const HomeItem* focused_item() {
        if (focus_row_ >= 0 && focus_row_ < (int)rows_.size() && focus_col_ >= 0 &&
            focus_col_ < (int)rows_[focus_row_].items.size())
            return &rows_[focus_row_].items[focus_col_];
        for (auto& r : rows_)
            if (!r.items.empty()) return &r.items[0];
        return nullptr;
    }

    float start_tiles(Id g, float x0, float y) {
        struct Tile { const char* title; const char* sub; int icon; app::Section s; uint32_t color; };
        const Tile tiles[] = {
            {"YouTube", tr("Search and watch"), ic::SMART_DISPLAY, app::SEC_YOUTUBE, 0xE53935},
            {"Jellyfin", jellyfin::account().valid() ? tr("Your library") : tr("Connect your server"), ic::VIDEO_LIBRARY,
             app::SEC_JELLYFIN, 0x7E57C2},
            {"Twitch", tr("Live streams"), ic::LIVE_TV, app::SEC_TWITCH, 0x9146FF},
            {tr("Radio"), tr("40,000 stations"), ic::RADIO, app::SEC_RADIO, 0xFB8C00},
            {tr("Podcasts"), tr("Shows and episodes"), ic::PODCASTS, app::SEC_PODCASTS, 0xD81B60},
            {tr("My Media"), tr("SD card and network"), ic::FOLDER, app::SEC_MEDIA, 0x00897B},
        };
        ShelfSpec s;
        s.title = tr("Explore");
        s.count = 6;
        s.item_w = 220;
        s.item = [&tiles](int i) {
            CardInfo c;
            c.title = tiles[i].title;
            c.subtitle = tiles[i].sub;
            c.icon = tiles[i].icon;
            c.tint = gfx::rgb(tiles[i].color).alpha(0.85f);
            return c;
        };
        s.on_click = [&tiles](int i) { app::open_section(tiles[i].s); };
        return shelf(id(g, "explore"), x0, y, s, &page_) + 20;
    }

    void refresh() {
        refreshed_ = ui::time();
        dirty_ = true;
        scope_.reset();
        jf_loading_ = jellyfin::account().valid();
        tw_loading_ = !store::favs("twitch").empty();
        yt_loading_ = true;
        if (jf_loading_) {
            scope_.run<std::pair<jellyfin::List, jellyfin::List>>(
                [] { return std::make_pair(jellyfin::resume(), jellyfin::next_up()); },
                [this](std::pair<jellyfin::List, jellyfin::List> r) {
                    jf_resume_ = std::move(r.first.items);
                    jf_next_ = std::move(r.second.items);
                    jf_loading_ = false;
                    dirty_ = true;
                });
        }
        if (tw_loading_) {
            scope_.run<twitch::Streams>([] { return twitch::followed(); }, [this](twitch::Streams s) {
                tw_live_.clear();
                for (auto& st : s.items)
                    if (st.live) tw_live_.push_back(st);
                tw_loading_ = false;
                dirty_ = true;
            });
        }
        yt_personal_ = yt::has_for_you();
        bool personal = yt_personal_;
        scope_.run<youtube::Results>([personal] { return personal ? yt::for_you() : youtube::trending(); },
                                     [this](youtube::Results r) {
            yt_ = std::move(r.items);
            yt_loading_ = false;
            dirty_ = true;
        });
    }

    void build_rows() {
        // Only when something changed: favorites/resume lists come from the store.
        if (!dirty_) return;
        dirty_ = false;
        rows_.clear();
        Row cont{N_("Continue watching"), CARD_WIDE, 300, {}, false};
        for (auto& jr : jf_resume_) cont.items.push_back(from_jellyfin(jr));
        for (auto& r : store::resume_list(12)) cont.items.push_back(from_resume(r));
        cont.loading = cont.items.empty() && jf_loading_;
        rows_.push_back(std::move(cont));

        Row next{N_("Next up"), CARD_WIDE, 300, {}, false};
        for (auto& it : jf_next_) next.items.push_back(from_jellyfin(it));
        rows_.push_back(std::move(next));

        Row live{N_("Live on Twitch"), CARD_WIDE, 300, {}, tw_loading_};
        for (auto& s : tw_live_) live.items.push_back(from_twitch(s));
        rows_.push_back(std::move(live));

        Row stations{N_("Your radio stations"), CARD_SQUARE, 160, {}, false};
        for (auto& s : radio::favorites()) stations.items.push_back(from_station(s));
        rows_.push_back(std::move(stations));

        Row yt{yt_personal_ ? N_("Recommended for you") : N_("Popular on YouTube"), CARD_WIDE, 300, {}, yt_loading_};
        for (auto& v : yt_) yt.items.push_back(from_youtube(v));
        rows_.push_back(std::move(yt));
    }

    std::vector<Row> rows_;
    std::vector<jellyfin::Item> jf_resume_, jf_next_;
    std::vector<twitch::Stream> tw_live_;
    std::vector<youtube::Video> yt_;
    bool yt_personal_ = false;
    bool jf_loading_ = false, tw_loading_ = false, yt_loading_ = false;
    bool dirty_ = true;
    int focus_row_ = -1, focus_col_ = -1;
    std::string hero_url_, prev_hero_;
    double hero_t_ = -10, refreshed_ = 0;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_home() { return std::make_unique<HomeScreen>(); }

}  // namespace screens
