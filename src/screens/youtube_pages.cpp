// YouTube channel and playlist pages, subscriptions, the library (Watch later, history, saved
// playlists) and the per-video "More" menu. All of it is stored locally; a signed-in account
// adds its own subscriptions and recommendations.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>

#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "gfx/images.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/youtube_common.hpp"
#include "services/yt_account.hpp"
#include "services/yt_recs.hpp"
#include "ui/ui.hpp"

namespace screens {
namespace yt {

using namespace ui;

const char* const SUBS = "yt_channel";
const char* const LATER = "yt_later";
const char* const WATCHED = "yt_watched";
const char* const PLAYLISTS = "yt_playlist";

// --- feed -------------------------------------------------------------------------------------

void Feed::load() {
    if (loading || loaded) return;
    loading = true;
    auto f = fetch;
    scope.run<youtube::Results>([f] { return f(""); }, [this](youtube::Results r) {
        res = std::move(r);
        loading = false;
        loaded = true;
    });
}

void Feed::more() {
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

void Feed::reload() {
    scope.reset();
    loading = loaded = false;
    res = youtube::Results();
    load();
}

void Feed::remove(const std::string& video_id) {
    res.items.erase(std::remove_if(res.items.begin(), res.items.end(),
                                   [&](const youtube::Video& v) { return v.id == video_id; }),
                    res.items.end());
}

// --- cards and playback ----------------------------------------------------------------------

CardInfo video_card(const youtube::Video& v) {
    CardInfo c;
    c.image = youtube::thumbnail(v.id);
    c.image_w = 360;
    c.title = v.title;
    std::string sub = v.channel;
    if (!v.views.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + v.views;
    if (!v.published.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + v.published;
    c.subtitle = sub;
    c.badge = v.live ? "" : v.duration;
    c.live = v.live;
    c.icon = ic::SMART_DISPLAY;
    double pos = store::resume_position("youtube", v.id);
    if (pos > 0) c.progress = 0.35f;
    return c;
}

CardInfo playlist_card(const youtube::Playlist& p) {
    CardInfo c;
    c.image = p.thumbnail;
    c.image_w = 360;
    c.title = p.title;
    c.subtitle = p.channel;
    c.badge = p.count;
    c.icon = ic::PLAYLIST_PLAY;
    return c;
}

namespace {

// Videos are stored as favorites: extra = channel id, duration, views, published, live.
store::Fav to_fav(const youtube::Video& v) {
    std::string extra = v.channel_id + "\x1f" + v.duration + "\x1f" + v.views + "\x1f" + v.published + "\x1f" +
                        (v.live ? "1" : "");
    return store::Fav{v.id, v.title, v.channel, youtube::thumbnail(v.id), extra};
}

youtube::Video from_fav(const store::Fav& f) {
    youtube::Video v;
    v.id = f.id;
    v.title = f.title;
    v.channel = f.subtitle;
    std::vector<std::string> p = util::split(f.extra, '\x1f');
    if (p.size() > 0) v.channel_id = p[0];
    if (p.size() > 1) v.duration = p[1];
    if (p.size() > 2) v.views = p[2];
    if (p.size() > 3) v.published = p[3];
    if (p.size() > 4) v.live = p[4] == "1";
    return v;
}

std::vector<youtube::Video> videos_in(const char* list) {
    std::vector<youtube::Video> out;
    for (const store::Fav& f : store::favs(list)) out.push_back(from_fav(f));
    return out;
}

void remember_watched(const youtube::Video& v) {
    store::fav_set(WATCHED, to_fav(v), true);
    store::fav_trim(WATCHED, 200);
}

}  // namespace

void play(const youtube::Video& v) {
    remember_watched(v);
    play_video(youtube::make_source(v));
}

void play_all(const std::vector<youtube::Video>& list, int index) {
    if (index < 0 || index >= (int)list.size()) return;
    std::vector<player::Source> queue;
    for (const youtube::Video& v : list) {
        player::Source s = youtube::make_source(v);
        // The queue moves on by itself: note each video that actually got watched.
        s.on_stop = [v, stop = s.on_stop](double position, bool finished) {
            if (position > 10 || finished) remember_watched(v);
            if (stop) stop(position, finished);
        };
        queue.push_back(std::move(s));
    }
    remember_watched(list[index]);
    play_video_queue(std::move(queue), index);
}

// --- subscriptions ----------------------------------------------------------------------------

bool subscribed(const std::string& channel_id) { return store::fav_has(SUBS, channel_id); }

void set_subscribed(const std::string& channel_id, const std::string& name, const std::string& avatar, bool on) {
    if (channel_id.empty()) {
        toast("Channel unavailable for this video", ic::INFO);
        return;
    }
    if (on) {
        store::Fav f{channel_id, name, "", avatar, ""};
        store::fav_set(SUBS, f, true);
    } else {
        store::fav_set(SUBS, store::Fav{channel_id, "", "", "", ""}, false);
    }
    yt_recs::on_subscribe(channel_id, on);
    std::string who = name.empty() ? "channel" : name;
    toast(on ? "Subscribed to " + who : "Unsubscribed from " + who, on ? ic::CHECK_CIRCLE : ic::REMOVE);
}

std::vector<youtube::Channel> subscriptions() {
    std::vector<youtube::Channel> out;
    for (const store::Fav& f : store::favs(SUBS)) {
        youtube::Channel c;
        c.id = f.id;
        c.name = f.title;
        // Older versions stored a video thumbnail here instead of the avatar.
        if (f.image.find("ytimg.com/vi/") == std::string::npos) c.avatar = f.image;
        out.push_back(c);
    }
    return out;
}

std::vector<youtube::Channel> with_local_subscriptions(std::vector<youtube::Channel> account) {
    std::set<std::string> seen;
    for (const youtube::Channel& c : account) seen.insert(c.id);
    for (youtube::Channel& c : subscriptions())
        if (seen.insert(c.id).second) account.push_back(std::move(c));
    return account;
}

youtube::Results for_you(const std::string& continuation) {
    if (yt_account::signed_in()) {
        if (!continuation.empty()) return youtube::account_home(continuation);
        // Home and the YouTube page both want it at the start: fetched once, kept 10 minutes.
        static std::mutex m;
        static youtube::Results cached;
        static int cached_version = -1;
        static double cached_at = -1e9;
        std::lock_guard<std::mutex> lk(m);
        int v = yt_account::version();
        if (v == cached_version && util::now_seconds() - cached_at < 10 * 60 && !cached.items.empty()) return cached;
        youtube::Results r = youtube::account_home();
        if (!r.items.empty()) {
            cached = r;
            cached_version = v;
            cached_at = util::now_seconds();
            return r;
        }
        log_message(LOG_WARNING, "YouTube", "The account's recommendations didn't load (%s), showing this console's",
                    r.error.c_str());
    }
    return continuation.empty() ? yt_recs::for_you() : youtube::Results();
}

bool has_for_you() { return yt_account::signed_in() || yt_recs::has_profile(); }

void update_channels(const std::vector<youtube::Channel>& channels) {
    for (const youtube::Channel& c : channels) {
        if (c.id.empty() || c.name.empty() || !subscribed(c.id)) continue;
        store::fav_update(SUBS, store::Fav{c.id, c.name, "", c.avatar, ""});
    }
}

namespace {

std::string shown_path(const std::string& path) {
    const std::string sd = "/vol/external01/";
    return util::starts_with(path, sd) ? "sd:/" + path.substr(sd.size()) : path;
}

// One CSV line into fields ("quoted, with commas" allowed).
std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') out.back() += '"', i++;
            else if (c == '"') quoted = false;
            else out.back() += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.emplace_back();
        } else if (c != '\r') {
            out.back() += c;
        }
    }
    return out;
}

std::string channel_id_in(const std::string& s) {
    size_t p = s.find("UC");
    while (p != std::string::npos) {
        std::string id = s.substr(p, 24);
        bool ok = id.size() == 24 && std::all_of(id.begin(), id.end(), [](char c) {
            return isalnum((unsigned char)c) || c == '-' || c == '_';
        });
        if (ok && (p == 0 || !isalnum((unsigned char)s[p - 1]))) return id;
        p = s.find("UC", p + 1);
    }
    return "";
}

}  // namespace

std::string import_subscriptions() {
    std::string dir = platform::data_dir();
    static const char* const FILES[] = {"subscriptions.csv", "subscriptions.json", "newpipe_subscriptions.json",
                                        "youtube_subscriptions.json"};
    std::vector<std::pair<std::string, std::string>> found;  // {id, name}
    std::string used;
    for (const char* name : FILES) {
        std::string data;
        if (!util::read_file(util::join_path(dir, name), data)) continue;
        used = name;
        if (util::ends_with(name, ".csv")) {
            // Google Takeout: "Channel Id,Channel Url,Channel Title" (the header is localized).
            for (const std::string& line : util::split(data, '\n')) {
                std::vector<std::string> f = csv_fields(line);
                std::string id = channel_id_in(f[0]);
                if (id.empty() && f.size() > 1) id = channel_id_in(f[1]);
                if (!id.empty()) found.emplace_back(id, f.size() > 2 ? util::trim(f[2]) : "");
            }
        } else {
            // NewPipe / LibreTube: {"subscriptions": [{"service_id": 0, "url": ".../channel/UC...", "name": "..."}]}
            json::Doc doc = json::Doc::parse(data);
            json_t* subs = json::at(doc.get(), {"subscriptions"});
            for (size_t i = 0; i < json::size(subs); i++) {
                json_t* s = json_array_get(subs, i);
                if (json::num(s, {"service_id"}, 0) != 0) continue;  // other NewPipe services
                std::string id = channel_id_in(json::str(s, {"url"}));
                if (!id.empty()) found.emplace_back(id, json::str(s, {"name"}));
            }
        }
        break;
    }
    if (used.empty())
        return "Put subscriptions.csv (Google Takeout) or a NewPipe export in " + shown_path(dir);
    int added = 0;
    // Oldest first, so the list ends up in the file's order.
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        if (subscribed(it->first)) continue;
        store::fav_set(SUBS, store::Fav{it->first, it->second, "", "", ""}, true);
        yt_recs::on_subscribe(it->first, true);
        added++;
    }
    if (found.empty()) return "No channels found in " + used;
    if (added == 0) return "Already subscribed to everything in " + used;
    return util::fmt("Imported %d channel%s from %s", added, added == 1 ? "" : "s", used.c_str());
}

std::string export_subscriptions() {
    json_t* list = json_array();
    for (const youtube::Channel& c : subscriptions()) {
        json_t* o = json_object();
        json_object_set_new(o, "service_id", json_integer(0));
        json_object_set_new(o, "url", json_string(("https://www.youtube.com/channel/" + c.id).c_str()));
        json_object_set_new(o, "name", json_string(c.name.c_str()));
        json_array_append_new(list, o);
    }
    size_t n = json_array_size(list);
    json_t* root = json_object();
    json_object_set_new(root, "app_version", json_string("0.24.0"));  // NewPipe's format, for other apps
    json_object_set_new(root, "app_version_int", json_integer(990));
    json_object_set_new(root, "subscriptions", list);
    char* text = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    std::string path = util::join_path(platform::data_dir(), "youtube_subscriptions.json");
    bool ok = text && util::write_file_atomic(path, text);
    free(text);
    if (!ok) return "Couldn't write " + shown_path(path);
    return util::fmt("Saved %d channel%s to %s", (int)n, n == 1 ? "" : "s", shown_path(path).c_str());
}

// --- library ------------------------------------------------------------------------------------

bool in_watch_later(const std::string& video_id) { return store::fav_has(LATER, video_id); }

void set_watch_later(const youtube::Video& v, bool on) {
    store::fav_set(LATER, to_fav(v), on);
    toast(on ? "Saved to Watch later" : "Removed from Watch later", on ? ic::WATCH_LATER : ic::REMOVE);
}

std::vector<youtube::Video> watch_later() { return videos_in(LATER); }
std::vector<youtube::Video> history() { return videos_in(WATCHED); }

void remove_from_history(const std::string& video_id) {
    store::fav_set(WATCHED, store::Fav{video_id, "", "", "", ""}, false);
}

void clear_history() { store::fav_clear(WATCHED); }

bool playlist_saved(const std::string& playlist_id) { return store::fav_has(PLAYLISTS, playlist_id); }

void set_playlist_saved(const youtube::Playlist& p, bool on) {
    store::fav_set(PLAYLISTS, store::Fav{p.id, p.title, p.channel, p.thumbnail, p.count}, on);
    toast(on ? "Saved to your library" : "Removed from your library", on ? ic::PLAYLIST_ADD_CHECK : ic::REMOVE);
}

std::vector<youtube::Playlist> saved_playlists() {
    std::vector<youtube::Playlist> out;
    for (const store::Fav& f : store::favs(PLAYLISTS)) {
        youtube::Playlist p;
        p.id = f.id;
        p.title = f.title;
        p.channel = f.subtitle;
        p.thumbnail = f.image;
        p.count = f.extra;
        out.push_back(p);
    }
    return out;
}

// --- the "More" menu ------------------------------------------------------------------------------

void video_menu(const youtube::Video& v, MenuOptions o) {
    std::vector<MenuItem> items;
    if (o.show_channel && !v.channel_id.empty())
        items.push_back({"Go to channel", ic::ACCOUNT_CIRCLE, [v] { app::push(make_channel(v.channel_id, v.channel)); }});
    if (!v.channel_id.empty()) {
        bool sub = subscribed(v.channel_id);
        items.push_back({sub ? "Unsubscribe" : "Subscribe", sub ? ic::REMOVE : ic::PERSON_ADD,
                         [v, sub] { set_subscribed(v.channel_id, v.channel, "", !sub); }});
    }
    bool later = in_watch_later(v.id);
    items.push_back({later ? "Remove from Watch later" : "Save to Watch later", ic::WATCH_LATER,
                     [v, later] { set_watch_later(v, !later); }});
    if (!v.live)
        items.push_back({"Play from the start", ic::REPLAY_10, [v] {
                             store::resume_remove("youtube", v.id);
                             play(v);
                         }});
    items.push_back({"Not interested", ic::THUMB_DOWN, [v, removed = o.removed] {
                         yt_recs::not_interested(v);
                         if (removed) removed();
                         toast("Got it, you'll see less like this", ic::CHECK_CIRCLE);
                     }});
    for (MenuItem& e : o.extra) items.push_back(std::move(e));
    std::string sub = v.channel;
    if (!v.duration.empty() && !v.live) sub += (sub.empty() ? "" : " \xC2\xB7 ") + v.duration;
    show_menu(v.title, sub, std::move(items));
}

namespace {

// --- shared drawing ---------------------------------------------------------------------------

// Round avatar with a placeholder while it loads (or when there is none).
void draw_avatar(const std::string& url, const Rect& r) {
    const Theme& t = theme();
    const images::Image* img = url.empty() ? nullptr : images::get(url, (int)r.w * 2, (int)r.h * 2);
    if (img && img->ready) {
        gfx::image_cover(img->tex, img->w, img->h, r, r.w * 0.5f);
    } else {
        gfx::fill_rrect_vgrad(r, r.w * 0.5f, t.accent, t.accent2);
        text::icon(ic::PERSON, r.w * 0.45f, r.cx(), r.cy(), gfx::WHITE);
    }
}

// Channel banners come 2560 wide; ask for a size the screen can use.
std::string banner_url(std::string url) {
    size_t p = url.find("=w");
    if (p == std::string::npos) return url;
    size_t e = p + 2;
    while (e < url.size() && isdigit((unsigned char)url[e])) e++;
    return url.substr(0, p) + "=w1280" + url.substr(e);
}

std::string joined(std::initializer_list<std::string> parts) {
    std::string out;
    for (const std::string& p : parts)
        if (!p.empty()) out += (out.empty() ? "" : "  \xC2\xB7  ") + p;
    return out;
}

GridSpec video_grid(Feed& f, bool on_channel) {
    GridSpec gs;
    gs.count = (int)f.res.items.size();
    gs.cols = 3;
    gs.item_w = 336;
    gs.gap_x = 26;
    gs.loading = f.loading;
    gs.item = [&f](int i) { return video_card(f.res.items[i]); };
    gs.on_click = [&f](int i) { play(f.res.items[i]); };
    gs.on_focus = [&f](int i) { set_backdrop(youtube::thumbnail_hq(f.res.items[i].id)); };
    gs.on_x = [&f, on_channel](int i) {
        MenuOptions o;
        o.show_channel = !on_channel;
        std::string id = f.res.items[i].id;
        o.removed = [&f, id] { f.remove(id); };
        video_menu(f.res.items[i], o);
    };
    gs.on_reach_end = [&f] { f.more(); };
    return gs;
}

// Empty/error state for a feed; returns the height used (0 when there are items).
float feed_state(Id id, Feed& f, float x, float y, const char* empty_title, const char* empty_desc) {
    if (!f.loaded || !f.res.items.empty()) return 0;
    Rect r(x, y, W - x - 60, 260);
    if (f.res.error.empty()) {
        empty_state(r, ic::SMART_DISPLAY, empty_title, empty_desc);
    } else if (empty_state_action(id, r, ic::WIFI_OFF, "Couldn't reach YouTube", f.res.error.c_str())) {
        f.reload();
    }
    return 310;
}

// --- channel page -------------------------------------------------------------------------------

class ChannelScreen : public app::Screen {
public:
    ChannelScreen(const std::string& id, const std::string& name, const std::string& avatar)
        : fetched_(std::make_shared<youtube::Channel>()) {
        ch_.id = id;
        ch_.name = name;
        ch_.avatar = avatar;
        // Stored avatar for subscribed channels shows up before the page loads.
        if (ch_.avatar.empty())
            for (const youtube::Channel& c : subscriptions())
                if (c.id == id) ch_.avatar = c.avatar;
        for (int t = 0; t < 3; t++) {
            youtube::Tab tab = (youtube::Tab)t;
            auto hdr = t == 0 ? fetched_ : nullptr;
            feeds_[t].fetch = [id, tab, hdr](const std::string& c) {
                return youtube::channel_tab(id, tab, c, c.empty() ? hdr.get() : nullptr);
            };
        }
        feeds_[0].load();
    }
    app::Section section() const override { return app::SEC_YOUTUBE; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), cw = W - x0 - 60;
        Id g = id(id("ytchannel"), ch_.id);
        take_header();
        page_.begin(id(g, "page"));
        float y = page_.y(36);

