#include "services/yt_recs.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>

#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace yt_recs {

namespace {

constexpr int64_t HOUR = 3600, DAY = 24 * HOUR;
constexpr size_t MAX_TOPICS = 300, MAX_HISTORY = 60, MAX_QUERIES = 6, MAX_SEARCHES = 10;
constexpr size_t FEED_SIZE = 30;

struct Watch {
    std::string id, title, channel, channel_id;
    double fraction = 0;
    int64_t when = 0, used_as_seed = 0;
};

// Everything learned. Topics are words from titles and searches; channels are affinities.
struct Brain {
    std::map<std::string, double> topics, channels;
    std::vector<Watch> history;                              // enjoyed videos, newest first
    std::map<std::string, std::pair<int, int64_t>> seen;     // shown in the feed: {times, last}
    std::map<std::string, int64_t> blocked;                  // "not interested": id or "c:<channel>" -> until
    std::vector<std::string> queries, searches;              // discovery queries used; user searches
};

std::mutex g_m;
Brain g_b;
bool g_loaded = false;
std::atomic<int> g_version{0};

int64_t now() { return (int64_t)time(nullptr); }

// --- storage ------------------------------------------------------------------------

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    json::Doc doc = json::Doc::parse(store::get_str("yt_brain"));
    json_t* r = doc.get();
    if (!r) return;
    const char* k;
    json_t* v;
    json_object_foreach(json_object_get(r, "topics"), k, v) g_b.topics[k] = json_number_value(v);
    json_object_foreach(json_object_get(r, "channels"), k, v) g_b.channels[k] = json_number_value(v);
    json_object_foreach(json_object_get(r, "seen"), k, v)
        g_b.seen[k] = {(int)json_integer_value(json_array_get(v, 0)), json_integer_value(json_array_get(v, 1))};
    json_object_foreach(json_object_get(r, "blocked"), k, v) g_b.blocked[k] = json_integer_value(v);
    json_t* h = json_object_get(r, "history");
    for (size_t i = 0; i < json::size(h); i++) {
        json_t* e = json_array_get(h, i);
        Watch w;
        w.id = json::str(e, {"id"});
        w.title = json::str(e, {"title"});
        w.channel = json::str(e, {"channel"});
        w.channel_id = json::str(e, {"channel_id"});
        w.fraction = json::real(e, {"fraction"});
        w.when = json::num(e, {"when"});
        w.used_as_seed = json::num(e, {"seed"});
        if (!w.id.empty()) g_b.history.push_back(w);
    }
    for (const char* key : {"queries", "searches"}) {
        auto& out = std::string(key) == "queries" ? g_b.queries : g_b.searches;
        json_t* a = json_object_get(r, key);
        for (size_t i = 0; i < json::size(a); i++) out.push_back(json::str(json_array_get(a, i)));
    }
}

void save_locked() {
    json_t* r = json_object();
    json_t* o = json_object();
    for (auto& [k, v] : g_b.topics) json_object_set_new(o, k.c_str(), json_real(v));
    json_object_set_new(r, "topics", o);
    o = json_object();
    for (auto& [k, v] : g_b.channels) json_object_set_new(o, k.c_str(), json_real(v));
    json_object_set_new(r, "channels", o);
    o = json_object();
    for (auto& [k, v] : g_b.seen) json_object_set_new(o, k.c_str(), json_pack("[iI]", v.first, (json_int_t)v.second));
    json_object_set_new(r, "seen", o);
    o = json_object();
    for (auto& [k, v] : g_b.blocked) json_object_set_new(o, k.c_str(), json_integer(v));
    json_object_set_new(r, "blocked", o);
    json_t* h = json_array();
    for (auto& w : g_b.history)
        json_array_append_new(h, json_pack("{sssssssssfsIsI}", "id", w.id.c_str(), "title", w.title.c_str(), "channel",
                                           w.channel.c_str(), "channel_id", w.channel_id.c_str(), "fraction", w.fraction,
                                           "when", (json_int_t)w.when, "seed", (json_int_t)w.used_as_seed));
    json_object_set_new(r, "history", h);
    for (const char* key : {"queries", "searches"}) {
        json_t* a = json_array();
        for (auto& q : std::string(key) == "queries" ? g_b.queries : g_b.searches) json_array_append_new(a, json_string(q.c_str()));
        json_object_set_new(r, key, a);
    }
    store::set_str("yt_brain", json::dump(r));
    json_decref(r);
}

