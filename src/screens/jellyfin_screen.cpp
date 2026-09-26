// Jellyfin: sign-in flow, home shelves, libraries and item details.
#include <cmath>

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
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace jf = jellyfin;

std::unique_ptr<app::Screen> make_detail(const jf::Item& it);
std::unique_ptr<app::Screen> make_library(const jf::Item& view);

// Loads a jellyfin::List in the background.
struct Loader {
    jf::List list;
    bool loading = false, loaded = false;
    tasks::Scope scope;

    void load(std::function<jf::List()> fn) {
        scope.reset();
        loading = true;
        scope.run<jf::List>(std::move(fn), [this](jf::List l) {
            list = std::move(l);
            loading = false;
            loaded = true;
        });
    }
};

std::string episode_label(const jf::Item& it) {
    if (it.type != "Episode") return it.year;
    return util::fmt(tr("S%d E%d \xC2\xB7 %s"), it.parent_index, it.index, it.series_name.c_str());
}

CardInfo poster_card(const jf::Item& it) {
    CardInfo c;
    c.image = jf::poster(it, 300);
    c.image_w = 300;
    c.title = it.name;
    c.subtitle = it.type == "Series" && it.unplayed > 0 ? util::fmt(tr("%d unwatched"), it.unplayed) : it.year;
    c.icon = it.type == "MusicAlbum" ? ic::ALBUM : ic::MOVIE;
    c.favorite = it.favorite;
    if (it.position > 0 && it.runtime > 0) c.progress = (float)(it.position / it.runtime);
    return c;
}

CardInfo thumb_card(const jf::Item& it) {
    CardInfo c;
    c.image = jf::thumb(it, 480);
    c.image_w = 400;
    c.title = it.type == "Episode" ? it.series_name : it.name;
    c.subtitle = it.type == "Episode" ? util::fmt(tr("S%d E%d \xC2\xB7 %s"), it.parent_index, it.index, it.name.c_str()) : it.year;
    c.icon = ic::MOVIE;
    if (it.position > 0 && it.runtime > 0) c.progress = (float)(it.position / it.runtime);
    if (it.runtime > 0) c.badge = it.position > 0 ? util::fmt(tr("%s left"), util::format_duration(it.runtime - it.position).c_str())
                                                  : util::format_ticks_duration((int64_t)(it.runtime * 1e7));
    return c;
}

void open_item(const jf::Item& it) {
    if (it.type == "Movie" || it.type == "Series" || it.type == "MusicAlbum" || it.type == "Season" || it.type == "BoxSet") {
        app::push(make_detail(it));
    } else if (it.type == "Episode" || it.type == "Video" || it.type == "MusicVideo") {
        play_video(jf::make_source(it));
    } else if (it.is_audio()) {
        play_audio(jf::make_source(it));
    } else if (it.type == "CollectionFolder" || it.type == "UserView" || it.type == "Folder") {
        app::push(make_library(it));
    } else {
        app::push(make_detail(it));
    }
}

// ============================================================================
// Sign-in

class ConnectView {
public:
    enum Step { SERVER, METHOD, QUICK, PASSWORD };

