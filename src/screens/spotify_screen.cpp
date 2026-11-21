// Spotify (services/spotify), an optional section: signing in with a phone, then the account's library:
// playlists, liked songs, saved albums, followed artists and saved episodes.
#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

#include "core/i18n.hpp"
#include "core/qr.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "app/personal.hpp"
#include "app/spotify_podcasts.hpp"
#include "app/spotify_search_index.hpp"
#include "gfx/images.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/podcasts.hpp"
#include "services/spotify/spotify.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using spotify::Item;

// A list from the library, loaded on a worker.
struct Loader {
    std::vector<Item> items;
    std::string error;
    bool loading = false, loaded = false;
    tasks::Scope scope;

    void load(std::function<std::vector<Item>(std::string&)> fn) {
        scope.reset();
        loading = true;
        loaded = false;
        struct Got {
            std::vector<Item> items;
            std::string error;
        };
        scope.run<Got>([fn = std::move(fn)] {
            Got g;
            g.items = fn(g.error);
            return g;
        }, [this](Got g) {
            items = std::move(g.items);
            error = std::move(g.error);
            loading = false;
            loaded = true;
        });
    }
    // A long list in parts: shown from the first part, the rest coming in after it (`loading` meanwhile).
    void load_parts(spotify::Parts parts) {
        scope.reset();
        items.clear();
        error.clear();
        loading = true;
        loaded = false;
        next_part(std::move(parts));
    }
    void reset() {
        scope.reset();
        items.clear();
        error.clear();
        loading = loaded = false;
    }

private:
    void next_part(spotify::Parts parts) {
        struct Got {
            spotify::Part part;
            std::string error;
        };
        scope.run<Got>([parts] {
            Got g;
            g.part = parts(g.error);
            return g;
        }, [this, parts](Got g) {
            for (Item& it : g.part.items) items.push_back(std::move(it));
            if (items.empty()) error = std::move(g.error);
            loaded = true;
            loading = g.part.more;
            if (g.part.more) next_part(parts);
        });
    }
};

// Plays from `index` on: the queue takes up to 1,000 of a long list around it (a queue entry is far bigger than a
// list's), or with `shuffle` up to 1,000 of all of it, mixed.
void play_items(const std::vector<Item>& items, int index, bool shuffle) {
    if (items.empty()) return;
    constexpr int WINDOW = 500;
    const int n = (int)items.size();
    index = std::clamp(index, 0, n - 1);
    std::vector<int> pick;
    if (shuffle) {
        for (int i = 0; i < n; i++) pick.push_back(i);
        std::mt19937 g(std::random_device{}());
        std::shuffle(pick.begin(), pick.end(), g);
        if ((int)pick.size() > 2 * WINDOW) pick.resize(2 * WINDOW);
        index = 0;
    } else {
        const int lo = std::max(0, index - WINDOW), hi = std::min(n, index + WINDOW);
        for (int i = lo; i < hi; i++) pick.push_back(i);
        index -= lo;
    }
    std::vector<player::Source> q;
    q.reserve(pick.size());
    for (int i : pick) q.push_back(spotify::source(items[i]));
    play_audio_queue(std::move(q), index);
}

CardInfo card(const Item& it, int icon) {
    CardInfo c;
    c.image = it.image;
    c.image_w = 300;
    c.title = it.name;
    c.subtitle = it.subtitle;
    c.icon = icon;
    if (it.year > 0) c.subtitle += (c.subtitle.empty() ? "" : " \xC2\xB7 ") + std::to_string(it.year);
    if (it.count > 0) c.badge = util::fmt(tr("%d songs"), it.count);
    return c;
}

std::unique_ptr<app::Screen> make_list(Item head, int icon, std::function<std::vector<Item>(std::string&)> load);
std::unique_ptr<app::Screen> make_list(Item head, int icon, spotify::Parts parts);
std::unique_ptr<app::Screen> make_playlist(const Item& playlist);
std::unique_ptr<app::Screen> make_album(const Item& album);
std::unique_ptr<app::Screen> make_artist(const Item& artist);