        if (!ch_.banner.empty()) {
            Rect br(x0, y, cw, 150);
            const images::Image* img = images::get(banner_url(ch_.banner), 1280, 212);
            if (img && img->ready) {
                gfx::push_alpha(images::fade(img));
                gfx::image_cover(img->tex, img->w, img->h, br, 18);
                gfx::pop_alpha();
            } else {
                gfx::fill_rrect(br, 18, t.surface);
            }
            y += 172;
        }

        Rect av(x0, y, 104, 104);
        draw_avatar(ch_.avatar, av);
        float tx = av.r() + 28;
        Id ag = id(g, "actions");
        bool sub = subscribed(ch_.id);
        const char* label = sub ? "Subscribed" : "Subscribe";
        float bw = measure_button(label, sub ? ic::CHECK : ic::PERSON_ADD);
        float tw = cw - (tx - x0) - bw - 30;
        text::draw_fit(font::headline, tx, y + 4, tw, ch_.name.empty() ? "Channel" : ch_.name, t.text);
        text::draw_fit(font::body, tx, y + 50, tw, joined({ch_.handle, ch_.subscribers, ch_.videos}), t.text2);
        std::string desc = ch_.description.substr(0, ch_.description.find('\n'));
        if (!desc.empty()) text::draw_fit(font::small, tx, y + 80, tw, desc, t.text3);
        if (button(id(ag, "sub"), Rect(x0 + cw - bw, y + 26, bw, 52), label, sub ? ic::CHECK : ic::PERSON_ADD,
                   sub ? BTN_NORMAL : BTN_PRIMARY, ag, F_DEFAULT))
            set_subscribed(ch_.id, ch_.name, ch_.avatar, !sub);
        y += 132;

