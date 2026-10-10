// Navidrome: server connection, library browsing, albums, artists, playlists, and audio queue.
#include <algorithm>
#include <cmath>
#include <random>

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/navidrome.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace nd = navidrome;

// Forward declarations
std::unique_ptr<app::Screen> make_album_screen(const nd::Album& al);
std::unique_ptr<app::Screen> make_artist_screen(const nd::Artist& art);
std::unique_ptr<app::Screen> make_playlist_screen(const nd::Playlist& pl);

struct Loader {
    nd::List list;
    bool loading = false, loaded = false;
    tasks::Scope scope;

    void load(std::function<nd::List()> fn) {
        scope.reset();
        loading = true;
        scope.run<nd::List>(std::move(fn), [this](nd::List l) {
            list = std::move(l);
            loading = false;
            loaded = true;
        });
    }
};

CardInfo album_card(const nd::Album& al) {
    CardInfo c;
    c.image = nd::cover_art_url(al.cover_art.empty() ? al.id : al.cover_art, 300);
    c.title = al.name;
    c.subtitle = al.artist.empty() ? (al.year > 0 ? std::to_string(al.year) : "") : al.artist;
    c.icon = ic::ALBUM;
    c.favorite = al.starred;
    if (al.song_count > 0) c.badge = util::fmt(tr("%d tracks"), al.song_count);
    return c;
}

CardInfo artist_card(const nd::Artist& art) {
    CardInfo c;
    c.image = nd::cover_art_url(art.cover_art.empty() ? art.id : art.cover_art, 300);
    c.title = art.name;
    c.subtitle = art.album_count > 0 ? util::fmt(tr("%d albums"), art.album_count) : "";
    c.icon = ic::PERSON;
    c.favorite = art.starred;
    return c;
}

CardInfo playlist_card(const nd::Playlist& pl) {
    CardInfo c;
    c.image = nd::cover_art_url(pl.cover_art.empty() ? pl.id : pl.cover_art, 300);
    c.title = pl.name;
    c.subtitle = pl.song_count > 0 ? util::fmt(tr("%d tracks"), pl.song_count) : "";
    c.icon = ic::QUEUE_MUSIC;
    return c;
}

void play_songs(const std::vector<nd::Song>& songs, int index, bool shuffle = false) {
    if (songs.empty()) return;
    std::vector<player::Source> q;
    q.reserve(songs.size());
    for (const auto& s : songs) {
        q.push_back(nd::make_source(s));
    }
    if (shuffle && q.size() > 1) {
        player::Source first = q[std::clamp(index, 0, (int)q.size() - 1)];
        q.erase(q.begin() + std::clamp(index, 0, (int)q.size() - 1));
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(q.begin(), q.end(), g);
        q.insert(q.begin(), std::move(first));
        index = 0;
    }
    screens::play_audio_queue(std::move(q), std::clamp(index, 0, (int)songs.size() - 1));
}

// --- Sign-in / Server Connection Screen Component ---

class ConnectView {
public:
    enum Step { SERVER, CREDENTIALS };