    void frame(float x0, std::function<void()> on_signed_in) {
        const Theme& t = theme();
        Id g = id("jf_connect");
        float y = 60;
        text::draw(font::display, x0, y, "Jellyfin", t.text);
        text::draw(font::body, x0 + 2, y + 62, tr("Stream your own movies, shows and music from your Jellyfin server."), t.text2);
        y += 130;

        Rect card(x0, y, 720, 420);
        gfx::shadow(card, 30, Color(0, 0, 0, 120));
        gfx::fill_rrect(card, 26, Color(255, 255, 255, 14));
        float cx = card.x + 40, cw = card.w - 80;
        float cy = card.y + 36;

        if (busy_) {
            loading_indicator(card.cx(), card.cy() - 10, busy_label_.c_str());
            return;
        }

        switch (step_) {
            case SERVER: {
                text::draw(font::title, cx, cy, tr("Connect to your server"), t.text);
                text::draw_wrapped(font::body, Rect(cx, cy + 44, cw, 60),
                                   tr("Enter the address you use to open Jellyfin in a browser, e.g. 192.168.1.20 or jellyfin.example.com."),
                                   t.text2, 2);
                if (value_row(id(g, "addr"), Rect(cx, cy + 120, cw, 64), tr("Server"), url_.empty() ? tr("Enter address") : url_.c_str(),
                              ic::DNS, g))
                    prompt_text(tr("Jellyfin server address"), url_, "192.168.1.20:8096", [this](std::string v) { url_ = v; }, false, true);
                if (button(id(g, "continue"), Rect(cx, cy + 220, std::max(220.0f, measure_button(tr("Continue"), ic::ARROW_FORWARD)), 54), tr("Continue"), ic::ARROW_FORWARD, BTN_PRIMARY, g,
                           url_.empty() ? 0 : F_DEFAULT) && !url_.empty())
                    check_server();
                break;
            }
            case METHOD: {
                text::draw(font::title, cx, cy, server_name_.empty() ? tr("Jellyfin server") : server_name_, t.text);
                text::draw(font::small, cx, cy + 40, server_url_, t.text3);
                if (big_option(id(g, "qc"), Rect(cx, cy + 90, cw, 100), ic::KEY, tr("Use Quick Connect"),
                               tr("Approve it from your phone, nothing to type"), g, true))
                    start_quick_connect();
                if (big_option(id(g, "pw"), Rect(cx, cy + 206, cw, 100), ic::PERSON, tr("Sign in with password"),
                               tr("Enter your username and password"), g, false))
                    step_ = PASSWORD;
                if (button(id(g, "change"), Rect(cx, cy + 322, std::max(220.0f, measure_button(tr("Change server"), ic::ARROW_BACK)), 46),
                           tr("Change server"), ic::ARROW_BACK, BTN_GHOST, g))
                    step_ = SERVER;
                break;
            }
            case QUICK: {
                text::draw(font::title, cx, cy, tr("Enter this code"), t.text);
                text::draw_wrapped(font::body, Rect(cx, cy + 44, cw, 60),
                                   tr("On your phone or computer, open Jellyfin \xE2\x86\x92 your profile \xE2\x86\x92 Quick Connect, and enter:"),
                                   t.text2, 2);
                float tile = 70, gap = 14;
                float total = code_.size() * (tile + gap) - gap;
                float tx = card.cx() - total * 0.5f;
                for (size_t i = 0; i < code_.size(); i++) {
                    float bob = std::sin((float)ui::time() * 3 + i * 0.6f) * 3;
                    Rect r(tx + i * (tile + gap), cy + 130 + bob, tile, 90);
                    gfx::shadow(r, 16, Color(0, 0, 0, 120));
                    gfx::fill_rrect_vgrad(r, 16, t.accent, t.accent2);
                    text::draw(text::font(text::BOLD, 50), r.cx(), r.y + 14, std::string(1, code_[i]), gfx::rgb(0x1A1016), text::CENTER);
                }
                const char* waiting = tr("Waiting for approval\xE2\x80\xA6");
                float sx = card.cx() - (30 + text::measure(font::small_bold, waiting)) * 0.5f;
                spinner(sx + 10, cy + 275, 10, t.accent, 3);
                text::draw(font::small_bold, sx + 30, cy + 264, waiting, t.text2);
                float bw = std::max(180.0f, measure_button(tr("Cancel"), ic::CLOSE));
                if (button(id(g, "cancel"), Rect(card.cx() - bw * 0.5f, cy + 310, bw, 46), tr("Cancel"), ic::CLOSE, BTN_NORMAL, g, F_DEFAULT)) {
                    poll_.reset();
                    step_ = METHOD;
                }
                poll_quick(on_signed_in);
                break;
            }
            case PASSWORD: {
                text::draw(font::title, cx, cy, tr("Sign in"), t.text);
                if (value_row(id(g, "user"), Rect(cx, cy + 60, cw, 64), tr("Username"), user_.empty() ? tr("Enter") : user_.c_str(),
                              ic::PERSON, g))
                    prompt_text(tr("Username"), user_, tr("Username"), [this](std::string v) { user_ = v; });
                std::string masked = pass_.empty() ? tr("Enter") : std::string(pass_.size(), '*');
                if (value_row(id(g, "pass"), Rect(cx, cy + 136, cw, 64), tr("Password"), masked.c_str(), ic::LOCK, g))
                    prompt_text(tr("Password"), "", tr("Password"), [this](std::string v) { pass_ = v; }, true);
                float sw = std::max(200.0f, measure_button(tr("Sign in"), ic::ARROW_FORWARD));
                if (button(id(g, "signin"), Rect(cx, cy + 230, sw, 54), tr("Sign in"), ic::ARROW_FORWARD, BTN_PRIMARY, g,
                           user_.empty() ? 0 : F_DEFAULT) && !user_.empty())
                    sign_in(on_signed_in);
                if (button(id(g, "back"), Rect(cx + sw + 16, cy + 230, std::max(160.0f, measure_button(tr("Back"), ic::ARROW_BACK)), 54), tr("Back"),
                           ic::ARROW_BACK, BTN_GHOST, g))
                    step_ = METHOD;
                break;
            }
        }
    }