        static const char* const TABS[] = {"Videos", "Shorts", "Live", "Playlists"};
        static const int ICONS[] = {ic::SMART_DISPLAY, ic::BOLT, ic::SENSORS, ic::PLAYLIST_PLAY};
        Id tg = id(g, "tabs");
        float cx = x0;
        for (int i = 0; i < 4; i++) {
            float w = text::measure(font::small_bold, TABS[i]) + 64;
            if (chip(id(tg, (int64_t)i), Rect(cx, y, w, 44), TABS[i], tab_ == i, tg, ICONS[i]) && tab_ != i) {
                tab_ = i;
                if (i < 3) feeds_[i].load();
                else load_playlists(false);
            }
            cx += w + 12;
        }
        y += 72;
        // The header and tabs aren't a shelf or grid: scroll back up to them when they're focused.
        if (focus_in_group(ag) || focus_in_group(tg)) page_.focus_range(0, y + page_.scroll());

        if (tab_ < 3) {
            Feed& f = feeds_[tab_];
            static const char* const EMPTY[] = {"No videos yet", "No Shorts", "No live streams"};
            float h = feed_state(id(g, "retry"), f, x0, y, EMPTY[tab_], "Nothing on this tab.");
            if (h > 0) {
                y += h;
            } else {
                name_videos(f);
                y += grid(id(id(g, "grid"), (int64_t)tab_), x0, y, video_grid(f, true), &page_);
            }
        } else {
            y += playlists_grid(g, x0, y);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Select"}, {"X", "More"}, {"B", "Back"}});
    }

private:
    // The first Videos page brings the channel's header.
    void take_header() {
        if (have_header_ || !feeds_[0].loaded) return;
        have_header_ = true;
        const youtube::Channel& h = *fetched_;
        if (h.name.empty()) return;
        std::string avatar = h.avatar.empty() ? ch_.avatar : h.avatar;
        ch_ = h;
        ch_.avatar = avatar;
        if (subscribed(ch_.id)) update_channels({ch_});
    }

