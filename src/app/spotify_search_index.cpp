#include "app/spotify_search_index.hpp"

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/spotify/spotify.hpp"
#include "services/spotify/spotify_index.hpp"

namespace spotify_search_index {

namespace {

constexpr double EVERY = 30 * 60;  // seconds between updates (revisions make an update cheap)

tasks::Scope g_scope;
bool g_running = false;
double g_last = -1e9;
int g_done = 0;
struct Todo {
    std::string uri, name, revision;
};
std::vector<Todo> g_todo;

void finish() {
    g_running = false;
    g_scope.run<bool>([] {
        spotify::index::save();
        return true;
    }, [](bool) {
        log_message(LOG_OK, "Spotify", "Search index: %zu songs in %zu playlists", spotify::index::songs(),
                    spotify::index::playlists());
    });
}

void next() {
    if ((size_t)g_done >= g_todo.size()) return finish();
    const Todo t = g_todo[g_done];
    const bool save = g_done % 10 == 9;  // now and then on the way: an update cut short keeps what it read
    g_scope.run<bool>([t, save] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // gently: playback and browsing come first
        std::vector<spotify::Item> items;
        std::string error;
        spotify::Parts parts = spotify::list_parts(t.uri);
        for (;;) {
            spotify::Part part = parts(error);
            for (spotify::Item& it : part.items) items.push_back(std::move(it));
            if (!part.more) break;
        }
        if (items.empty() && !error.empty()) {  // tried again next update
            log_message(LOG_WARNING, "Spotify", "Search index: a playlist failed (%s)", error.c_str());
            return false;
        }
        spotify::index::put(t.uri, t.name, t.revision, std::move(items));
        if (save) spotify::index::save();
        return true;
    }, [](bool) {
        g_done++;
        next();
    });
}

}  // namespace

void maybe_run(bool force) {
    if (g_running || !spotify::signed_in() || store::kids_profile()) return;
    if (!force && util::now_seconds() - g_last < EVERY) return;
    g_running = true;
    g_last = util::now_seconds();
    g_done = 0;
    g_todo.clear();
    struct Listing {
        std::vector<spotify::Listed> lists;
        std::string liked;  // the liked songs' revision
        std::string error;
    };
    g_scope.run<Listing>([] {
        Listing l;
        l.lists = spotify::playlists_with_revisions(l.error);
        std::string e;
        l.liked = spotify::list_revision(spotify::LIKED, e);
        return l;
    }, [](Listing l) {
        if (l.lists.empty()) {
            g_running = false;
            if (!l.error.empty()) log_message(LOG_WARNING, "Spotify", "Search index: %s", l.error.c_str());
            return;
        }
        std::set<std::string> all = {spotify::LIKED};
        if (!l.liked.empty() && l.liked != spotify::index::revision(spotify::LIKED))  // the liked songs first
            g_todo.push_back(Todo{spotify::LIKED, "", l.liked});
        for (const spotify::Listed& p : l.lists) {
            all.insert(p.playlist.uri);
            if (p.revision.empty() || p.revision != spotify::index::revision(p.playlist.uri))
                g_todo.push_back(Todo{p.playlist.uri, p.playlist.name, p.revision});
        }
        spotify::index::keep_only(all);
        next();
    });
}

void stop() {
    g_scope.reset();
    g_running = false;
    g_todo.clear();
    g_done = 0;
    g_last = -1e9;
    spotify::index::forget();
}

bool running() { return g_running; }
int done() { return g_done; }
int total() { return (int)g_todo.size(); }

}  // namespace spotify_search_index