    bool on_back() {
        if (busy_) return true;
        if (step_ == QUICK) poll_.reset();
        if (step_ != SERVER) {
            step_ = step_ == PASSWORD || step_ == QUICK ? METHOD : SERVER;
            return true;
        }
        return false;
    }

    void prefill(const std::string& url) {
        if (url_.empty()) url_ = url;
    }

private:
    bool big_option(Id iid, const Rect& r, int icon, const char* title, const char* desc, Id g, bool def) {
        const Theme& t = theme();
        Item it = focusable(iid, r, g, def ? F_DEFAULT : 0);
        Rect br = r.scaled(1 + 0.03f * it.f).offset(bump_x(iid), bump_y(iid));
        if (it.f > 0.01f) gfx::shadow(br, 18, Color(0, 0, 0, (uint8_t)(110 * it.f)));
        gfx::fill_rrect(br, 18, gfx::lerp(t.surface_hi, t.surface_focus, it.f));
        Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f), fg2 = gfx::lerp(t.text2, Color(21, 18, 26, 170), it.f);
        gfx::fill_rrect_hgrad(Rect(br.x + 22, br.cy() - 28, 56, 56), 16, t.accent, t.accent2);
        text::icon(icon, 30, br.x + 50, br.cy(), gfx::rgb(0x1A1016));
        text::draw(font::label, br.x + 98, br.y + 24, title, fg);
        text::draw_fit(font::small, br.x + 98, br.y + 56, br.w - 120, desc, fg2);
        return it.clicked;
    }

    void check_server() {
        server_url_ = jf::normalize_url(url_);
        busy_ = true;
        busy_label_ = tr("Connecting\xE2\x80\xA6");
        std::string u = server_url_;
        scope_.run<jf::ServerInfo>([u] { return jf::server_info(u); }, [this](jf::ServerInfo info) {
            busy_ = false;
            if (!info.ok) {
                toast(util::fmt(tr("Couldn't connect: %s"), info.error.c_str()), ic::ERROR_OUTLINE, theme().bad);
                return;
            }
            server_name_ = info.name;
            step_ = METHOD;
            reset_focus();
        });
    }

    void start_quick_connect() {
        busy_ = true;
        busy_label_ = tr("Starting Quick Connect\xE2\x80\xA6");
        std::string u = server_url_;
        scope_.run<jf::QuickConnect>([u] { return jf::quick_connect_start(u); }, [this](jf::QuickConnect q) {
            busy_ = false;
            if (!q.ok) {
                toast(q.error, ic::ERROR_OUTLINE, theme().bad);
                return;
            }
            code_ = q.code;
            secret_ = q.secret;
            step_ = QUICK;
            next_poll_ = ui::time() + 2;
            reset_focus();
        });
    }

    void poll_quick(const std::function<void()>& on_signed_in) {
        if (polling_ || ui::time() < next_poll_) return;
        polling_ = true;
        std::string u = server_url_, sec = secret_;
        poll_.run<int>([u, sec] { return jf::quick_connect_poll(u, sec); }, [this, u, sec, on_signed_in](int st) {
            polling_ = false;
            next_poll_ = ui::time() + 2.5;
            if (st < 0) {
                toast(tr("The code expired, starting over"), ic::REFRESH);
                step_ = METHOD;
            } else if (st > 0) {
                busy_ = true;
                busy_label_ = tr("Signing in\xE2\x80\xA6");
                scope_.run<jf::AuthResult>([u, sec] { return jf::quick_connect_finish(u, sec); },
                                           [this, on_signed_in](jf::AuthResult r) { done(r, on_signed_in); });
            }
        });
    }

    void sign_in(const std::function<void()>& on_signed_in) {
        busy_ = true;
        busy_label_ = tr("Signing in\xE2\x80\xA6");
        std::string u = server_url_, user = user_, pass = pass_;
        scope_.run<jf::AuthResult>([u, user, pass] { return jf::sign_in(u, user, pass); },
                                   [this, on_signed_in](jf::AuthResult r) { done(r, on_signed_in); });
    }

    void done(const jf::AuthResult& r, const std::function<void()>& on_signed_in) {
        busy_ = false;
        if (!r.ok) {
            toast(r.error, ic::ERROR_OUTLINE, theme().bad);
            return;
        }
        pass_.clear();
        toast(util::fmt(tr("Signed in as %s"), jf::account().user_name.c_str()), ic::CHECK_CIRCLE, theme().good);
        on_signed_in();
    }

    Step step_ = SERVER;
    std::string url_, server_url_, server_name_, user_, pass_, code_, secret_;
    bool busy_ = false, polling_ = false;
    std::string busy_label_;
    double next_poll_ = 0;
    tasks::Scope scope_, poll_;
};