// A signal changed what the feed should show.
void learned_locked() {
    save_locked();
    g_version++;
}

// --- learning -----------------------------------------------------------------------

// Words that say nothing about what a video is about.
const std::set<std::string> STOP = {
    "the", "and", "for", "with", "you", "your", "this", "that", "from", "are", "was", "how", "what", "why", "who",
    "when", "not", "but", "all", "can", "its", "our", "out", "get", "got", "his", "her", "they", "them", "just", "will",
    "into", "about", "more", "most", "best", "top", "one", "two", "vs", "new", "full", "official", "video", "videos",
    "audio", "lyric", "lyrics", "music", "song", "hd", "4k", "hq", "live", "ft", "feat", "episode", "part", "shorts",
    "short", "watch", "day", "year", "time", "ever", "every", "like", "make", "made", "now", "ep", "vol", "remaster",
    "remastered", "trailer", "clip", "reaction", "review", "compilation", "highlights", "update", "des", "les", "der",
    "die", "und", "que", "los", "las", "con", "por", "para"};

std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] {
        bool digits = std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); });
        if (w.size() >= 3 && !digits && !STOP.count(w) && std::find(out.begin(), out.end(), w) == out.end())
            out.push_back(w);
        w.clear();
    };
    for (unsigned char c : text) {
        if (c >= 0x80) w += (char)c;  // keep UTF-8 words whole
        else if (std::isalnum(c)) w += (char)std::tolower(c);
        else flush();
    }
    flush();
    return out;
}

void learn(Brain& b, const std::string& text, double amount) {
    auto t = tokens(text);
    if (t.empty()) return;
    double per = amount / std::sqrt((double)t.size());
    for (auto& k : t) b.topics[k] += per;
}

// Older interests fade so the feed follows what you're into now.
void decay(Brain& b) {
    for (auto it = b.topics.begin(); it != b.topics.end();) {
        it->second *= 0.99;
        it = std::fabs(it->second) < 0.02 ? b.topics.erase(it) : std::next(it);
    }
    if (b.topics.size() > MAX_TOPICS) {
        std::vector<std::pair<double, std::string>> v;
        for (auto& [k, w] : b.topics) v.push_back({std::fabs(w), k});
        std::sort(v.begin(), v.end());
        for (size_t i = 0; i < v.size() - MAX_TOPICS; i++) b.topics.erase(v[i].second);
    }
}

void bump_channel(Brain& b, const std::string& id, double delta, double lo = -1.0, double hi = 1.5) {
    if (id.empty()) return;
    b.channels[id] = std::clamp(b.channels[id] + delta, lo, hi);
}

double parse_duration(const std::string& s) {
    double total = 0, part = 0;
    bool any = false;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) {
            part = part * 10 + (c - '0');
            any = true;
        } else if (c == ':') {
            total = total * 60 + part;
            part = 0;
        }
    }
    return any ? total * 60 + part : 0;
}

// --- feed ---------------------------------------------------------------------------

double interest(const Brain& b, const youtube::Video& v) {
    auto t = tokens(v.title);
    if (t.empty()) return 0;
    double sum = 0;
    for (auto& k : t) {
        auto it = b.topics.find(k);
        if (it != b.topics.end()) sum += it->second;
    }
    return std::clamp(sum / std::sqrt((double)t.size()), -1.0, 2.0);
}

// Share of the shorter title's words found in the other: catches re-uploads, lyric videos and
// covers of the same thing.
bool same_thing(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() < 2 || b.size() < 2) return false;
    size_t common = 0;
    for (auto& w : a) common += std::find(b.begin(), b.end(), w) != b.end();
    return common >= 0.7 * std::min(a.size(), b.size());
}