// Songs or episodes, one row each. A long list has only the rows around the view (and the one in focus) taking
// part each frame: thousands of them would cost a Wii U's frame. Moving on brings the next in as the page scrolls.
float item_rows(Id g, float x0, float y, const std::vector<Item>& items, Page& page) {
    const Theme& t = theme();
    constexpr float ROW = 62, AROUND = 600;
    static std::unordered_map<Id, size_t> focused_row;  // by list: the row last in focus
    const size_t n = items.size();
    const size_t first = y < -AROUND ? (size_t)((-AROUND - y) / ROW) : 0;
    const size_t last = std::min(n, (size_t)std::max(0.0f, (H + AROUND - y) / ROW) + 1);
    auto fr = focused_row.find(g);
    for (size_t i = 0; i < n; i++) {
        if ((i < first || i >= last) && !(fr != focused_row.end() && fr->second == i)) continue;
        const Item& s = items[i];
        Rect r(x0, y + ROW * i, W - x0 - 60, 56);
        Id rid = id(g, (int64_t)i);
        ui::Item it = focusable(rid, r, g);
        if (it.focused) {
            page.focus_range(r.y + page.scroll() - 10, r.b() + page.scroll() + 10);
            focused_row[g] = i;
        }
        if (r.b() > -60 && r.y < H + 60) {
            Rect br = r.offset(bump_x(rid), bump_y(rid));
            gfx::fill_rrect(br, 12, gfx::lerp(Color(255, 255, 255, i % 2 ? 6 : 14), t.surface_focus, it.f));
            Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
            Color fg2 = gfx::lerp(t.text3, Color(21, 18, 26, 160), it.f);
            text::draw(font::small_bold, br.x + 30, br.cy() - 10, std::to_string((int)i + 1), fg2, text::CENTER);
            std::string line = s.name;
            if (!s.subtitle.empty()) line += " \xC2\xB7 " + s.subtitle;
            text::draw_fit(font::body, br.x + 64, br.cy() - 12, br.w - 180, line, fg);
            if (s.duration_ms > 0)
                text::draw(font::small_bold, br.r() - 20, br.cy() - 10, util::format_duration(s.duration_ms / 1000.0), fg2,
                           text::RIGHT);
        }
        if (it.clicked) play_items(items, (int)i, false);
    }
    return y + ROW * n;
}

// Cover, name, what it is; Play and Shuffle. Returns where the list goes.
float list_header(Id g, float x0, float y, const Item& head, int icon, const std::vector<Item>& items, bool ready) {
    const Theme& t = theme();
    const float side = 180;
    Rect cr(x0, y, side, side);
    const images::Image* img = head.image.empty() ? nullptr : images::get(head.image, 300, 300);
    if (img && img->ready && img->tex) {
        gfx::image_cover(img->tex, img->w, img->h, cr, 16);
    } else {
        gfx::fill_rrect(cr, 16, t.surface);
        text::icon(icon, 64, cr.cx(), cr.cy(), t.text2);
    }
    const float mx = x0 + side + 30;
    text::draw_fit(font::title, mx, y + 12, W - mx - 60, head.name, t.text);
    std::string sub = head.subtitle;
    if (head.year > 0) sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::to_string(head.year);
    if (ready) {
        const bool episodes = !items.empty() && util::starts_with(items[0].uri, "spotify:episode:");
        sub += (sub.empty() ? "" : " \xC2\xB7 ") + util::fmt(episodes ? tr("%d episodes") : tr("%d songs"), (int)items.size());
    }
    text::draw_fit(font::body, mx, y + 58, W - mx - 60, sub, t.text2);
    Id act = id(g, "actions");
    const float pw = std::max(160.0f, measure_button(tr("Play"), ic::PLAY));
    const float sw = std::max(160.0f, measure_button(tr("Shuffle"), ic::SHUFFLE));
    if (button(id(act, "play"), Rect(mx, y + side - 50, pw, 46), tr("Play"), ic::PLAY, BTN_PRIMARY, act, F_DEFAULT))
        play_items(items, 0, false);
    if (button(id(act, "shuffle"), Rect(mx + pw + 14, y + side - 50, sw, 46), tr("Shuffle"), ic::SHUFFLE, BTN_NORMAL, act))
        play_items(items, 0, true);
    return y + side + 36;
}

float list_body(Id g, float x0, float y, Loader& l, Page& page, int icon, const char* empty) {
    if (!l.loaded) {
        loading_indicator(x0 + 40, y + 20);
        return y + 80;
    }
    if (l.items.empty()) {
        empty_state(Rect(x0, y, W - x0 - 60, 180), l.error.empty() ? icon : ic::ERROR_OUTLINE,
                    l.error.empty() ? empty : tr("Couldn't load this"), l.error.c_str());
        return y + 200;
    }
    y = item_rows(id(g, "rows"), x0, y, l.items, page);
    if (l.loading) {  // more on the way
        loading_indicator(x0 + 40, y + 20);
        y += 80;
    }
    return y;
}

// A playlist, the liked songs, the saved episodes or an album: its songs.
class ListScreen : public app::Screen {
public:
    ListScreen(Item head, int icon, std::function<std::vector<Item>(std::string&)> load)
        : head_(std::move(head)), icon_(icon) {
        loader_.load(std::move(load));
    }
    ListScreen(Item head, int icon, spotify::Parts parts) : head_(std::move(head)), icon_(icon) {
        loader_.load_parts(std::move(parts));
    }
    app::Section section() const override { return app::SEC_SPOTIFY; }