    // Channel pages leave the channel's name off each video.
    void name_videos(Feed& f) {
        if (ch_.name.empty()) return;
        for (youtube::Video& v : f.res.items)
            if (v.channel.empty()) v.channel = ch_.name;
    }

    float playlists_grid(Id g, float x0, float y) {
        if (!pl_loading_ && pl_.items.empty()) {
            Rect r(x0, y, W - x0 - 60, 260);
            if (pl_.error.empty()) empty_state(r, ic::PLAYLIST_PLAY, "No playlists", "This channel hasn't made any public playlists.");
            else if (empty_state_action(id(g, "plretry"), r, ic::WIFI_OFF, "Couldn't reach YouTube", pl_.error.c_str()))
                load_playlists(false);
            return 310;
        }
        GridSpec gs;
        gs.count = (int)pl_.items.size();
        gs.cols = 3;
        gs.item_w = 336;
        gs.gap_x = 26;
        gs.loading = pl_loading_;
        gs.item = [this](int i) { return playlist_card(pl_.items[i]); };
        gs.on_click = [this](int i) { app::push(make_playlist(pl_.items[i].id, pl_.items[i].title)); };
        gs.on_focus = [this](int i) { set_backdrop(pl_.items[i].thumbnail); };
        gs.on_x = [this](int i) {
            youtube::Playlist p = pl_.items[i];
            if (p.channel.empty()) p.channel = ch_.name;
            bool saved = playlist_saved(p.id);
            show_menu(p.title, p.count, {{saved ? "Remove from library" : "Save to library", ic::PLAYLIST_ADD,
                                          [p, saved] { set_playlist_saved(p, !saved); }}});
        };
        gs.on_reach_end = [this] { load_playlists(true); };
        return grid(id(g, "plgrid"), x0, y, gs, &page_);
    }