// Weighted pick of up to n distinct indices.
std::vector<size_t> pick(std::vector<double> weights, size_t n, std::mt19937& rng) {
    std::vector<size_t> out;
    while (out.size() < n) {
        double total = 0;
        for (double w : weights) total += std::max(w, 0.0);
        if (total <= 0) break;
        double r = std::uniform_real_distribution<double>(0, total)(rng);
        for (size_t i = 0; i < weights.size(); i++) {
            r -= std::max(weights[i], 0.0);
            if (r <= 0 && weights[i] > 0) {
                out.push_back(i);
                weights[i] = 0;
                break;
            }
        }
    }
    return out;
}

enum Source { RELATED, SUBSCRIPTION, DISCOVERY, POPULAR };

}  // namespace

void on_watch(const youtube::Video& v, double watched, bool finished) {
    if (v.id.empty() || (!finished && watched < 10)) return;  // opened by accident
    double dur = parse_duration(v.duration);
    double frac = finished ? 1.0 : dur > 0 ? std::min(1.0, watched / dur) : std::min(1.0, watched / 240.0);
    bool enjoyed = frac >= 0.2 || watched >= 180;
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    decay(g_b);
    if (enjoyed) {
        double s = 0.15 * frac + 0.05 * std::log1p(watched / 60.0);
        learn(g_b, v.title, s);
        bump_channel(g_b, v.channel_id, s);
        g_b.history.erase(std::remove_if(g_b.history.begin(), g_b.history.end(), [&](const Watch& w) { return w.id == v.id; }),
                          g_b.history.end());
        g_b.history.insert(g_b.history.begin(), Watch{v.id, v.title, v.channel, v.channel_id, frac, now(), 0});
        if (g_b.history.size() > MAX_HISTORY) g_b.history.resize(MAX_HISTORY);
    } else {  // backed out early: a skip
        learn(g_b, v.title, -0.08);
        bump_channel(g_b, v.channel_id, -0.05);
    }
    learned_locked();
}