    void frame() override {
        const float x0 = content_x();
        const Id g = id("sp_list");
        page_.begin(id(g, "page"));
        float y = list_header(g, x0, page_.y(52), head_, icon_, loader_.items, loader_.loaded && !loader_.loading);
        y = list_body(g, x0, y, loader_, page_, icon_, tr("Nothing here yet"));
        page_.end(y + page_.scroll() + 20);
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    Item head_;
    int icon_;
    Loader loader_;
    Page page_;
};

class ArtistScreen : public app::Screen {
public:
    explicit ArtistScreen(Item a) : head_(std::move(a)) {
        struct Got {
            spotify::Artist artist;
            std::string error;
            bool ok = false;
        };
        scope_.run<Got>([uri = head_.uri] {
            Got g;
            g.ok = spotify::artist(uri, g.artist, g.error);
            return g;
        }, [this](Got g) {
            artist_ = std::move(g.artist);
            error_ = std::move(g.error);
            loaded_ = true;
            if (!artist_.info.image.empty()) head_.image = artist_.info.image;
        });
    }
    app::Section section() const override { return app::SEC_SPOTIFY; }

    void frame() override {
        const Theme& t = theme();
        const float x0 = content_x();
        const Id g = id("sp_artist");
        page_.begin(id(g, "page"));
        float y = list_header(g, x0, page_.y(52), head_, ic::PERSON, artist_.top_tracks, false);
        if (!loaded_) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else {
            if (!artist_.top_tracks.empty()) {
                text::draw(font::title, x0, y, tr("Popular"), t.text);
                y = item_rows(id(g, "top"), x0, y + 48, artist_.top_tracks, page_) + 20;
            }
            y += shelf_of(id(g, "albums"), x0, y, tr("Albums"), artist_.albums);
            y += shelf_of(id(g, "singles"), x0, y, tr("Singles and EPs"), artist_.singles);
            if (artist_.top_tracks.empty() && artist_.albums.empty() && artist_.singles.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 180), ic::ERROR_OUTLINE, tr("Couldn't load this"), error_.c_str());
                y += 200;
            }
        }
        page_.end(y + page_.scroll() + 20);
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    float shelf_of(Id sid, float x, float y, const char* title, const std::vector<Item>& albums) {
        if (albums.empty()) return 0;
        ShelfSpec s;
        s.title_str = title;
        s.count = (int)albums.size();
        s.shape = CARD_SQUARE;
        s.item_w = 180;
        s.item = [&albums](int i) { return card(albums[i], ic::ALBUM); };
        s.on_click = [&albums](int i) { app::push(make_album(albums[i])); };
        return shelf(sid, x, y, s, &page_) + 16;
    }

    Item head_;
    spotify::Artist artist_;
    std::string error_;
    bool loaded_ = false;
    tasks::Scope scope_;
    Page page_;
};

std::unique_ptr<app::Screen> make_list(Item head, int icon, std::function<std::vector<Item>(std::string&)> load) {
    return std::make_unique<ListScreen>(std::move(head), icon, std::move(load));
}

std::unique_ptr<app::Screen> make_list(Item head, int icon, spotify::Parts parts) {
    return std::make_unique<ListScreen>(std::move(head), icon, std::move(parts));
}

std::unique_ptr<app::Screen> make_playlist(const Item& playlist) {
    return make_list(playlist, ic::QUEUE_MUSIC, spotify::playlist_parts(playlist.uri));
}

std::unique_ptr<app::Screen> make_album(const Item& album) {
    return make_list(album, ic::ALBUM, [uri = album.uri](std::string& error) {
        spotify::Album a;
        if (!spotify::album(uri, a, error)) return std::vector<Item>();
        return a.tracks;
    });
}

std::unique_ptr<app::Screen> make_artist(const Item& artist) { return std::make_unique<ArtistScreen>(artist); }

// A podcast's episodes: Spotify doesn't list them to this client, so they come from the podcast's own feed (found
// by its name, the publisher to tell apart ones named alike), as the Podcasts section plays them. A show only on
// Spotify gets its newest episode, which Spotify's public embed page names.
void open_show(const Item& show) {
    static tasks::Scope scope;
    static bool opening = false;
    if (opening) return;
    opening = true;
    toast(tr("Looking for the podcast\xE2\x80\xA6"), ic::PODCASTS);
    struct Found {
        bool feed = false;
        podcasts::Show rss;
        Item latest;
    };
    scope.run<Found>([show] {
        Found f;
        f.feed = spotify::find_feed(show, f.rss);
        if (!f.feed) f.latest = spotify::latest_episode(show.uri);
        return f;
    }, [show](Found f) {
        opening = false;
        if (f.feed) {
            app::push(make_podcast_show(f.rss));
            return;
        }
        Item head = show;
        head.count = 0;
        head.subtitle = tr("Only on Spotify: its newest episode");
        std::vector<Item> one;
        if (!f.latest.uri.empty()) one.push_back(f.latest);
        app::push(make_list(head, ic::PODCASTS, [one](std::string& e) {
            if (one.empty()) e = tr("Spotify doesn't list this podcast's episodes here");
            return one;
        }));
    });
}