    void frame(float x0, const std::function<void()>& on_signed_in) {
        const Theme& t = theme();
        Id g = id("nd_connect");
        Rect card(x0, 100, std::min(700.0f, W - x0 - 60), 440);
        gfx::fill_rrect(card, 24, t.surface);

        float cx = card.x + 48, cy = card.y + 44, cw = card.w - 96;

        if (busy_) {
            spinner(card.cx(), card.cy() - 20, 24, t.accent);
            text::draw(font::label, card.cx(), card.cy() + 25, busy_label_.c_str(), t.text2, text::CENTER);
            return;
        }

        switch (step_) {
            case SERVER: {
                text::draw(font::title, cx, cy, tr("Connect to Navidrome"), t.text);
                text::draw_wrapped(font::body, Rect(cx, cy + 44, cw, 60),
                                   tr("Enter the server address, e.g. 192.168.1.50:4533 or music.example.com."),
                                   t.text2, 2);
                if (value_row(id(g, "addr"), Rect(cx, cy + 120, cw, 64), tr("Server"),
                              url_.empty() ? tr("Enter address") : url_.c_str(), ic::DNS, g)) {
                    prompt_text(tr("Navidrome server address"), url_, "192.168.1.50:4533",
                                [this](std::string v) { url_ = v; }, false, true);
                }
                float bw = std::max(220.0f, measure_button(tr("Continue"), ic::ARROW_FORWARD));
                if (button(id(g, "continue"), Rect(cx, cy + 220, bw, 54), tr("Continue"), ic::ARROW_FORWARD,
                           BTN_PRIMARY, g, url_.empty() ? 0 : F_DEFAULT) && !url_.empty()) {
                    check_server();
                }
                if (saved_ && button(id(g, "saved"), Rect(cx + bw + 16, cy + 220,
                                                          std::max(200.0f, measure_button(tr("Saved accounts"), ic::PERSON)), 54),
                                     tr("Saved accounts"), ic::PERSON, BTN_GHOST, g)) {
                    navidrome_account_menu();
                }
                break;
            }
            case CREDENTIALS: {
                text::draw(font::title, cx, cy, server_name_.empty() ? tr("Navidrome Server") : server_name_, t.text);
                text::draw(font::small, cx, cy + 40, server_url_, t.text3);

                if (value_row(id(g, "user"), Rect(cx, cy + 80, cw, 64), tr("Username"),
                              user_.empty() ? tr("Enter username") : user_.c_str(), ic::PERSON, g)) {
                    prompt_text(tr("Username"), user_, tr("Username"), [this](std::string v) { user_ = v; });
                }

                std::string masked = pass_.empty() ? tr("Enter password") : std::string(pass_.size(), '*');
                if (value_row(id(g, "pass"), Rect(cx, cy + 156, cw, 64), tr("Password"), masked.c_str(), ic::LOCK, g)) {
                    prompt_text(tr("Password"), "", tr("Password"), [this](std::string v) { pass_ = v; }, true);
                }

                float sw = std::max(200.0f, measure_button(tr("Sign in"), ic::ARROW_FORWARD));
                if (button(id(g, "signin"), Rect(cx, cy + 250, sw, 54), tr("Sign in"), ic::ARROW_FORWARD,
                           BTN_PRIMARY, g, user_.empty() ? 0 : F_DEFAULT) && !user_.empty()) {
                    sign_in(on_signed_in);
                }
                if (button(id(g, "back"), Rect(cx + sw + 16, cy + 250,
                                               std::max(160.0f, measure_button(tr("Back"), ic::ARROW_BACK)), 54),
                           tr("Back"), ic::ARROW_BACK, BTN_GHOST, g)) {
                    step_ = SERVER;
                }
                break;
            }
        }
    }

    bool on_back() {
        if (busy_) return true;
        if (step_ != SERVER) {
            step_ = SERVER;
            return true;
        }
        return false;
    }

    void prefill(const std::string& url) {
        if (url_.empty()) url_ = url;
    }

    void reset(const std::string& url) {
        scope_.reset();
        busy_ = false;
        step_ = SERVER;
        url_ = url;
        user_.clear();
        pass_.clear();
    }

    void set_saved(bool saved) { saved_ = saved; }

private:
    void check_server() {
        server_url_ = nd::normalize_url(url_);
        busy_ = true;
        busy_label_ = tr("Connecting\xE2\x80\xA6");
        std::string u = server_url_;
        scope_.run<nd::ServerInfo>([u] { return nd::server_info(u); }, [this](nd::ServerInfo info) {
            busy_ = false;
            if (!info.ok) {
                toast(util::fmt(tr("Couldn't connect: %s"), info.error.c_str()), ic::ERROR_OUTLINE, theme().bad);
                return;
            }
            server_name_ = info.name;
            step_ = CREDENTIALS;
            reset_focus();
        });
    }

