#include "services/spotify/spotify_index.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_set>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace spotify::index {

namespace {

// One song: what the search shows and plays. (Its picture comes when it plays.)
struct Song {
    std::string uri, name, artists;
    int64_t duration_ms = 0;
};
struct List {
    std::string name, revision;
    std::vector<Song> songs;
};

std::mutex g_mu;
bool g_loaded = false;
std::string g_user;                    // whose index it is
std::map<std::string, List> g_lists;   // by playlist URI
bool g_dirty = false;

std::string path() { return platform::data_dir() + "/spotify_index.json"; }

// Under g_mu: the card's index, once; none (or another account's) is an empty one.
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    g_user = user_name();
    std::string text;
    if (!util::read_file(path(), text)) return;
    json::Doc d = json::Doc::parse(text);
    if (json::str(d.get(), {"user"}) != g_user) return;
    json_t* lists = json::at(d.get(), {"lists"});
    const char* uri;
    json_t* l;
    json_object_foreach(lists, uri, l) {
        List list;
        list.name = json::str(l, {"name"});
        list.revision = json::str(l, {"rev"});
        json_t* songs = json::at(l, {"songs"});
        for (size_t i = 0; i < json::size(songs); i++) {
            json_t* s = json_array_get(songs, i);  // [uri, name, artists, duration]
            list.songs.push_back(Song{json::str(json_array_get(s, 0)), json::str(json_array_get(s, 1)),
                                      json::str(json_array_get(s, 2)), json::num(json_array_get(s, 3))});
        }
        g_lists[uri] = std::move(list);
    }
}

// Signed in as someone else since: start over.
void check_user_locked() {
    load_locked();
    if (g_user == user_name()) return;
    g_user = user_name();
    g_lists.clear();
    g_dirty = true;
}

}  // namespace

std::string revision(const std::string& playlist_uri) {
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    auto it = g_lists.find(playlist_uri);
    return it == g_lists.end() ? "" : it->second.revision;
}

void put(const std::string& playlist_uri, const std::string& name, const std::string& revision, std::vector<Item> items) {
    List list;
    list.name = name;
    list.revision = revision;
    list.songs.reserve(items.size());
    for (Item& it : items)
        if (util::starts_with(it.uri, "spotify:track:"))
            list.songs.push_back(Song{std::move(it.uri), std::move(it.name), std::move(it.subtitle), it.duration_ms});
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    g_lists[playlist_uri] = std::move(list);
    g_dirty = true;
}

void keep_only(const std::set<std::string>& playlist_uris) {
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    for (auto it = g_lists.begin(); it != g_lists.end();) {
        if (playlist_uris.count(it->first)) {
            ++it;
        } else {
            it = g_lists.erase(it);
            g_dirty = true;
        }
    }
}

void save() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_dirty || !g_loaded) return;
        json_t* root = json_object();
        json_object_set_new(root, "user", json_string(g_user.c_str()));
        json_t* lists = json_object();
        for (const auto& [uri, list] : g_lists) {
            json_t* l = json_object();
            json_object_set_new(l, "name", json_string(list.name.c_str()));
            json_object_set_new(l, "rev", json_string(list.revision.c_str()));
            json_t* songs = json_array();
            for (const Song& s : list.songs) {
                json_t* a = json_array();
                json_array_append_new(a, json_string(s.uri.c_str()));
                json_array_append_new(a, json_string(s.name.c_str()));
                json_array_append_new(a, json_string(s.artists.c_str()));
                json_array_append_new(a, json_integer(s.duration_ms));
                json_array_append_new(songs, a);
            }
            json_object_set_new(l, "songs", songs);
            json_object_set_new(lists, uri.c_str(), l);
        }
        json_object_set_new(root, "lists", lists);
        char* dumped = json_dumps(root, JSON_COMPACT);
        if (dumped) text = dumped, free(dumped);
        json_decref(root);
        g_dirty = false;
    }
    if (text.empty() || !util::write_file_atomic(path(), text))
        log_message(LOG_WARNING, "Spotify", "Couldn't save the search index");
}

void forget() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_lists.clear();
    g_loaded = true;  // nothing to load: the file goes
    g_user.clear();
    g_dirty = false;
    std::remove(path().c_str());
}

size_t songs() {
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    size_t n = 0;
    for (const auto& [uri, list] : g_lists) n += list.songs.size();
    return n;
}

size_t playlists() {
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    return g_lists.size();
}

std::vector<Item> search(const std::vector<std::string>& words, size_t max, std::string (*fold)(const std::string&)) {
    std::vector<Item> out;
    std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lk(g_mu);
    check_user_locked();
    for (const auto& [uri, list] : g_lists) {
        for (const Song& s : list.songs) {
            if (seen.count(s.uri)) continue;
            const std::string text = fold(s.name + " " + s.artists);
            bool all = true;
            for (const std::string& w : words)
                if (text.find(w) == std::string::npos) {
                    all = false;
                    break;
                }
            if (!all) continue;
            seen.insert(s.uri);
            Item it;
            it.uri = s.uri;
            it.name = s.name;
            it.subtitle = s.artists.empty() ? util::fmt(tr("in %s"), list.name.c_str())
                                            : s.artists + " \xC2\xB7 " + util::fmt(tr("in %s"), list.name.c_str());
            it.duration_ms = s.duration_ms;
            out.push_back(std::move(it));
            if (out.size() >= max) return out;
        }
    }
    return out;
}

}  // namespace spotify::index