// ============================================================================
// Home

class JellyfinScreen : public app::Screen {
public:
    JellyfinScreen() {
        connect_.prefill(jf::account().server);
        if (jf::account().valid()) load();
    }

    app::Section section() const override { return app::SEC_JELLYFIN; }

    bool on_back() override { return !jf::account().valid() && connect_.on_back(); }

    void on_enter() override {
        // Refresh progress after watching something.
        if (jf::account().valid() && ui::time() - loaded_at_ > 2) {
            resume_.load([] { return jf::resume(); });
            next_up_.load([] { return jf::next_up(); });
        }
    }

    void frame() override {
        float x0 = content_x();
        if (!jf::account().valid()) {
            connect_.frame(x0, [this] {
                load();
                reset_focus();
            });
            return;
        }
        const Theme& t = theme();
        Id g = id("jf_home");
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw(font::display, x0, y, "Jellyfin", t.text);
        const jf::Account& a = jf::account();
        std::string who = a.user_name + " \xC2\xB7 " + (a.server_name.empty() ? a.server : a.server_name);
        text::draw_fit(font::small, x0 + 4, y + 60, 400, who, t.text3);
        Id top = id(g, "top");
        if (search_bar(id(top, "search"), Rect(x0 + 330, y + 4, W - x0 - 390, 56), "", tr("Search your library"), top))
            prompt_text(tr("Search Jellyfin"), "", tr("Movie, show or album"), [](std::string q) {
                if (!q.empty()) app::push(make_jellyfin_search(q));
            });
        y += 96;

        // Libraries
        if (views_.loaded && views_.list.items.empty() && !views_.list.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::WIFI_OFF, tr("Can't reach your server"),
                                   views_.list.error.c_str()))
                load();
            page_.end(y + 330 + page_.scroll());
            return;
        }
        float cx = x0;
        for (size_t i = 0; i < views_.list.items.size(); i++) {
            const jf::Item& v = views_.list.items[i];
            int icon = v.collection_type == "movies" ? ic::MOVIE : v.collection_type == "tvshows" ? ic::TV
                     : v.collection_type == "music" ? ic::LIBRARY_MUSIC : v.collection_type == "livetv" ? ic::LIVE_TV
                                                                                                     : ic::VIDEO_LIBRARY;
            float w = text::measure(font::small_bold, v.name) + 60;
            if (chip(id(top, (int64_t)i), Rect(cx, y, w, 42), v.name.c_str(), false, top, icon)) app::push(make_library(v));
            cx += w + 12;
        }
        y += 72;
        if (focus_in_group(top)) page_.focus_range(0, y + page_.scroll());

        y += item_shelf(id(g, "resume"), x0, y, tr("Continue watching"), resume_, CARD_WIDE, 320) ;
        y += item_shelf(id(g, "nextup"), x0, y, tr("Next up"), next_up_, CARD_WIDE, 320);
        for (size_t i = 0; i < latest_.size(); i++) {
            auto& [name, loader, music] = latest_[i];
            if (y < H + 400 && !loader->loaded && !loader->loading) {
                std::string pid = latest_ids_[i];
                loader->load([pid] { return jf::latest(pid); });
            }
            y += item_shelf(id(g, (int64_t)(50 + i)), x0, y, util::fmt(tr("Latest %s"), name.c_str()).c_str(), *loader,
                            music ? CARD_SQUARE : CARD_POSTER, music ? 190 : 170);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    float item_shelf(Id sid, float x, float y, const char* title, Loader& l, CardShape shape, float w) {
        if (l.loaded && l.list.items.empty()) return 0;
        ShelfSpec s;
        s.title_str = title;
        s.count = (int)l.list.items.size();
        s.loading = !l.loaded;
        s.shape = shape;
        s.item_w = w;
        s.item = [&l, shape](int i) { return shape == CARD_WIDE ? thumb_card(l.list.items[i]) : poster_card(l.list.items[i]); };
        s.on_click = [&l](int i) { open_item(l.list.items[i]); };
        s.on_focus = [&l](int i) { set_backdrop(jf::backdrop(l.list.items[i], 1280)); };
        return shelf(sid, x, y, s, &page_) + 14;
    }

    void load() {
        loaded_at_ = ui::time();
        resume_.load([] { return jf::resume(); });
        next_up_.load([] { return jf::next_up(); });
        views_.scope.reset();
        views_.loading = true;
        views_.scope.run<jf::List>([] { return jf::views(); }, [this](jf::List l) {
            views_.list = std::move(l);
            views_.loading = false;
            views_.loaded = true;
            latest_.clear();
            latest_ids_.clear();
            for (auto& v : views_.list.items) {
                if (v.collection_type == "playlists" || v.collection_type == "livetv" || v.collection_type == "boxsets") continue;
                latest_.emplace_back(v.name, std::make_unique<Loader>(), v.collection_type == "music");
                latest_ids_.push_back(v.id);
            }
        });
    }

    ConnectView connect_;
    Loader views_, resume_, next_up_;
    std::vector<std::tuple<std::string, std::unique_ptr<Loader>, bool>> latest_;
    std::vector<std::string> latest_ids_;
    double loaded_at_ = 0;
    Page page_;
};