    void sign_in(const std::function<void()>& on_signed_in) {
        busy_ = true;
        busy_label_ = tr("Signing in\xE2\x80\xA6");
        std::string u = server_url_, user = user_, pass = pass_;
        scope_.run<nd::AuthResult>([u, user, pass] { return nd::sign_in(u, user, pass); },
                                   [this, on_signed_in](nd::AuthResult r) {
            busy_ = false;
            if (!r.ok) {
                toast(r.error, ic::ERROR_OUTLINE, theme().bad);
                return;
            }
            toast(tr("Signed in to Navidrome"), ic::CHECK);
            if (on_signed_in) on_signed_in();
        });
    }

    Step step_ = SERVER;
    std::string url_, server_url_, server_name_;
    std::string user_, pass_;
    bool busy_ = false, saved_ = false;
    std::string busy_label_;
    tasks::Scope scope_;
};

// --- Album Details Screen ---

class AlbumScreen : public app::Screen {
public:
    AlbumScreen(nd::Album al) : album_(std::move(al)) {
        album_loader_.load([id = album_.id] { return nd::get_album(id); });
    }

    app::Section section() const override { return app::SEC_NAVIDROME; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("nd_album");
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        // Header info
        float cover_size = 200;
        Rect cr(x0, y, cover_size, cover_size);
        std::string cover = nd::cover_art_url(album_.cover_art.empty() ? album_.id : album_.cover_art, 500);
        if (!cover.empty()) {
            if (const images::Image* img = images::get(cover, 500, 500)) {
                if (img->ready) gfx::image_cover(img->tex, img->w, img->h, cr, 16);
            }
        }
        if (cover.empty()) {
            gfx::fill_rrect(cr, 16, t.surface);
            text::icon(ic::ALBUM, 64, cr.cx(), cr.cy(), t.text2);
        }

        float meta_x = x0 + cover_size + 30;
        float my = y + 10;
        text::draw_fit(font::title, meta_x, my, W - meta_x - 60, album_.name, t.text);
        my += 40;
        text::draw_fit(font::body_bold, meta_x, my, W - meta_x - 60, album_.artist, t.accent);
        my += 34;

        std::string info_line;
        if (album_.year > 0) info_line += std::to_string(album_.year);
        if (!album_.genre.empty()) {
            if (!info_line.empty()) info_line += " \xC2\xB7 ";
            info_line += album_.genre;
        }
        if (album_.song_count > 0) {
            if (!info_line.empty()) info_line += " \xC2\xB7 ";
            info_line += util::fmt(tr("%d songs"), album_.song_count);
        }
        if (album_.duration > 0) {
            if (!info_line.empty()) info_line += " \xC2\xB7 ";
            info_line += util::format_duration(album_.duration);
        }
        text::draw_fit(font::small, meta_x, my, W - meta_x - 60, info_line, t.text2);
        my += 48;

        // Action Buttons
        Id act = id(g, "actions");
        float bx = meta_x;
        float play_w = std::max(160.0f, measure_button(tr("Play"), ic::PLAY));
        if (button(id(act, "play"), Rect(bx, my, play_w, 46), tr("Play"), ic::PLAY, BTN_PRIMARY, act, F_DEFAULT)) {
            play_songs(album_loader_.list.songs, 0, false);
        }
        bx += play_w + 14;

        float shuf_w = std::max(160.0f, measure_button(tr("Shuffle"), ic::SHUFFLE));
        if (button(id(act, "shuffle"), Rect(bx, my, shuf_w, 46), tr("Shuffle"), ic::SHUFFLE, BTN_NORMAL, act)) {
            play_songs(album_loader_.list.songs, 0, true);
        }
        bx += shuf_w + 14;

        if (icon_button(id(act, "star"), bx + 23, my + 23, 23, album_.starred ? ic::FAVORITE : ic::FAVORITE_BORDER,
                        act, 0, album_.starred)) {
            album_.starred = !album_.starred;
            if (album_.starred) nd::star(album_.id, true, false);
            else nd::unstar(album_.id, true, false);
        }

        y += cover_size + 40;

        // Tracklist
        text::draw(font::title, x0, y, tr("Tracks"), t.text);
        y += 48;

        if (!album_loader_.loaded) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else if (album_loader_.list.songs.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 160), ic::MUSIC, tr("No tracks found"), "");
            y += 180;
        } else {
            Id tg = id(g, "tracks");
            for (size_t i = 0; i < album_loader_.list.songs.size(); i++) {
                const nd::Song& s = album_loader_.list.songs[i];
                Rect r(x0, y, W - x0 - 60, 52);
                Id tid = id(tg, (int64_t)i);
                Item it = focusable(tid, r, tg);
                if (it.focused) page_.focus_range(r.y + page_.scroll() - 10, r.b() + page_.scroll() + 10);
                Rect br = r.offset(bump_x(tid), bump_y(tid));
                gfx::fill_rrect(br, 12, gfx::lerp(Color(255, 255, 255, i % 2 ? 6 : 14), t.surface_focus, it.f));
                Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
                Color fg2 = gfx::lerp(t.text3, Color(21, 18, 26, 160), it.f);

                text::draw(font::body_bold, br.x + 30, br.cy() - 12,
                           std::to_string(s.track > 0 ? s.track : (int)i + 1), fg2, text::CENTER);
                text::draw_fit(font::body, br.x + 64, br.cy() - 12, br.w - 180, s.title, fg);
                text::draw(font::small_bold, br.r() - 20, br.cy() - 10,
                           util::format_duration(s.duration), fg2, text::RIGHT);

                if (it.clicked) play_songs(album_loader_.list.songs, (int)i, false);
                y += 58;
            }
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    nd::Album album_;
    Loader album_loader_;
    Page page_;
};