    void load_playlists(bool more) {
        if (pl_loading_ || (more ? pl_.continuation.empty() : pl_loaded_)) return;
        pl_loading_ = true;
        std::string id = ch_.id, c = more ? pl_.continuation : "";
        pl_scope_.run<youtube::PlaylistResults>([id, c] { return youtube::channel_playlists(id, c); },
                                                [this, more](youtube::PlaylistResults r) {
            pl_loading_ = false;
            pl_loaded_ = true;
            if (!more) {
                pl_ = std::move(r);
                return;
            }
            pl_.continuation = r.continuation;
            for (auto& p : r.items) pl_.items.push_back(std::move(p));
        });
    }

    youtube::Channel ch_;
    std::shared_ptr<youtube::Channel> fetched_;  // written by the first Videos load
    bool have_header_ = false;
    Feed feeds_[3];
    int tab_ = 0;
    youtube::PlaylistResults pl_;
    bool pl_loading_ = false, pl_loaded_ = false;
    tasks::Scope pl_scope_;
    Page page_;
};

// --- playlist page -------------------------------------------------------------------------------

class PlaylistScreen : public app::Screen {
public:
    PlaylistScreen(const std::string& id, const std::string& title) : fetched_(std::make_shared<youtube::Playlist>()) {
        info_.id = id;
        info_.title = title;
        for (const youtube::Playlist& p : saved_playlists())
            if (p.id == id) info_ = p;
        auto out = fetched_;
        feed_.fetch = [id, out](const std::string& c) {
            return youtube::playlist_videos(id, c, c.empty() ? out.get() : nullptr);
        };
        feed_.load();
    }
    app::Section section() const override { return app::SEC_YOUTUBE; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x(), cw = W - x0 - 60;
        Id g = id(id("ytplaylist"), info_.id);
        if (!have_info_ && feed_.loaded) {
            have_info_ = true;
            if (!fetched_->title.empty()) {
                info_ = *fetched_;
                if (playlist_saved(info_.id))
                    store::fav_update(PLAYLISTS, store::Fav{info_.id, info_.title, info_.channel, info_.thumbnail, info_.count});
            }
        }
        page_.begin(id(g, "page"));
        float y = page_.y(52);

        Rect art(x0, y, 320, 180);
        gfx::shadow(art, 26, Color(0, 0, 0, 170));
        std::string thumb = !feed_.res.items.empty() ? youtube::thumbnail_hq(feed_.res.items[0].id) : info_.thumbnail;
        const images::Image* img = thumb.empty() ? nullptr : images::get(thumb, 480, 360);
        if (img && img->ready) gfx::image_cover(img->tex, img->w, img->h, art, 16);
        else {
            gfx::fill_rrect_vgrad(art, 16, t.accent, t.accent2);
            text::icon(ic::PLAYLIST_PLAY, 64, art.cx(), art.cy(), gfx::WHITE);
        }
        if (!thumb.empty()) set_backdrop(thumb);
        float tx = art.r() + 32, tw = x0 + cw - tx;
        text::draw_wrapped(font::headline, Rect(tx, y, tw, 90), info_.title.empty() ? "Playlist" : info_.title, t.text, 2);
        float ty = y + text::measure_wrapped(font::headline, tw, info_.title.empty() ? "Playlist" : info_.title, 2) + 8;
        text::draw_fit(font::body, tx, ty, tw, joined({info_.channel, info_.count}), t.text2);
        Id ag = id(g, "actions");
        float by = y + 128, bx = tx;
        float pw = measure_button("Play all", ic::PLAY);
        if (button(id(ag, "play"), Rect(bx, by, pw, 52), "Play all", ic::PLAY, BTN_PRIMARY, ag, F_DEFAULT))
            play_all(feed_.res.items, 0);
        bx += pw + 14;
        float sw = measure_button("Shuffle", ic::SHUFFLE);
        if (button(id(ag, "shuffle"), Rect(bx, by, sw, 52), "Shuffle", ic::SHUFFLE, BTN_NORMAL, ag)) shuffle();
        bx += sw + 14;
        bool saved = playlist_saved(info_.id);
        const char* sl = saved ? "Saved" : "Save";
        if (button(id(ag, "save"), Rect(bx, by, measure_button(sl, ic::PLAYLIST_ADD), 52), sl,
                   saved ? ic::PLAYLIST_ADD_CHECK : ic::PLAYLIST_ADD, BTN_NORMAL, ag))
            set_playlist_saved(info_, !saved);
        y += 216;
        if (focus_in_group(ag)) page_.focus_range(0, y + page_.scroll());

        float h = feed_state(id(g, "retry"), feed_, x0, y, "This playlist is empty", "Its videos may be private or removed.");
        if (h > 0) {
            y += h;
        } else {
            GridSpec gs = video_grid(feed_, false);
            gs.on_click = [this](int i) { play_all(feed_.res.items, i); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "More"}, {"B", "Back"}});
    }

private:
    void shuffle() {
        if (feed_.res.items.empty()) return;
        std::vector<youtube::Video> list = feed_.res.items;
        for (size_t i = list.size() - 1; i > 0; i--) std::swap(list[i], list[util::hash64(list[i].id, (uint64_t)ui::frame()) % (i + 1)]);
        play_all(list, 0);
    }