// The library searched (spotify::search_library): songs and episodes as rows, the rest as shelves.
class SearchScreen : public app::Screen {
public:
    explicit SearchScreen(std::string query) : query_(std::move(query)) { run(); }
    app::Section section() const override { return app::SEC_SPOTIFY; }

    void frame() override {
        const Theme& t = theme();
        const float x0 = content_x();
        const Id g = id("sp_search");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        const Id top = id(g, "top");
        if (search_bar(id(top, "bar"), Rect(x0, y, W - x0 - 60, 56), query_, tr("Search your Spotify library"), top))
            ask(query_);
        y += 72;
        const std::string note =
            spotify_search_index::running() && spotify_search_index::total() > 0
                ? util::fmt(tr("Your playlists are still being read for the search (%d of %d)"), spotify_search_index::done(),
                            spotify_search_index::total())
                : std::string(tr("Searches your library: Spotify's own search isn't open to CoffeeFlix."));
        text::draw_fit(font::small, x0 + 4, y, W - x0 - 64, note, t.text3);
        y += 40;
        if (!loaded_) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else if (found_.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 180), error_.empty() ? ic::SEARCH : ic::ERROR_OUTLINE,
                        error_.empty() ? tr("Nothing found in your library") : tr("Couldn't load this"), error_.c_str());
            y += 200;
        } else {
            y = rows(id(g, "songs"), x0, y, tr("Liked Songs"), found_.songs);
            y = rows(id(g, "inpl"), x0, y, tr("In your playlists"), found_.in_playlists);
            y = rows(id(g, "eps"), x0, y, tr("Your Episodes"), found_.episodes);
            y += shelf_of(id(g, "pl"), x0, y, tr("Playlists"), found_.playlists, ic::QUEUE_MUSIC, [](const Item& it) {
                app::push(make_playlist(it));
            });
            y += shelf_of(id(g, "al"), x0, y, tr("Albums"), found_.albums, ic::ALBUM, [](const Item& it) { app::push(make_album(it)); });
            y += shelf_of(id(g, "ar"), x0, y, tr("Artists"), found_.artists, ic::PERSON, [](const Item& it) { app::push(make_artist(it)); });
            y += shelf_of(id(g, "po"), x0, y, tr("Podcasts"), found_.podcasts, ic::PODCASTS, [](const Item& it) { open_show(it); });
        }
        page_.end(y + page_.scroll() + 20);
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

    // The keyboard for a search, then its results on a screen of their own.
    static void ask(const std::string& initial) {
        prompt_text(tr("Search your Spotify library"), initial, tr("Song, album, artist, playlist or podcast"), [](std::string q) {
            if (!q.empty()) app::push(std::make_unique<SearchScreen>(q));
        });
    }

private:
    void run() {
        struct Got {
            spotify::Found found;
            std::string error;
        };
        scope_.run<Got>([q = query_] {
            Got g;
            g.found = spotify::search_library(q, g.error);
            return g;
        }, [this](Got g) {
            found_ = std::move(g.found);
            error_ = std::move(g.error);
            loaded_ = true;
        });
    }

    float rows(Id gid, float x0, float y, const char* title, const std::vector<Item>& items) {
        if (items.empty()) return y;
        text::draw(font::title, x0, y, title, theme().text);
        return item_rows(gid, x0, y + 48, items, page_) + 20;
    }

    float shelf_of(Id sid, float x, float y, const char* title, const std::vector<Item>& items, int icon,
                   std::function<void(const Item&)> open) {
        if (items.empty()) return 0;
        ShelfSpec s;
        s.title_str = title;
        s.count = (int)items.size();
        s.shape = CARD_SQUARE;
        s.item_w = 180;
        s.item = [&items, icon](int i) { return card(items[i], icon); };
        s.on_click = [&items, open](int i) { open(items[i]); };
        return shelf(sid, x, y, s, &page_) + 16;
    }

    std::string query_, error_;
    spotify::Found found_;
    bool loaded_ = false;
    tasks::Scope scope_;
    Page page_;
};

// --- the section -------------------------------------------------------------------------------------