// --- Artist Details Screen ---

class ArtistScreen : public app::Screen {
public:
    ArtistScreen(nd::Artist art) : artist_(std::move(art)) {
        artist_loader_.load([id = artist_.id] { return nd::get_artist(id); });
    }

    app::Section section() const override { return app::SEC_NAVIDROME; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("nd_artist");
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw(font::display, x0, y, artist_.name, t.text);
        y += 60;
        if (artist_.album_count > 0) {
            text::draw(font::small, x0, y, util::fmt(tr("%d albums"), artist_.album_count), t.text2);
            y += 40;
        }

        text::draw(font::title, x0, y, tr("Albums"), t.text);
        y += 48;

        if (!artist_loader_.loaded) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else if (artist_loader_.list.albums.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 160), ic::ALBUM, tr("No albums found"), "");
            y += 180;
        } else {
            ShelfSpec s;
            s.title_str = "";
            s.count = (int)artist_loader_.list.albums.size();
            s.shape = CARD_SQUARE;
            s.item_w = 180;
            s.item = [this](int i) { return album_card(artist_loader_.list.albums[i]); };
            s.on_click = [this](int i) {
                app::push(make_album_screen(artist_loader_.list.albums[i]));
            };
            y += shelf(id(g, "albums"), x0, y, s, &page_) + 16;
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    nd::Artist artist_;
    Loader artist_loader_;
    Page page_;
};

// --- Playlist Details Screen ---

class PlaylistScreen : public app::Screen {
public:
    PlaylistScreen(nd::Playlist pl) : playlist_(std::move(pl)) {
        playlist_loader_.load([id = playlist_.id] { return nd::get_playlist(id); });
    }