void on_search(const std::string& query) {
    std::string q = util::trim(query);
    if (q.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    learn(g_b, q, 0.1);
    g_b.searches.erase(std::remove(g_b.searches.begin(), g_b.searches.end(), q), g_b.searches.end());
    g_b.searches.insert(g_b.searches.begin(), q);
    if (g_b.searches.size() > MAX_SEARCHES) g_b.searches.resize(MAX_SEARCHES);
    learned_locked();
}

void on_subscribe(const std::string& channel_id, bool subscribed) {
    if (channel_id.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    double& c = g_b.channels[channel_id];
    c = subscribed ? std::max(c, 0.65) : std::min(c, 0.2);
    learned_locked();
}

void not_interested(const youtube::Video& v) {
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    g_b.blocked[v.id] = now() + 30 * DAY;
    if (!v.channel_id.empty()) {
        g_b.blocked["c:" + v.channel_id] = now() + 14 * DAY;
        bump_channel(g_b, v.channel_id, -0.5);
    }
    learn(g_b, v.title, -0.15);
    learned_locked();
}

bool has_profile() {
    {
        std::lock_guard<std::mutex> lk(g_m);
        load_locked();
        if (!g_b.history.empty() || !g_b.searches.empty()) return true;
    }
    return !store::favs("yt_channel").empty();
}

int version() { return g_version.load(); }

void reset() {
    std::lock_guard<std::mutex> lk(g_m);
    g_b = Brain();
    g_loaded = true;
    learned_locked();
}

youtube::Results for_you() {
    Brain b;
    {
        std::lock_guard<std::mutex> lk(g_m);
        load_locked();
        b = g_b;
    }
    const int64_t t = now();
    std::mt19937 rng((unsigned)(t ^ (version() * 2654435761u)));

    // Seeds: videos you enjoyed, favouring recent ones; one used in the last 6 hours is much less
    // likely to be picked again (but still can be, so a small history doesn't run dry).
    std::vector<double> sw;
    for (auto& w : b.history)
        sw.push_back(((0.3 + w.fraction) * std::exp(-(double)(t - w.when) / (3.0 * DAY)) + 0.02) *
                     (t - w.used_as_seed < 6 * HOUR ? 0.15 : 1.0));
    std::vector<size_t> seeds = pick(sw, 3, rng);

    // Subscriptions, favouring the channels you watch most.
    auto subs = store::favs("yt_channel");
    std::vector<double> cw;
    for (auto& f : subs) {
        auto it = b.channels.find(f.id);
        cw.push_back(1.0 + (it != b.channels.end() ? std::max(it->second, 0.0) : 0.0));
    }
    std::vector<size_t> chans = pick(cw, 3, rng);

    // Discovery: your own recent searches, and searches made from the strongest words of a video you
    // enjoyed (words from one title belong together; mixing titles gives nonsense). Rotated so the
    // same searches don't come back every time.
    std::vector<std::string> queries;
    auto fresh = [&](const std::string& q) {
        return !q.empty() && std::find(b.queries.begin(), b.queries.end(), q) == b.queries.end() &&
               std::find(queries.begin(), queries.end(), q) == queries.end();
    };
    for (auto& s : b.searches)
        if (queries.size() < 2 && fresh(s)) queries.push_back(s);
    std::vector<double> qw;
    for (auto& w : b.history) qw.push_back((0.3 + w.fraction) * std::exp(-(double)(t - w.when) / (3.0 * DAY)) + 0.02);
    for (size_t i : pick(qw, 2, rng)) {
        auto words = tokens(b.history[i].title);
        std::stable_sort(words.begin(), words.end(), [&](const std::string& x, const std::string& y) {
            auto wx = b.topics.find(x), wy = b.topics.find(y);
            return (wx != b.topics.end() ? wx->second : 0) > (wy != b.topics.end() ? wy->second : 0);
        });
        if (words.size() > 3) words.resize(3);
        std::string q;
        for (auto& w : words) q += (q.empty() ? "" : " ") + w;
        if (queries.size() < 3 && words.size() >= 2 && fresh(q)) queries.push_back(q);
    }
    // Nothing new to try: reuse the oldest searches rather than going without.
    for (auto it = b.queries.rbegin(); it != b.queries.rend() && queries.size() < 2; ++it)
        if (std::find(queries.begin(), queries.end(), *it) == queries.end()) queries.push_back(*it);

    // Fetch everything in parallel.
    struct Job {
        Source source;
        std::future<youtube::Results> result;
    };
    std::vector<Job> jobs;
    for (size_t i : seeds)
        jobs.push_back({RELATED, std::async(std::launch::async, youtube::related, b.history[i].id)});
    for (size_t i : chans)
        jobs.push_back({SUBSCRIPTION, std::async(std::launch::async, [id = subs[i].id] { return youtube::channel_videos(id); })});
    for (auto& q : queries)
        jobs.push_back({DISCOVERY, std::async(std::launch::async, [q] { return youtube::search(q, youtube::PARAMS_VIDEOS); })});
    // Little to go on yet (nothing watched to find related videos for): fill up with what's popular.
    if (seeds.empty() || jobs.size() < 4) jobs.push_back({POPULAR, std::async(std::launch::async, youtube::trending)});

    struct Candidate {
        youtube::Video v;
        double score = 0;
    };
    std::map<std::string, Candidate> cands;
    std::string first_error;
    std::set<std::string> watched;
    for (auto& w : b.history) watched.insert(w.id);
    auto blocked = [&](const std::string& key) {
        auto it = b.blocked.find(key);
        return it != b.blocked.end() && it->second > t;
    };
    for (auto& j : jobs) {
        youtube::Results r = j.result.get();
        if (!r.ok && first_error.empty()) first_error = r.error;
        double bonus = j.source == RELATED ? 0.25 : j.source == SUBSCRIPTION ? 0.3 : j.source == DISCOVERY ? 0.1 : 0.0;
        size_t limit = j.source == SUBSCRIPTION ? 6 : 20;
        for (size_t k = 0; k < r.items.size() && k < limit; k++) {
            youtube::Video& v = r.items[k];
            if (watched.count(v.id) || blocked(v.id) || (!v.channel_id.empty() && blocked("c:" + v.channel_id))) continue;
            auto it = cands.find(v.id);
            if (it != cands.end()) {
                it->second.score += 0.1;  // suggested from more than one direction
                continue;
            }
            cands[v.id] = Candidate{v, bonus};
        }
    }

    // Score: how well it matches your interests and channels, plus exploration noise; things you've
    // already been shown recently sink, and repeatedly ignored ones drop out for a while.
    std::vector<Candidate> ranked, resting;
    std::uniform_real_distribution<double> jitter(0, 0.1);
    for (auto& [id, c] : cands) {
        auto ch = b.channels.find(c.v.channel_id);
        c.score += interest(b, c.v) + (ch != b.channels.end() ? ch->second * 0.6 : 0) + jitter(rng);
        auto s = b.seen.find(id);
        if (s != b.seen.end() && t - s->second.second < 6 * HOUR) c.score *= c.score > 0 ? 0.5 : 1.5;
        bool tired = s != b.seen.end() && s->second.first >= 3 && t - s->second.second < 60 * HOUR;
        (tired ? resting : ranked).push_back(c);
    }
    auto by_score = [](const Candidate& a, const Candidate& b) { return a.score > b.score; };
    std::sort(ranked.begin(), ranked.end(), by_score);
    std::sort(resting.begin(), resting.end(), by_score);

    // Variety: at most two videos per channel, and nothing that's the same thing as a video you
    // watched or one already picked.
    youtube::Results out;
    std::map<std::string, int> per_channel;
    std::vector<std::vector<std::string>> taken;
    for (auto& w : b.history) taken.push_back(tokens(w.title));
    std::set<std::string> picked;
    // Looser passes when the first leaves the feed thin (e.g. most candidates come from a few
    // subscriptions): more per channel, then the resting videos after all.
    struct Pass {
        const std::vector<Candidate>* pool;
        int max_per_channel;
    };
    for (const Pass& pass : {Pass{&ranked, 2}, Pass{&ranked, 4}, Pass{&resting, 4}}) {
        if (pass.pool != &ranked || pass.max_per_channel > 2)
            if (out.items.size() >= FEED_SIZE / 2) break;
        int max_per_channel = pass.max_per_channel;
        for (auto& c : *pass.pool) {
            if (out.items.size() >= FEED_SIZE) break;
            if (picked.count(c.v.id)) continue;
            std::string ch = c.v.channel_id.empty() ? c.v.channel : c.v.channel_id;
            auto words = tokens(c.v.title);
            if (per_channel[ch] >= max_per_channel ||
                std::any_of(taken.begin(), taken.end(), [&](const std::vector<std::string>& t) { return same_thing(words, t); }))
                continue;
            per_channel[ch]++;
            picked.insert(c.v.id);
            taken.push_back(std::move(words));
            out.items.push_back(c.v);
        }
    }
    out.ok = !out.items.empty() || first_error.empty();
    if (!out.ok) out.error = first_error;

    {
        std::lock_guard<std::mutex> lk(g_m);
        // Only the first few are what you actually see without scrolling.
        for (size_t i = 0; i < out.items.size() && i < 10; i++) {
            auto& s = g_b.seen[out.items[i].id];
            s.first++;
            s.second = t;
        }
        for (auto it = g_b.seen.begin(); it != g_b.seen.end();)
            it = t - it->second.second > 3 * DAY ? g_b.seen.erase(it) : std::next(it);
        for (auto it = g_b.blocked.begin(); it != g_b.blocked.end();)
            it = it->second <= t ? g_b.blocked.erase(it) : std::next(it);
        for (size_t i : seeds)
            for (auto& w : g_b.history)
                if (w.id == b.history[i].id) w.used_as_seed = t;
        for (auto& q : queries) {
            g_b.queries.erase(std::remove(g_b.queries.begin(), g_b.queries.end(), q), g_b.queries.end());
            g_b.queries.insert(g_b.queries.begin(), q);
        }
        if (g_b.queries.size() > MAX_QUERIES) g_b.queries.resize(MAX_QUERIES);
        save_locked();
    }
    log_message(LOG_OK, "YouTube", "For you: %zu videos from %zu seeds, %zu channels, %zu searches (%zu candidates)",
                out.items.size(), seeds.size(), chans.size(), queries.size(), cands.size());
    return out;
}

}  // namespace yt_recs