// Turning Spotify off: it leaves the side menu (and Home), and nothing of it runs until it's turned on again.
void confirm_turn_off() {
    show_menu(tr("Turn off Spotify?"), tr("Spotify leaves the side menu and Home."),
              {{tr("Turn off Spotify"), ic::CLOSE, [] {
                    personal::set_hidden(app::SEC_SPOTIFY, true);
                    app::open_section(app::SEC_HOME);
                    toast(tr("Spotify is off"), ic::CHECK_CIRCLE);
                }, util::fmt(tr("To turn it on again: %s"), (std::string(tr("Settings")) + " > " + tr("Make it yours") +
                                                              " > " + tr("Side menu")).c_str())},
               {tr("Keep it"), ic::ARROW_BACK, [] {}, ""}});
}

class SpotifyScreen : public app::Screen {
public:
    enum Tab { TAB_PLAYLISTS, TAB_ALBUMS, TAB_ARTISTS, TAB_PODCASTS };

    SpotifyScreen() {
        if (spotify::signed_in()) load();
        else request();
    }
    app::Section section() const override { return app::SEC_SPOTIFY; }

    void frame() override {
        if (spotify::signed_in() != was_signed_in_) {  // signed in here, or out from the menu
            was_signed_in_ = spotify::signed_in();
            if (was_signed_in_) load();
            else reset_library(), request();
            reset_focus();
        }
        if (spotify::signed_in()) library();
        else sign_in();
    }

private:
    // --- library ---

    void load() {
        playlists_.load([](std::string& e) { return spotify::playlists(e); });
        if (tab_ == TAB_ALBUMS) albums_.load([](std::string& e) { return spotify::saved_albums(e); });
        if (tab_ == TAB_ARTISTS) artists_.load([](std::string& e) { return spotify::followed_artists(e); });
        if (tab_ == TAB_PODCASTS) shows_.load([](std::string& e) { return spotify::followed_shows(e); });
    }
    void reset_library() {
        covers_.reset();
        covers_started_ = false;
        playlists_.reset();
        shows_.reset();
        albums_.reset();
        artists_.reset();
    }