// ============================================================================
// Library grid

class LibraryScreen : public app::Screen {
public:
    explicit LibraryScreen(jf::Item view) : view_(std::move(view)) {
        music_ = view_.collection_type == "music";
        types_ = view_.collection_type == "movies" ? "Movie" : view_.collection_type == "tvshows" ? "Series"
                 : music_ ? "MusicAlbum" : view_.collection_type == "boxsets" ? "BoxSet" : "";
        fetch(true);
    }
    app::Section section() const override { return app::SEC_JELLYFIN; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(id("jf_lib"), view_.id);
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw_fit(font::display, x0, y, W - x0 - 60, view_.name, t.text);
        if (total_ > 0) text::draw(font::small, x0 + 4, y + 60, util::fmt(tr("%d items"), total_), t.text3);
        y += 96;
        Id sg = id(g, "sort");
        const char* sorts[] = {tr("A\xE2\x80\x93Z"), tr("Recently added"), tr("Release date"), tr("Rating")};
        const char* keys[] = {"SortName", "DateCreated", "PremiereDate", "CommunityRating"};
        float cx = x0;
        for (int i = 0; i < 4; i++) {
            float w = text::measure(font::small_bold, sorts[i]) + 40;
            if (chip(id(sg, (int64_t)i), Rect(cx, y, w, 42), sorts[i], sort_ == i, sg)) {
                sort_ = i;
                sort_key_ = keys[i];
                fetch(true);
            }
            cx += w + 12;
        }
        y += 72;
        if (focus_in_group(sg)) page_.focus_range(0, y + page_.scroll());

        if (!loading_ && items_.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 280), ic::VIDEO_LIBRARY,
                                   error_.empty() ? tr("Nothing here yet") : tr("Couldn't load this library"), error_.c_str()))
                fetch(true);
            y += 330;
        } else {
            GridSpec gs;
            gs.count = (int)items_.size();
            gs.cols = music_ ? 5 : 6;
            gs.item_w = music_ ? 190 : 158;
            gs.gap_x = music_ ? 26 : 22;
            gs.shape = music_ ? CARD_SQUARE : CARD_POSTER;
            gs.loading = loading_;
            gs.item = [this](int i) { return poster_card(items_[i]); };
            gs.on_click = [this](int i) { open_item(items_[i]); };
            gs.on_focus = [this](int i) { set_backdrop(jf::backdrop(items_[i], 1280)); };
            gs.on_reach_end = [this] {
                if (!loading_ && (int)items_.size() < total_) fetch(false);
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    void fetch(bool reset) {
        if (reset) {
            scope_.reset();
            items_.clear();
            total_ = 0;
        }
        loading_ = true;
        std::string pid = view_.id, types = types_, sort = sort_key_;
        int start = (int)items_.size();
        scope_.run<jf::List>([pid, types, sort, start] { return jf::items(pid, types, sort, start, 60); },
                             [this](jf::List l) {
                                 loading_ = false;
                                 error_ = l.error;
                                 total_ = l.total;
                                 for (auto& it : l.items) items_.push_back(std::move(it));
                             });
    }

    jf::Item view_;
    bool music_ = false;
    std::string types_, sort_key_ = "SortName";
    int sort_ = 0, total_ = 0;
    std::vector<jf::Item> items_;
    bool loading_ = true;
    std::string error_;
    tasks::Scope scope_;
    Page page_;
};

// ============================================================================
// Details (movie / series / album)

class DetailScreen : public app::Screen {
public:
    explicit DetailScreen(jf::Item it) : it_(std::move(it)) {
        refresh();
        if (it_.type == "Series") {
            seasons_.load([id = it_.id] { return jf::seasons(id); });
            next_.load([id = it_.id] { return jf::next_up(id); });
        } else if (it_.type == "MusicAlbum" || it_.type == "BoxSet" || it_.type == "Season") {
            children_.load([id = it_.id] { return jf::children(id); });
        } else {
            similar_.load([id = it_.id] { return jf::similar(id); });
        }
    }

    app::Section section() const override { return app::SEC_JELLYFIN; }
    bool draws_background() const override { return true; }
    void on_enter() override { refresh(); }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(id("jf_detail"), it_.id);

        // Cinematic backdrop
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        std::string bd = jf::backdrop(it_, 1280);
        if (const images::Image* img = images::get(bd, 1280, 720); img && img->ready) {
            float a = images::fade(img, 0.6f);
            float drift = std::sin((float)ui::time() * 0.05f) * 12;
            gfx::image_cover(img->tex, img->w, img->h, Rect(-20 + drift, -10, W + 40, H + 20), 0, Color(255, 255, 255, (uint8_t)(255 * a)));
        }
        gfx::fill_rect_hgrad(Rect(0, 0, W, H), Color(8, 6, 11, 250), Color(8, 6, 11, 40));
        gfx::fill_rect_vgrad(Rect(0, H * 0.35f, W, H * 0.65f), Color(8, 6, 11, 0), Color(8, 6, 11, 250));

        page_.begin(id(g, "page"));
        float y = page_.y(70);
        text::draw_wrapped(font::display, Rect(x0, y, 760, 120), it_.name, t.text, 2);
        y += text::measure_wrapped(font::display, 760, it_.name, 2) + 8;

        // Meta row
        float mx = x0;
        auto meta = [&](const std::string& s, bool boxed = false) {
            if (s.empty()) return;
            float w = text::measure(font::body_bold, s);
            if (boxed) {
                gfx::stroke_rrect(Rect(mx - 6, y - 2, w + 12, 30), 6, 1.5f, t.text2);
            }
            text::draw(font::body_bold, mx, y, s, t.text2);
            mx += w + (boxed ? 28 : 22);
        };
        meta(it_.year);
        meta(it_.rating, true);
        if (it_.runtime > 0) meta(util::format_ticks_duration((int64_t)(it_.runtime * 1e7)));
        if (it_.community_rating > 0) {
            text::icon(ic::STAR, 22, mx + 10, y + 13, t.warn);
            mx += 26;
            meta(util::fmt("%.1f", it_.community_rating));
        }
        if (!it_.album_artist.empty()) meta(it_.album_artist);
        y += 44;
        if (!it_.overview.empty()) {
            y += text::draw_wrapped(font::body, Rect(x0, y, 720, 120), it_.overview, t.text2, 4, text::LEFT, 1.1f) + 20;
        }

        // Actions
        Id ag = id(g, "actions");
        float bx = x0;
        bool resumable = it_.position > 30;
        std::string play_label = resumable ? util::fmt(tr("Resume %s"), util::format_duration(it_.position).c_str()) : tr("Play");
        const char* play_text = play_label.c_str();
        bool can_play = it_.type == "Movie" || it_.type == "Episode" || it_.type == "Video" || it_.type == "MusicAlbum" ||
                        (it_.type == "Series" && next_.loaded && !next_.list.items.empty());
        if (it_.type == "Series" && next_.loaded && !next_.list.items.empty()) {
            const jf::Item& n = next_.list.items[0];
            play_label = util::fmt(tr("Play S%d E%d"), n.parent_index, n.index);
            play_text = play_label.c_str();
        }
        if (can_play) {
            float w = measure_button(play_text, ic::PLAY);
            if (button(id(ag, "play"), Rect(bx, y, w, 56), play_text, ic::PLAY, BTN_PRIMARY, ag, F_DEFAULT)) play(false);
            bx += w + 16;
            if (resumable) {
                float rw = measure_button(tr("From start"), ic::REFRESH);
                if (button(id(ag, "restart"), Rect(bx, y, rw, 56), tr("From start"), ic::REFRESH, BTN_NORMAL, ag)) play(true);
                bx += rw + 16;
            }
        }
        if (icon_button(id(ag, "fav"), bx + 28, y + 28, 28, it_.favorite ? ic::FAVORITE : ic::FAVORITE_BORDER, ag, 0, it_.favorite)) {
            it_.favorite = !it_.favorite;
            jf::set_favorite(it_.id, it_.favorite);
            toast(it_.favorite ? tr("Added to favorites") : tr("Removed from favorites"), ic::FAVORITE);
        }
        bx += 72;
        if (it_.type == "Movie" || it_.type == "Episode" || it_.type == "Series") {
            if (icon_button(id(ag, "played"), bx + 28, y + 28, 28, ic::CHECK_CIRCLE, ag, 0, it_.played)) {
                it_.played = !it_.played;
                jf::set_played(it_.id, it_.played);
                toast(it_.played ? tr("Marked as watched") : tr("Marked as unwatched"), ic::CHECK_CIRCLE);
            }
        }
        y += 96;
        if (focus_in_group(ag)) page_.focus_range(0, y + page_.scroll());

        if (it_.type == "Series") y = series_section(g, x0, y);
        else if (it_.type == "MusicAlbum") y = tracks_section(g, x0, y);
        else if (!children_.list.items.empty()) y += items_shelf(g, "children", x0, y, tr("Items"), children_, CARD_POSTER);
        else if (similar_.loaded && !similar_.list.items.empty()) y += items_shelf(g, "similar", x0, y, tr("More like this"), similar_, CARD_POSTER);

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void refresh() {
        std::string id = it_.id;
        detail_.run<jf::Item>(
            [id] {
                jf::Item it;
                std::string err;
                jf::item(id, it, err);
                return it;
            },
            [this](jf::Item it) {
                if (!it.id.empty()) it_ = std::move(it);
            });
    }

    void play(bool from_start) {
        if (it_.type == "Series" && !next_.list.items.empty()) {
            play_video(jf::make_source(next_.list.items[0], from_start));
        } else if (it_.type == "MusicAlbum") {
            play_tracks(0);
        } else {
            play_video(jf::make_source(it_, from_start));
        }
    }

    void play_tracks(int index) {
        std::vector<player::Source> q;
        for (auto& tr : children_.list.items)
            if (tr.is_audio()) {
                player::Source s = jf::make_source(tr);
                if (s.artwork.empty()) s.artwork = jf::poster(it_, 600);
                q.push_back(std::move(s));
            }
        if (!q.empty()) play_audio_queue(std::move(q), std::clamp(index, 0, (int)q.size() - 1));
    }

    float items_shelf(Id g, const char* key, float x, float y, const char* title, Loader& l, CardShape shape) {
        ShelfSpec s;
        s.title = title;
        s.count = (int)l.list.items.size();
        s.loading = !l.loaded;
        s.shape = shape;
        s.item_w = 170;
        s.item = [&l](int i) { return poster_card(l.list.items[i]); };
        s.on_click = [&l](int i) { open_item(l.list.items[i]); };
        return shelf(id(g, key), x, y, s, &page_) + 12;
    }

    float series_section(Id g, float x, float y) {
        const Theme& t = theme();
        if (seasons_.loaded && season_index_ < 0 && !seasons_.list.items.empty()) {
            season_index_ = 0;
            // Start on the season of the next episode.
            if (!next_.list.items.empty())
                for (size_t i = 0; i < seasons_.list.items.size(); i++)
                    if (seasons_.list.items[i].id == next_.list.items[0].season_id) season_index_ = (int)i;
            load_episodes();
        }
        Id sg = id(g, "seasons");
        float cx = x;
        for (size_t i = 0; i < seasons_.list.items.size(); i++) {
            const std::string& n = seasons_.list.items[i].name;
            float w = text::measure(font::small_bold, n) + 40;
            if (chip(id(sg, (int64_t)i), Rect(cx, y, w, 42), n.c_str(), (int)i == season_index_, sg) && season_index_ != (int)i) {
                season_index_ = (int)i;
                load_episodes();
            }
            cx += w + 12;
        }
        y += 66;
        if (focus_in_group(sg)) page_.focus_range(y - 66 + page_.scroll() - 20, y + page_.scroll());
        ShelfSpec s;
        s.count = (int)episodes_.list.items.size();
        s.loading = episodes_.loading;
        s.item_w = 320;
        s.item = [this](int i) {
            const jf::Item& e = episodes_.list.items[i];
            CardInfo c = thumb_card(e);
            c.title = util::fmt("%d. %s", e.index, e.name.c_str());
            c.subtitle = e.runtime > 0 ? util::format_ticks_duration((int64_t)(e.runtime * 1e7)) : "";
            if (e.played) c.subtitle = c.subtitle.empty() ? std::string(tr("Watched")) : util::fmt("%s \xC2\xB7 %s", c.subtitle.c_str(), tr("Watched"));
            c.badge.clear();
            return c;
        };
        s.on_click = [this](int i) { play_video(jf::make_source(episodes_.list.items[i])); };
        s.on_focus = [this](int i) { focused_ep_ = i; };
        y += shelf(id(g, "episodes"), x, y, s, &page_);
        if (focused_ep_ >= 0 && focused_ep_ < (int)episodes_.list.items.size()) {
            const std::string& ov = episodes_.list.items[focused_ep_].overview;
            if (!ov.empty()) y += text::draw_wrapped(font::small, Rect(x, y, 900, 80), ov, t.text2, 3) + 10;
        }
        return y + 20;
    }

    void load_episodes() {
        focused_ep_ = -1;
        std::string sid = it_.id, season = seasons_.list.items[season_index_].id;
        episodes_.load([sid, season] { return jf::episodes(sid, season); });
    }

    float tracks_section(Id g, float x, float y) {
        const Theme& t = theme();
        text::draw(font::title, x, y, tr("Tracks"), t.text);
        y += 50;
        if (!children_.loaded) {
            loading_indicator(x + 40, y + 20);
            return y + 60;
        }
        Id tg = id(g, "tracks");
        for (size_t i = 0; i < children_.list.items.size(); i++) {
            const jf::Item& tr = children_.list.items[i];
            Rect r(x, y, 820, 54);
            Id tid = id(tg, (int64_t)i);
            Item it = focusable(tid, r, tg);
            if (it.focused) page_.focus_range(r.y + page_.scroll() - 10, r.b() + page_.scroll() + 10);
            Rect br = r.offset(bump_x(tid), bump_y(tid));
            gfx::fill_rrect(br, 12, gfx::lerp(Color(255, 255, 255, i % 2 ? 6 : 12), t.surface_focus, it.f));
            Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f), fg2 = gfx::lerp(t.text3, Color(21, 18, 26, 150), it.f);
            text::draw(font::body_bold, br.x + 30, br.cy() - 12, std::to_string(tr.index > 0 ? tr.index : (int)i + 1), fg2, text::CENTER);
            text::draw_fit(font::body, br.x + 64, br.cy() - 12, br.w - 180, tr.name, fg);
            text::draw(font::small_bold, br.r() - 20, br.cy() - 10, util::format_duration(tr.runtime), fg2, text::RIGHT);
            if (it.clicked) play_tracks((int)i);
            y += 60;
        }
        return y + 20;
    }

    jf::Item it_;
    Loader seasons_, episodes_, next_, children_, similar_;
    tasks::Scope detail_;
    int season_index_ = -1, focused_ep_ = -1;
    Page page_;
};