    app::Section section() const override { return app::SEC_NAVIDROME; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("nd_playlist");
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw(font::display, x0, y, playlist_.name, t.text);
        y += 60;

        std::string sub = util::fmt(tr("%d tracks"), playlist_.song_count);
        if (playlist_.duration > 0) sub += " \xC2\xB7 " + util::format_duration(playlist_.duration);
        text::draw(font::small, x0, y, sub, t.text2);
        y += 44;

        Id act = id(g, "actions");
        float play_w = std::max(160.0f, measure_button(tr("Play All"), ic::PLAY));
        if (button(id(act, "play"), Rect(x0, y, play_w, 46), tr("Play All"), ic::PLAY, BTN_PRIMARY, act, F_DEFAULT)) {
            play_songs(playlist_loader_.list.songs, 0, false);
        }
        float shuf_w = std::max(160.0f, measure_button(tr("Shuffle"), ic::SHUFFLE));
        if (button(id(act, "shuffle"), Rect(x0 + play_w + 14, y, shuf_w, 46), tr("Shuffle"), ic::SHUFFLE, BTN_NORMAL, act)) {
            play_songs(playlist_loader_.list.songs, 0, true);
        }
        y += 66;

        text::draw(font::title, x0, y, tr("Tracks"), t.text);
        y += 48;

        if (!playlist_loader_.loaded) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else if (playlist_loader_.list.songs.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 160), ic::MUSIC, tr("Playlist is empty"), "");
            y += 180;
        } else {
            Id tg = id(g, "tracks");
            for (size_t i = 0; i < playlist_loader_.list.songs.size(); i++) {
                const nd::Song& s = playlist_loader_.list.songs[i];
                Rect r(x0, y, W - x0 - 60, 52);
                Id tid = id(tg, (int64_t)i);
                Item it = focusable(tid, r, tg);
                if (it.focused) page_.focus_range(r.y + page_.scroll() - 10, r.b() + page_.scroll() + 10);
                Rect br = r.offset(bump_x(tid), bump_y(tid));
                gfx::fill_rrect(br, 12, gfx::lerp(Color(255, 255, 255, i % 2 ? 6 : 14), t.surface_focus, it.f));
                Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
                Color fg2 = gfx::lerp(t.text3, Color(21, 18, 26, 160), it.f);

                text::draw(font::body_bold, br.x + 30, br.cy() - 12, std::to_string((int)i + 1), fg2, text::CENTER);
                std::string title_line = s.title;
                if (!s.artist.empty()) title_line += " \xC2\xB7 " + s.artist;
                text::draw_fit(font::body, br.x + 64, br.cy() - 12, br.w - 180, title_line, fg);
                text::draw(font::small_bold, br.r() - 20, br.cy() - 10, util::format_duration(s.duration), fg2, text::RIGHT);

                if (it.clicked) play_songs(playlist_loader_.list.songs, (int)i, false);
                y += 58;
            }
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    nd::Playlist playlist_;
    Loader playlist_loader_;
    Page page_;
};

// --- Dedicated Navidrome Search Screen ---

class NavidromeSearchScreen : public app::Screen {
public:
    NavidromeSearchScreen(std::string query) : query_(std::move(query)) {
        if (!query_.empty()) run_search();
    }