    youtube::Playlist info_;
    std::shared_ptr<youtube::Playlist> fetched_;
    bool have_info_ = false;
    Feed feed_;
    Page page_;
};

// --- subscriptions --------------------------------------------------------------------------------

class SubscriptionsScreen : public app::Screen {
public:
    SubscriptionsScreen() { load(); }
    app::Section section() const override { return app::SEC_YOUTUBE; }

    void on_enter() override {
        if (store::favs(SUBS).size() != count_ || yt_account::version() != account_version_) load();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ytsubs");
        page_.begin(id(g, "page"));
        float y = page_.y(56);
        text::draw(font::headline, x0, y, "Subscriptions", t.text);
        const char* where = account_ ? "from your YouTube account and this console" : "stored on this console";
        text::draw(font::body, x0, y + 46,
                   chans_.empty() ? std::string(account_ ? "From your YouTube account and this console" : "Stored on this console")
                                  : util::fmt("%d channels \xC2\xB7 %s", (int)chans_.size(), where),
                   t.text2);
        y += 100;

        if (chans_.empty() && chans_loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 120);
            page_.end(y + 330 + page_.scroll());
            hint_bar({{"B", "Back"}});
            return;
        }
        if (chans_.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 280), ic::SUBSCRIPTIONS, "No subscriptions yet",
                        "Press X on a video and pick Subscribe, or import them from Google Takeout or NewPipe in Settings.");
            page_.end(y + 330 + page_.scroll());
            hint_bar({{"B", "Back"}});
            return;
        }

        ShelfSpec s;
        s.title = "Channels";
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
        s.on_click = [this](int i) { app::push(make_channel(chans_[i].id, chans_[i].name, chans_[i].avatar)); };
        s.on_x = [this](int i) {
            youtube::Channel c = chans_[i];
            std::vector<MenuItem> items = {
                {"Open channel", ic::ACCOUNT_CIRCLE, [c] { app::push(make_channel(c.id, c.name, c.avatar)); }},
            };
            // Only what was subscribed to here; the account's subscriptions are changed on YouTube.
            if (subscribed(c.id))
                items.push_back({"Unsubscribe", ic::REMOVE, [this, c] {
                     set_subscribed(c.id, c.name, c.avatar, false);
                     chans_.erase(std::remove_if(chans_.begin(), chans_.end(), [&](const youtube::Channel& o) { return o.id == c.id; }),
                                  chans_.end());
                     count_ = chans_.size();
                     std::string cid = c.id;
                     feed_.res.items.erase(std::remove_if(feed_.res.items.begin(), feed_.res.items.end(),
                                                          [&](const youtube::Video& v) { return v.channel_id == cid; }),
                                           feed_.res.items.end());
                 }});
            show_menu(c.name, "Channel", std::move(items));
        };
        y += shelf(id(g, "chans"), x0, y, s, &page_) + 12;

        text::draw(font::title, x0, y, "Latest videos", t.text);
        y += 52;
        float h = feed_state(id(g, "retry"), feed_, x0, y, "No videos yet", "Your channels haven't uploaded anything.");
        if (h > 0) y += h;
        else y += grid(id(g, "grid"), x0, y, video_grid(feed_, false), &page_);
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Select"}, {"X", "More"}, {"B", "Back"}});
    }