    void library() {
        // Once a visit: whether to take the podcasts over (asked until answered), else a sync now and then. Asked once
        // the screen is settled on top: a menu opened during the change of section closes with it.
        if (!asked_) {
            if (shown_at_ < 0) shown_at_ = ui::time();
            if (app::top() == this && ui::time() - shown_at_ > 0.5 && !menu_active() && !prompt_active()) {
                asked_ = true;
                if (spotify_podcasts::choice() == spotify_podcasts::UNASKED) ask_podcasts();
                else spotify_podcasts::maybe_sync();
                spotify_search_index::maybe_run();
            }
        }
        const Theme& t = theme();
        const float x0 = content_x();
        const Id g = id("sp_home");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Spotify", t.text);
        {  // the username, hidden until it's asked for (it's an account's id: screenshots and streams shouldn't show it)
            const Id uid = id(g, "user");
            const std::string shown = show_user_ ? spotify::user_name() : std::string(tr("Signed in")) + " \xC2\xB7 \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
            const float uw = std::min(500.0f, text::measure(font::small, shown) + 24);
            const Rect ur(x0 - 8, y + 54, uw, 34);
            ui::Item it = focusable(uid, ur, id(g, "top"));
            if (it.f > 0.01f) gfx::fill_rrect(ur, 10, gfx::lerp(Color(0, 0, 0, 0), t.surface_focus, it.f));
            text::draw_fit(font::small, x0 + 4, y + 60, 500, shown, gfx::lerp(t.text3, gfx::rgb(0x15121A), it.f));
            if (it.clicked) show_user_ = !show_user_;
        }
        if (spotify_search_index::running() && spotify_search_index::total() > 0) {  // the search index, coming along
            const std::string p = util::fmt(tr("Preparing your playlists for the search\xE2\x80\xA6 %d/%d"),
                                            spotify_search_index::done(), spotify_search_index::total());
            text::draw_fit(font::small, W - 60 - 28 - 28 - 24, y + 66, 420, p, t.text3, text::RIGHT);
        }
        const Id top = id(g, "top");
        if (icon_button(id(top, "account"), W - 60 - 28, y + 32, 28, ic::ACCOUNT_CIRCLE, top, 0, true)) account_menu();
        {
            const float bx = x0 + 330, bw = W - 60 - 28 - 28 - 24 - bx;
            if (search_bar(id(top, "search"), Rect(bx, y + 4, bw, 56), "", tr("Search your Spotify library"), top))
                SearchScreen::ask("");
        }
        y += 96;

        const struct {
            Tab tab;
            const char* name;
            int icon;
        } TABS[] = {
            {TAB_PLAYLISTS, N_("Playlists"), ic::QUEUE_MUSIC},
            {TAB_ALBUMS, N_("Albums"), ic::ALBUM},
            {TAB_ARTISTS, N_("Artists"), ic::PERSON},
            {TAB_PODCASTS, N_("Podcasts"), ic::PODCASTS},
        };
        float cx = x0;
        for (const auto& item : TABS) {
            const float w = text::measure(font::small_bold, tr(item.name)) + 60;
            if (chip(id(top, (int64_t)item.tab), Rect(cx, y, w, 42), tr(item.name), tab_ == item.tab, top, item.icon)) {
                tab_ = item.tab;
                if (tab_ == TAB_ALBUMS && !albums_.loaded && !albums_.loading)
                    albums_.load([](std::string& e) { return spotify::saved_albums(e); });
                if (tab_ == TAB_ARTISTS && !artists_.loaded && !artists_.loading)
                    artists_.load([](std::string& e) { return spotify::followed_artists(e); });
                if (tab_ == TAB_PODCASTS && !shows_.loaded && !shows_.loading)
                    shows_.load([](std::string& e) { return spotify::followed_shows(e); });
            }
            cx += w + 14;
        }
        y += 70;

        switch (tab_) {
        case TAB_PLAYLISTS: y = playlists_grid(id(g, "pl"), x0, y); break;
        case TAB_ALBUMS:
            y = items_grid(id(g, "al"), x0, y, albums_, ic::ALBUM, tr("No saved albums"),
                           [](const Item& it) { app::push(make_album(it)); });
            break;
        case TAB_PODCASTS: y = podcasts_grid(id(g, "po"), x0, y); break;
        case TAB_ARTISTS:
            y = items_grid(id(g, "ar"), x0, y, artists_, ic::PERSON, tr("No followed artists"),
                           [](const Item& it) { app::push(make_artist(it)); });
            break;
        }
        page_.end(y + page_.scroll() + 20);
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

    // The liked songs first, as Spotify's apps have them, then the playlists.
    float playlists_grid(Id gid, float x0, float y) {
        if (playlists_.loaded && playlists_.items.empty() && !playlists_.error.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 200), ic::ERROR_OUTLINE, tr("Couldn't load this"),
                        playlists_.error.c_str());
            return y + 220;
        }
        if (playlists_.loaded && !covers_started_) fill_covers();
        GridSpec s;
        s.count = 1 + (int)playlists_.items.size();
        s.loading = !playlists_.loaded;
        s.cols = 5;
        s.shape = CARD_SQUARE;
        s.item_w = 175;
        s.item = [this](int i) {
            if (i == 0) return card(liked_item(), ic::FAVORITE);
            return card(playlists_.items[i - 1], ic::QUEUE_MUSIC);
        };
        s.on_click = [this](int i) {
            if (i == 0)
                app::push(make_list(liked_item(), ic::FAVORITE, spotify::liked_parts()));
            else
                app::push(make_playlist(playlists_.items[i - 1]));
        };
        return y + grid(gid, x0, y, s, &page_) + 16;
    }

    // Playlists without a picture of their own get the one Spotify's apps make, one after another in the background.
    void fill_covers() {
        covers_started_ = true;
        missing_.clear();
        for (const Item& it : playlists_.items)
            if (it.image.empty()) missing_.push_back(it.uri);
        next_cover();
    }
    void next_cover() {
        if (missing_.empty()) return;
        const std::string uri = missing_.front();
        missing_.erase(missing_.begin());
        covers_.run<std::string>([uri] { return spotify::playlist_cover(uri); }, [this, uri](std::string image) {
            for (Item& it : playlists_.items)
                if (it.uri == uri && it.image.empty()) it.image = image;
            next_cover();
        });
    }

    // Whether to subscribe in CoffeeFlix to the podcasts followed on Spotify.
    static void ask_podcasts() {
        show_menu(tr("Your podcasts in CoffeeFlix?"), tr("With all their episodes, in Podcasts"),
                  {{tr("Yes, subscribe to them"), ic::PODCASTS, [] { spotify_podcasts::turn_on(); },
                    tr("The shows you follow on Spotify. You can turn it off any time in Settings > Accounts")},
                   {tr("Not now"), ic::CLOSE, [] { spotify_podcasts::turn_off(false); }, ""}});
    }

    // The podcasts to show here: while they're taken over, the ones only on Spotify (the rest are in Podcasts).
    void filter_shows() {
        shown_shows_.clear();
        moved_ = 0;
        const bool on = spotify_podcasts::choice() == spotify_podcasts::ON;
        const std::set<std::string> moved = on ? spotify_podcasts::taken_over() : std::set<std::string>();
        for (const Item& it : shows_.items) {
            if (moved.count(it.uri)) moved_++;
            else shown_shows_.push_back(it);
        }
        filtered_for_ = shows_.items.size();
        filtered_sync_ = spotify_podcasts::syncing();
        filtered_choice_ = spotify_podcasts::choice();
    }

    // "Your Episodes" first, then the podcasts followed.
    float podcasts_grid(Id gid, float x0, float y) {
        if (shows_.loaded && (filtered_for_ != shows_.items.size() || filtered_sync_ != spotify_podcasts::syncing() ||
                              filtered_choice_ != spotify_podcasts::choice()))
            filter_shows();
        if (spotify_podcasts::choice() == spotify_podcasts::ON) {
            const Theme& t = theme();
            std::string note = spotify_podcasts::syncing() ? tr("Taking over your podcasts from Spotify\xE2\x80\xA6")
                               : moved_ > 0 ? util::fmt(moved_ == 1 ? tr("1 more podcast is in your subscriptions in Podcasts")
                                                                    : tr("%d more podcasts are in your subscriptions in Podcasts"),
                                                         moved_)
                                            : std::string();
            if (!note.empty()) {
                text::icon(ic::INFO, 20, x0 + 10, y + 11, t.text3);
                text::draw(font::small, x0 + 30, y, note, t.text3);
                y += 40;
            }
        }
        GridSpec s;
        s.count = 1 + (int)shown_shows_.size();
        s.loading = !shows_.loaded;
        s.cols = 5;
        s.shape = CARD_SQUARE;
        s.item_w = 175;
        s.item = [this](int i) { return i == 0 ? card(episodes_item(), ic::PODCASTS) : card(shown_shows_[i - 1], ic::PODCASTS); };
        s.on_click = [this](int i) {
            if (i == 0) {
                app::push(make_list(episodes_item(), ic::PODCASTS, [](std::string& e) { return spotify::saved_episodes(e); }));
                return;
            }
            open_show(shown_shows_[i - 1]);
        };
        float h = grid(gid, x0, y, s, &page_) + 16;
        if (shows_.loaded && shows_.items.empty() && !shows_.error.empty())
            text::draw(font::small, x0, y + h, shows_.error, theme().text3);
        return y + h + 30;
    }

    static Item liked_item() {
        Item it;
        it.name = tr("Liked Songs");
        return it;
    }
    static Item episodes_item() {
        Item it;
        it.name = tr("Your Episodes");
        it.subtitle = tr("Saved in Spotify's apps");
        return it;
    }

    float items_grid(Id gid, float x0, float y, Loader& l, int icon, const char* empty, std::function<void(const Item&)> open) {
        if (l.loaded && l.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 200), l.error.empty() ? icon : ic::ERROR_OUTLINE,
                        l.error.empty() ? empty : tr("Couldn't load this"), l.error.c_str());
            return y + 220;
        }
        GridSpec s;
        s.count = (int)l.items.size();
        s.loading = !l.loaded;
        s.cols = 5;
        s.shape = CARD_SQUARE;
        s.item_w = 175;
        s.item = [&l, icon](int i) { return card(l.items[i], icon); };
        s.on_click = [&l, open](int i) { open(l.items[i]); };
        return y + grid(gid, x0, y, s, &page_) + 16;
    }

    void account_menu() {
        std::vector<MenuItem> items;
        items.push_back({tr("Sign out"), ic::LOGOUT, [] {
                             spotify_search_index::stop();
                             spotify::sign_out();
                             toast(tr("Signed out of Spotify"), ic::LOGOUT);
                         }});
        items.push_back({tr("Turn off Spotify"), ic::CLOSE, [] { confirm_turn_off(); }, ""});
        show_menu("Spotify", tr("Signed in to Spotify"), std::move(items));
    }

    // --- signing in ---

    void sign_in() {
        const Theme& t = theme();
        const float x0 = content_x();
        const Id g = id("spotify");
        text::draw(font::display, x0, 60, "Spotify", t.text);
        text::icon(ic::INFO, 22, x0 + 13, 134, t.accent);
        text::draw(font::body_bold, x0 + 34, 122, tr("Needs Spotify Premium."), t.text);
        text::draw(font::body, x0 + 34 + text::measure(font::body_bold, tr("Needs Spotify Premium.")) + 8, 122,
                   tr("Unofficial: not affiliated with or endorsed by Spotify."), t.text2);

        const Rect panel(x0, 176, W - x0 - 60, 400);
        gfx::shadow(panel, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(panel, 26, Color(255, 255, 255, 12));
        gfx::stroke_rrect(panel, 26, 1, Color(255, 255, 255, 18));

        // Don't want it: off, after asking (left under the panel).
        {
            const Id og = id(g, "off");
            const float ow = measure_button(tr("Turn off Spotify"), ic::CLOSE);
            if (button(id(og, "off"), Rect(x0, panel.b() + 22, ow, 46), tr("Turn off Spotify"), ic::CLOSE, BTN_GHOST, og))
                confirm_turn_off();
        }

        if (login_.user_code.empty()) {
            if (error_.empty()) {
                loading_indicator(panel.cx(), panel.cy() - 10, tr("Getting a code\xE2\x80\xA6"));
            } else if (empty_state_action(id(g, "retry"), panel, ic::WIFI_OFF, tr("Couldn't start signing in"),
                                          error_.c_str())) {
                request();
            }
            hint_bar({{"B", tr("Back")}});
            return;
        }

        if (login_.url != qr_text_) qr_text_ = login_.url, qr_ = qr::encode(login_.url);
        const float side = qr_code(qr_, panel.x + 52, panel.y + 48, 236) > 0 ? 236 : 0;
        text::draw(font::small_bold, panel.x + 52 + side * 0.5f, panel.y + 48 + side + 18, tr("Scan with your phone"),
                   t.text2, text::CENTER);
        const float rx = panel.x + 356;
        text::draw(font::body, rx, panel.y + 52, tr("Or go to spotify.com/pair and enter"), t.text2);
        text::draw(text::font(text::BOLD, 48), rx, panel.y + 90, login_.user_code, t.text);
        text::draw_wrapped(font::body, Rect(rx, panel.y + 176, panel.r() - rx - 40, 90),
                           tr("Spotify's page will name its desktop app: that's how CoffeeFlix signs in. Your password "
                              "stays with Spotify."),
                           t.text3, 3);
        const float sy = panel.b() - 52;
        if (error_.empty()) {
            spinner(rx + 10, sy + 10, 9, t.accent, 3);
            text::draw(font::small_bold, rx + 30, sy, tr("Waiting for you to allow it\xE2\x80\xA6"), t.text2);
            poll();
        } else {
            text::icon(ic::ERROR_OUTLINE, 22, rx + 10, sy + 10, t.bad);
            text::draw(font::small_bold, rx + 30, sy, error_, t.bad);
            const Id bg = id(g, "buttons");
            const float nw = measure_button(tr("New code"), ic::REFRESH);
            if (button(id(bg, "new"), Rect(panel.r() - nw, panel.b() + 22, nw, 46), tr("New code"), ic::REFRESH,
                       BTN_PRIMARY, bg, F_DEFAULT))
                request();
        }
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

    void request() {
        error_.clear();
        login_ = spotify::DeviceLogin();
        polling_ = false;
        poll_.reset();
        struct Got {
            spotify::DeviceLogin login;
            std::string error;
        };
        scope_.run<Got>([] {
            Got g;
            spotify::begin_login(g.login, g.error);
            return g;
        }, [this](Got g) {
            if (g.login.user_code.empty()) {
                error_ = g.error;
                return;
            }
            login_ = g.login;
            next_poll_ = ui::time() + login_.interval;
            reset_focus();
        });
    }

    void poll() {
        if (polling_ || ui::time() < next_poll_) return;
        polling_ = true;
        struct Answer {
            spotify::DeviceLogin login;
            spotify::LoginState state;
            std::string error;
        };
        poll_.run<Answer>([login = login_] {
            Answer a{login, spotify::LoginState::WAITING, ""};
            a.state = spotify::check_login(a.login, a.error);
            return a;
        }, [this](Answer a) {
            polling_ = false;
            login_.interval = a.login.interval;
            next_poll_ = ui::time() + login_.interval;
            if (a.state == spotify::LoginState::SIGNED_IN) {
                toast(tr("Signed in to Spotify"), ic::CHECK_CIRCLE);
                asked_ = false;  // the library asks about the podcasts as it first shows
                shown_at_ = -1;
            } else if (a.state == spotify::LoginState::FAILED) {
                error_ = a.error;
                reset_focus();
            }
        });
    }

    Tab tab_ = TAB_PLAYLISTS;
    bool show_user_ = false, covers_started_ = false, asked_ = false;
    double shown_at_ = -1;
    std::vector<Item> shown_shows_;
    size_t filtered_for_ = (size_t)-1;
    bool filtered_sync_ = false;
    int moved_ = 0;
    spotify_podcasts::Choice filtered_choice_ = spotify_podcasts::UNASKED;
    tasks::Scope covers_;
    std::vector<std::string> missing_;
    Loader playlists_, albums_, artists_, shows_;
    Page page_;
    bool was_signed_in_ = spotify::signed_in();

    spotify::DeviceLogin login_;
    std::string error_, qr_text_;
    qr::Code qr_;
    double next_poll_ = 0;
    bool polling_ = false;
    tasks::Scope scope_, poll_;
};

}  // namespace

std::unique_ptr<app::Screen> make_spotify() {
    // From Settings with the section turned off: the screen on its own.
    return std::make_unique<SpotifyScreen>();
}

}  // namespace screens
