#include "app/spotify_podcasts.hpp"

#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/podcasts.hpp"
#include "services/spotify/spotify.hpp"
#include "ui/ui.hpp"

namespace spotify_podcasts {

namespace {

constexpr const char* CHOICE_KEY = "sp_podcast_sync";  // Choice
constexpr const char* AT_KEY = "sp_podcast_sync_at";   // unix time of the last sync
// What the syncs saw: show URI -> {"f": its feed ("" none: only on Spotify), "a": 1 when the sync subscribed it}.
constexpr const char* MAP_KEY = "sp_podcast_map";
constexpr int64_t EVERY = 6 * 3600;
constexpr int64_t RETRY = 10 * 60;  // after a search that didn't get through

struct Entry {
    std::string feed;
    bool added = false;
};
using Map = std::map<std::string, Entry>;

Map load_map() {
    Map m;
    json::Doc d = json::Doc::parse(store::get_str(MAP_KEY, "{}"));
    const char* key;
    json_t* v;
    if (json_is_object(d.get()))
        json_object_foreach(d.get(), key, v) m[key] = Entry{json::str(v, {"f"}), json::boolean(v, {"a"})};
    return m;
}

void save_map(const Map& m) {
    json_t* o = json_object();
    for (const auto& [uri, e] : m) {
        json_t* v = json_object();
        json_object_set_new(v, "f", json_string(e.feed.c_str()));
        if (e.added) json_object_set_new(v, "a", json_true());
        json_object_set_new(o, uri.c_str(), v);
    }
    store::set_str(MAP_KEY, json::dump(o));
    json_decref(o);
}

bool g_running = false;
tasks::Scope g_scope;

struct Plan {
    bool ok = false;
    Map map;
    std::vector<podcasts::Show> subscribe;
    std::vector<std::string> unsubscribe;  // feeds
    int only_spotify = 0;
    int failed = 0;  // searches that didn't get through: those shows are tried again soon
};

// Worker: what changed on Spotify since `map`. Network only; the store is left to the main thread.
Plan plan(Map map, std::set<std::string> subscribed) {
    Plan p;
    std::string error;
    const std::vector<spotify::Item> followed = spotify::followed_shows(error);
    if (followed.empty() && !error.empty()) {
        log_message(LOG_WARNING, "Spotify", "Podcast sync: %s", error.c_str());
        return p;
    }
    std::set<std::string> now;
    int searches = 0;
    for (const spotify::Item& show : followed) {
        now.insert(show.uri);
        if (map.count(show.uri)) continue;  // seen before: left as the viewer has it now
        // Apple's search allows about 20 a minute: the first sync of many shows takes its time.
        if (searches++ > 0) std::this_thread::sleep_for(std::chrono::seconds(3));
        podcasts::Show feed;
        bool searched = true;
        bool found = spotify::find_feed(show, feed, &searched);
        if (!found && !searched) {  // the network gone for a moment: once more
            std::this_thread::sleep_for(std::chrono::seconds(5));
            searched = true;
            found = spotify::find_feed(show, feed, &searched);
        }
        if (found) {
            Entry e{feed.feed_url, false};
            if (!subscribed.count(feed.feed_url)) {
                p.subscribe.push_back(feed);
                subscribed.insert(feed.feed_url);
                e.added = true;
            }
            map[show.uri] = e;
        } else if (searched) {
            map[show.uri] = Entry{"", false};  // only on Spotify (or not surely the same): asked again if unfollowed
        } else {
            p.failed++;  // not in the map: tried again next time
        }
    }
    for (auto it = map.begin(); it != map.end();) {
        if (now.count(it->first)) {
            if (it->second.feed.empty()) p.only_spotify++;
            ++it;
            continue;
        }
        if (it->second.added && subscribed.count(it->second.feed)) p.unsubscribe.push_back(it->second.feed);
        it = map.erase(it);
    }
    p.map = std::move(map);
    p.ok = true;
    return p;
}

std::set<std::string> subscribed_feeds() {
    std::set<std::string> out;
    for (const podcasts::Show& s : podcasts::subscriptions()) out.insert(s.feed_url);
    return out;
}

void unsubscribe(const std::string& feed) {
    if (!podcasts::is_subscribed(feed)) return;
    podcasts::Show s;
    s.feed_url = feed;
    podcasts::toggle_subscription(s);
}

}  // namespace

Choice choice() { return (Choice)store::get_int(CHOICE_KEY, UNASKED); }

void turn_on() {
    store::set_int(CHOICE_KEY, ON);
    maybe_sync(true);
}

void turn_off(bool remove_synced) {
    g_scope.reset();
    g_running = false;
    store::set_int(CHOICE_KEY, OFF);
    if (remove_synced)
        for (const auto& [uri, e] : load_map())
            if (e.added) unsubscribe(e.feed);
    store::set_str(MAP_KEY, "");
    store::set_int(AT_KEY, 0);
}

bool syncing() { return g_running; }

void maybe_sync(bool force) {
    if (g_running || choice() != ON || !spotify::signed_in() || store::kids_profile()) return;
    const int64_t now = (int64_t)time(nullptr);
    if (!force && now - store::get_int(AT_KEY, 0) < EVERY) return;
    g_running = true;
    const bool first = load_map().empty();
    if (first) ui::toast(tr("Taking over your podcasts from Spotify\xE2\x80\xA6"), ic::PODCASTS);
    g_scope.run<Plan>([map = load_map(), subs = subscribed_feeds()] { return plan(map, subs); }, [now](Plan p) {
        g_running = false;
        if (!p.ok || choice() != ON) return;
        int added = 0, removed = 0;
        for (const podcasts::Show& s : p.subscribe)
            if (!podcasts::is_subscribed(s.feed_url) && podcasts::toggle_subscription(s)) added++;
        for (const std::string& feed : p.unsubscribe)
            if (podcasts::is_subscribed(feed)) unsubscribe(feed), removed++;
        save_map(p.map);
        store::set_int(AT_KEY, p.failed ? now - EVERY + RETRY : now);
        log_message(LOG_OK, "Spotify", "Podcast sync: %d subscribed, %d unsubscribed, %d only on Spotify, %d not reached",
                    added, removed, p.only_spotify, p.failed);
        if (added && removed)
            ui::toast(util::fmt(tr("Spotify: %d podcasts added, %d removed"), added, removed), ic::PODCASTS);
        else if (added)
            ui::toast(util::fmt(added == 1 ? tr("Spotify: 1 podcast added to your subscriptions")
                                       : tr("Spotify: %d podcasts added to your subscriptions"), added),
                  ic::PODCASTS);
        else if (removed)
            ui::toast(util::fmt(removed == 1 ? tr("Spotify: 1 podcast you unfollowed removed")
                                         : tr("Spotify: %d podcasts you unfollowed removed"), removed),
                  ic::PODCASTS);
    });
}

std::set<std::string> taken_over() {
    std::set<std::string> out;
    for (const auto& [uri, e] : load_map())
        if (!e.feed.empty()) out.insert(uri);
    return out;
}

}  // namespace spotify_podcasts