// ============================================================================
// Search results

class JellyfinSearchScreen : public app::Screen {
public:
    explicit JellyfinSearchScreen(std::string q) : q_(std::move(q)) {
        std::string term = q_;
        res_.load([term] { return jf::search(term); });
    }
    app::Section section() const override { return app::SEC_JELLYFIN; }
    void frame() override {
        float x0 = content_x();
        Id g = id(id("jf_search"), q_);
        page_.begin(id(g, "page"));
        float y = page_.y(52);
        text::draw_fit(font::headline, x0, y, W - x0 - 60, "\xE2\x80\x9C" + q_ + "\xE2\x80\x9D", theme().text);
        y += 80;
        if (res_.loaded && res_.list.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 280), ic::SEARCH, tr("No matches"), res_.list.error.c_str());
            y += 300;
        } else {
            GridSpec gs;
            gs.count = (int)res_.list.items.size();
            gs.cols = 6;
            gs.item_w = 158;
            gs.shape = CARD_POSTER;
            gs.loading = !res_.loaded;
            gs.item = [this](int i) { return poster_card(res_.list.items[i]); };
            gs.on_click = [this](int i) { open_item(res_.list.items[i]); };
            gs.on_focus = [this](int i) { set_backdrop(jf::backdrop(res_.list.items[i], 1280)); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
    }

private:
    std::string q_;
    Loader res_;
    Page page_;
};

std::unique_ptr<app::Screen> make_detail(const jf::Item& it) { return std::make_unique<DetailScreen>(it); }
std::unique_ptr<app::Screen> make_library(const jf::Item& view) { return std::make_unique<LibraryScreen>(view); }

}  // namespace

std::unique_ptr<app::Screen> make_jellyfin() { return std::make_unique<JellyfinScreen>(); }
std::unique_ptr<app::Screen> make_jellyfin_search(const std::string& q) { return std::make_unique<JellyfinSearchScreen>(q); }

std::unique_ptr<app::Screen> make_jellyfin_item(const jellyfin::Item& item) { return make_detail(item); }

}  // namespace screens