private:
    void load() {
        chans_ = subscriptions();
        count_ = chans_.size();
        account_ = yt_account::signed_in();
        account_version_ = yt_account::version();
        chans_scope_.reset();
        chans_loading_ = false;
        if (account_) {
            chans_loading_ = true;
            chans_scope_.run<youtube::ChannelResults>([] { return youtube::account_channels(); },
                                                      [this](youtube::ChannelResults r) {
                chans_loading_ = false;
                if (!r.ok) toast("Couldn't load your channels: " + r.error, ic::ERROR_OUTLINE, theme().bad);
                else chans_ = with_local_subscriptions(std::move(r.items));
            });
            feed_.fetch = [](const std::string& c) { return youtube::account_subscriptions(c); };
            feed_.reload();
            return;
        }
        std::vector<std::string> ids;
        for (size_t i = 0; i < chans_.size() && i < 60; i++) ids.push_back(chans_[i].id);
        feed_.fetch = [ids](const std::string& c) {
            if (!c.empty() || ids.empty()) return youtube::Results();
            std::vector<youtube::Channel> headers;
            youtube::Results r = youtube::subscription_feed(ids, 6, &headers);
            update_channels(headers);  // the store is thread-safe
            return r;
        };
        feed_.reload();
    }

    std::vector<youtube::Channel> chans_;
    size_t count_ = 0;
    bool account_ = false, chans_loading_ = false;
    int account_version_ = -1;
    tasks::Scope chans_scope_;
    Feed feed_;
    Page page_;
};

