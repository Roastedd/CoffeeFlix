#include "services/ryd.hpp"

#include <chrono>
#include <map>
#include <mutex>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace ryd {

namespace {

struct Entry {
    Votes votes;
    double at = 0;
};
std::mutex g_mutex;
std::map<std::string, Entry> g_cache;
double g_pause_until = 0;  // after "too many requests"

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool valid_id(const std::string& id) {
    if (id.size() != 11) return false;
    for (char c : id)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    return true;
}

}  // namespace

bool enabled() { return store::get_bool("yt_ryd", true); }

Votes parse(const std::string& body) {
    Votes v;
    json::Doc doc = json::Doc::parse(body);
    if (!doc || !json_is_object(doc.get())) return v;
    const int64_t likes = json::num(doc.get(), {"likes"}, -1), dislikes = json::num(doc.get(), {"dislikes"}, -1);
    if (likes < 0 || dislikes < 0) return v;
    v.ok = true;
    v.likes = likes;
    v.dislikes = dislikes;
    return v;
}

Votes fetch(const std::string& video_id, std::string& error) {
    if (!valid_id(video_id)) return {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(video_id);
        // A good answer is kept ten minutes, a failed one two, so a down service isn't asked for every frame.
        if (it != g_cache.end() && now() - it->second.at < (it->second.votes.ok ? 600 : 120)) return it->second.votes;
        if (now() < g_pause_until) return {};
    }
    http::Response r = http::get(util::env_or("COFFEEFLIX_RYD_API", "https://returnyoutubedislikeapi.com") +
                                 "/votes?videoId=" + video_id, {{"User-Agent", "CoffeeFlix/2.0"}}, 8);
    Votes v;
    if (r.ok()) v = parse(r.body);
    else error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (r.status == 429) g_pause_until = now() + 60;
    if (g_cache.size() > 200) g_cache.clear();
    g_cache[video_id] = {v, now()};
    return v;
}

}  // namespace ryd