    app::Section section() const override { return app::SEC_NAVIDROME; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("nd_search");
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw(font::display, x0, y, tr("Search Navidrome"), t.text);
        y += 74;

        Id top = id(g, "top");
        if (search_bar(id(top, "bar"), Rect(x0, y, W - x0 - 60, 56), query_,
                       tr("Search songs, albums, artists\xE2\x80\xA6"), top, F_DEFAULT)) {
            prompt_text(tr("Search Navidrome"), query_, tr("Song, album or artist"), [this](std::string q) {
                if (!q.empty()) {
                    query_ = q;
                    run_search();
                }
            });
        }
        y += 84;

        if (loader_.loading) {
            loading_indicator(x0 + 40, y + 20);
            y += 80;
        } else if (loader_.loaded && loader_.list.total == 0) {
            empty_state(Rect(x0, y, W - x0 - 60, 200), ic::SEARCH, tr("No results found"), tr("Try searching for something else."));
            y += 220;
        } else if (loader_.loaded) {
            // Songs
            if (!loader_.list.songs.empty()) {
                ShelfSpec s;
                s.title = tr("Songs");
                s.count = (int)loader_.list.songs.size();
                s.shape = CARD_WIDE;
                s.item_w = 280;
                s.item = [this](int i) {
                    const nd::Song& sg = loader_.list.songs[i];
                    CardInfo c;
                    c.image = nd::cover_art_url(sg.cover_art.empty() ? sg.album_id : sg.cover_art, 300);
                    c.title = sg.title;
                    c.subtitle = sg.artist.empty() ? sg.album : sg.artist;
                    c.icon = ic::MUSIC;
                    c.badge = util::format_duration(sg.duration);
                    return c;
                };
                s.on_click = [this](int i) {
                    screens::play_audio(nd::make_source(loader_.list.songs[i]));
                };
                y += shelf(id(g, "songs"), x0, y, s, &page_) + 14;
            }

            // Albums
            if (!loader_.list.albums.empty()) {
                ShelfSpec s;
                s.title = tr("Albums");
                s.count = (int)loader_.list.albums.size();
                s.shape = CARD_SQUARE;
                s.item_w = 180;
                s.item = [this](int i) { return album_card(loader_.list.albums[i]); };
                s.on_click = [this](int i) {
                    app::push(make_album_screen(loader_.list.albums[i]));
                };
                y += shelf(id(g, "albums"), x0, y, s, &page_) + 14;
            }

            // Artists
            if (!loader_.list.artists.empty()) {
                ShelfSpec s;
                s.title = tr("Artists");
                s.count = (int)loader_.list.artists.size();
                s.shape = CARD_SQUARE;
                s.item_w = 180;
                s.item = [this](int i) { return artist_card(loader_.list.artists[i]); };
                s.on_click = [this](int i) {
                    app::push(make_artist_screen(loader_.list.artists[i]));
                };
                y += shelf(id(g, "artists"), x0, y, s, &page_) + 14;
            }
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    void run_search() {
        std::string q = query_;
        loader_.load([q] { return nd::search(q, 30); });
    }

    std::string query_;
    Loader loader_;
    Page page_;
};

// --- Main Navidrome Dashboard Screen ---

class NavidromeScreen : public app::Screen {
public:
    enum Tab { TAB_ALBUMS, TAB_ARTISTS, TAB_PLAYLISTS };

    NavidromeScreen() : version_(nd::version()) {
        connect_.prefill(nd::account().server);
        connect_.set_saved(!nd::others().empty());
        if (nd::signed_in()) load();
    }

    app::Section section() const override { return app::SEC_NAVIDROME; }

    bool on_back() override {
        return !nd::signed_in() && connect_.on_back();
    }

    void on_enter() override {
        if (nd::signed_in() && nd::version() != version_) account_changed();
    }

    void frame() override {
        float x0 = content_x();
        if (nd::version() != version_) account_changed();

        if (!nd::signed_in()) {
            connect_.frame(x0, [this] { account_changed(); });
            return;
        }

        const Theme& t = theme();
        Id g = id("nd_home");
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw(font::display, x0, y, "Navidrome", t.text);
        const nd::Account& a = nd::account();
        std::string who = a.user_name + " \xC2\xB7 " + (a.server_name.empty() ? a.server : a.server_name);
        text::draw_fit(font::small, x0 + 4, y + 60, 400, who, t.text3);

        Id top = id(g, "top");
        float bx = W - 60 - 28;
        if (icon_button(id(top, "account"), bx, y + 32, 28, ic::ACCOUNT_CIRCLE, top, 0, true)) {
            navidrome_account_menu();
        }
        if (search_bar(id(top, "search"), Rect(x0 + 330, y + 4, bx - 28 - 24 - x0 - 330, 56), "",
                       tr("Search your music library"), top)) {
            prompt_text(tr("Search Navidrome"), "", tr("Song, album or artist"), [](std::string q) {
                if (!q.empty()) app::push(make_navidrome_search(q));
            });
        }
        y += 96;

        // Tabs
        float cx = x0;
        const struct { Tab tab; const char* name; int icon; } TABS[] = {
            {TAB_ALBUMS, N_("Albums"), ic::ALBUM},
            {TAB_ARTISTS, N_("Artists"), ic::PERSON},
            {TAB_PLAYLISTS, N_("Playlists"), ic::QUEUE_MUSIC},
        };
        for (const auto& item : TABS) {
            float w = text::measure(font::small_bold, tr(item.name)) + 60;
            if (chip(id(top, (int64_t)item.tab), Rect(cx, y, w, 42), tr(item.name),
                     tab_ == item.tab, top, item.icon)) {
                tab_ = item.tab;
                load_tab();
            }
            cx += w + 14;
        }
        y += 70;

        switch (tab_) {
            case TAB_ALBUMS: {
                y += album_shelf(id(g, "recent"), x0, y, tr("Recently Added"), recent_);
                y += album_shelf(id(g, "frequent"), x0, y, tr("Most Played"), frequent_);
                y += album_shelf(id(g, "random"), x0, y, tr("Random Mix"), random_);
                y += album_shelf(id(g, "starred"), x0, y, tr("Favorite Albums"), starred_);
                break;
            }
            case TAB_ARTISTS: {
                if (!artists_.loaded) {
                    loading_indicator(x0 + 40, y + 20);
                    y += 80;
                } else if (artists_.list.artists.empty()) {
                    empty_state(Rect(x0, y, W - x0 - 60, 200), ic::PERSON, tr("No artists found"), "");
                    y += 220;
                } else {
                    GridSpec s;
                    s.count = (int)artists_.list.artists.size();
                    s.cols = 5;
                    s.shape = CARD_SQUARE;
                    s.item_w = 175;
                    s.item = [this](int i) { return artist_card(artists_.list.artists[i]); };
                    s.on_click = [this](int i) {
                        app::push(make_artist_screen(artists_.list.artists[i]));
                    };
                    y += grid(id(g, "art_grid"), x0, y, s, &page_) + 16;
                }
                break;
            }
            case TAB_PLAYLISTS: {
                if (!playlists_.loaded) {
                    loading_indicator(x0 + 40, y + 20);
                    y += 80;
                } else if (playlists_.list.playlists.empty()) {
                    empty_state(Rect(x0, y, W - x0 - 60, 200), ic::QUEUE_MUSIC, tr("No playlists found"), "");
                    y += 220;
                } else {
                    GridSpec s;
                    s.count = (int)playlists_.list.playlists.size();
                    s.cols = 4;
                    s.shape = CARD_SQUARE;
                    s.item_w = 220;
                    s.item = [this](int i) { return playlist_card(playlists_.list.playlists[i]); };
                    s.on_click = [this](int i) {
                        app::push(make_playlist_screen(playlists_.list.playlists[i]));
                    };
                    y += grid(id(g, "pl_grid"), x0, y, s, &page_) + 16;
                }
                break;
            }
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    float album_shelf(Id sid, float x, float y, const char* title, Loader& l) {
        if (l.loaded && l.list.albums.empty()) return 0;
        ShelfSpec s;
        s.title_str = title;
        s.count = (int)l.list.albums.size();
        s.loading = !l.loaded;
        s.shape = CARD_SQUARE;
        s.item_w = 180;
        s.item = [&l](int i) { return album_card(l.list.albums[i]); };
        s.on_click = [&l](int i) {
            app::push(make_album_screen(l.list.albums[i]));
        };
        return shelf(sid, x, y, s, &page_) + 16;
    }

    void account_changed() {
        version_ = nd::version();
        connect_.reset(nd::account().server);
        connect_.set_saved(!nd::others().empty());
        recent_.scope.reset(); recent_.loading = recent_.loaded = false;
        frequent_.scope.reset(); frequent_.loading = frequent_.loaded = false;
        random_.scope.reset(); random_.loading = random_.loaded = false;
        starred_.scope.reset(); starred_.loading = starred_.loaded = false;
        artists_.scope.reset(); artists_.loading = artists_.loaded = false;
        playlists_.scope.reset(); playlists_.loading = playlists_.loaded = false;
        if (nd::signed_in()) load();
        reset_focus();
    }

    void load_tab() {
        if (tab_ == TAB_ARTISTS && !artists_.loaded && !artists_.loading) {
            artists_.load([] { return nd::get_artists(); });
        } else if (tab_ == TAB_PLAYLISTS && !playlists_.loaded && !playlists_.loading) {
            playlists_.load([] { return nd::get_playlists(); });
        }
    }

    void load() {
        recent_.load([] { return nd::get_album_list("recent", 20); });
        frequent_.load([] { return nd::get_album_list("frequent", 20); });
        random_.load([] { return nd::get_album_list("random", 20); });
        starred_.load([] { return nd::get_album_list("starred", 20); });
        load_tab();
    }

    int version_ = 0;
    Tab tab_ = TAB_ALBUMS;
    ConnectView connect_;
    Loader recent_, frequent_, random_, starred_, artists_, playlists_;
    Page page_;
};

std::unique_ptr<app::Screen> make_album_screen(const nd::Album& al) { return std::make_unique<AlbumScreen>(al); }
std::unique_ptr<app::Screen> make_artist_screen(const nd::Artist& art) { return std::make_unique<ArtistScreen>(art); }
std::unique_ptr<app::Screen> make_playlist_screen(const nd::Playlist& pl) { return std::make_unique<PlaylistScreen>(pl); }

}  // namespace

void navidrome_account_menu() {
    const nd::Account a = nd::account();
    std::vector<nd::Account> others = nd::others();
    if (!a.valid() && others.empty()) {
        app::open_section(app::SEC_NAVIDROME);
        return;
    }
    auto server = [](const nd::Account& o) { return o.server_name.empty() ? o.server : o.server_name; };
    bool servers = false;
    for (auto& o : others) servers |= o.server != (a.valid() ? a.server : others[0].server);

    std::vector<MenuItem> items;
    for (size_t i = 0; i < others.size(); i++) {
        std::string name = servers ? util::fmt(tr("%s on %s"), others[i].user_name.c_str(), server(others[i]).c_str())
                                   : others[i].user_name;
        items.push_back({util::fmt(tr("Switch to %s"), name.c_str()), ic::PERSON, [i] {
            nd::switch_to(i);
            toast(util::fmt(tr("Now using %s"), nd::account().user_name.c_str()), ic::ACCOUNT_CIRCLE);
        }});
    }

    items.push_back({tr("Add another account"), ic::PERSON_ADD, [n = others.size()] {
        if (n >= store::MAX_SAVED_ACCOUNTS) {
            toast(tr("You can keep up to 5 accounts. Switch to one and sign out to make room."), ic::INFO);
            return;
        }
        nd::add_account();
        app::open_section(app::SEC_NAVIDROME);
    }});

    if (a.valid()) {
        items.push_back({tr("Sign out"), ic::LOGOUT, [] {
            nd::sign_out();
            toast(tr("Signed out of Navidrome"), ic::LOGOUT);
        }});
    }

    show_menu(a.valid() ? a.user_name : tr("Navidrome accounts"),
              a.valid() ? server(a) : tr("Not signed in"), std::move(items));
}

std::unique_ptr<app::Screen> make_navidrome() { return std::make_unique<NavidromeScreen>(); }
std::unique_ptr<app::Screen> make_navidrome_search(const std::string& query) {
    return std::make_unique<NavidromeSearchScreen>(query);
}

}  // namespace screens