// --- library ----------------------------------------------------------------------------------------

class LibraryScreen : public app::Screen {
public:
    LibraryScreen() { refresh(); }
    app::Section section() const override { return app::SEC_YOUTUBE; }
    void on_enter() override { refresh(); }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ytlibrary");
        // Menu actions (Watch later, history, not interested) change the lists.
        bool menu = menu_active();
        if (dirty_ || (menu_open_ && !menu)) refresh();
        menu_open_ = menu;
        page_.begin(id(g, "page"));
        float y = page_.y(56);
        text::draw(font::headline, x0, y, "Library", t.text);
        text::draw(font::body, x0, y + 46, "Stored on this console", t.text2);
        y += 100;

        if (later_.empty() && history_.empty() && playlists_.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 280), ic::VIDEO_LIBRARY, "Nothing here yet",
                        "Videos you watch show up here. Press X on a video to save it to Watch later.");
            page_.end(y + 330 + page_.scroll());
            hint_bar({{"B", "Back"}});
            return;
        }
        if (!later_.empty()) y += videos(id(g, "later"), x0, y, "Watch later", later_, true) + 12;
        if (!playlists_.empty()) {
            ShelfSpec s;
            s.title = "Saved playlists";
            s.count = (int)playlists_.size();
            s.item_w = 300;
            s.item = [this](int i) { return playlist_card(playlists_[i]); };
            s.on_click = [this](int i) { app::push(make_playlist(playlists_[i].id, playlists_[i].title)); };
            s.on_focus = [this](int i) { set_backdrop(playlists_[i].thumbnail); };
            s.on_x = [this](int i) {
                youtube::Playlist p = playlists_[i];
                show_menu(p.title, p.channel, {{"Remove from library", ic::REMOVE, [p] { set_playlist_saved(p, false); }}});
            };
            y += shelf(id(g, "playlists"), x0, y, s, &page_) + 12;
        }
        if (!history_.empty()) {
            y += videos(id(g, "history"), x0, y, "History", history_, false) + 12;
            Id ag = id(g, "actions");
            float bw = measure_button("Clear history", ic::DELETE);
            if (button(id(ag, "clear"), Rect(x0, y, bw, 48), "Clear history", ic::DELETE, BTN_GHOST, ag)) {
                clear_history();
                toast("History cleared", ic::DELETE);
                dirty_ = true;
            }
            if (focus_in_group(ag)) page_.focus_range(y + page_.scroll() - 60, y + page_.scroll() + 70);
            y += 70;
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Play"}, {"X", "More"}, {"B", "Back"}});
    }

private:
    float videos(Id sid, float x, float y, const char* title, const std::vector<youtube::Video>& list, bool later) {
        ShelfSpec s;
        s.title = title;
        s.count = (int)list.size();
        s.item_w = 300;
        s.item = [&list](int i) { return video_card(list[i]); };
        s.on_click = [this, &list](int i) {
            youtube::Video v = list[i];
            play(v);
            dirty_ = true;
        };
        s.on_focus = [&list](int i) { set_backdrop(youtube::thumbnail_hq(list[i].id)); };
        s.on_x = [&list, later](int i) {
            youtube::Video v = list[i];
            MenuOptions o;
            if (!later) o.extra.push_back({"Remove from history", ic::HISTORY, [v] { remove_from_history(v.id); }});
            video_menu(v, o);
        };
        return shelf(sid, x, y, s, &page_);
    }

    void refresh() {
        later_ = watch_later();
        history_ = history();
        playlists_ = saved_playlists();
        dirty_ = false;
    }

    std::vector<youtube::Video> later_, history_;
    std::vector<youtube::Playlist> playlists_;
    bool dirty_ = false, menu_open_ = false;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_channel(const std::string& id, const std::string& name, const std::string& avatar) {
    return std::make_unique<ChannelScreen>(id, name, avatar);
}

std::unique_ptr<app::Screen> make_playlist(const std::string& id, const std::string& title) {
    return std::make_unique<PlaylistScreen>(id, title);
}

std::unique_ptr<app::Screen> make_subscriptions() { return std::make_unique<SubscriptionsScreen>(); }
std::unique_ptr<app::Screen> make_library() { return std::make_unique<LibraryScreen>(); }

}  // namespace yt
}  // namespace screens
