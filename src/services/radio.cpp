#include "services/radio.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"

namespace radio {

namespace {

// Mirrors of the API; the first reachable one is remembered. "all" is the directory's own round robin over the servers that
// are up; fi1, nl1 and at1 no longer answer (each cost a 15 s timeout before the next was tried).
const char* SERVERS[] = {"https://all.api.radio-browser.info", "https://de1.api.radio-browser.info",
                         "https://de2.api.radio-browser.info"};
std::mutex g_m;
int g_server = 0;

Station parse(json_t* j) {
    Station s;
    s.uuid = json::str(j, {"stationuuid"});
    s.name = util::trim(json::str(j, {"name"}));
    s.url = json::str(j, {"url_resolved"});
    if (s.url.empty()) s.url = json::str(j, {"url"});
    s.favicon = json::str(j, {"favicon"});
    s.country = json::str(j, {"country"});
    s.countrycode = json::str(j, {"countrycode"});
    s.tags = json::str(j, {"tags"});
    s.codec = json::str(j, {"codec"});
    s.homepage = json::str(j, {"homepage"});
    s.bitrate = (int)json::num(j, {"bitrate"});
    s.votes = (int)json::num(j, {"votes"});
    s.hls = json::num(j, {"hls"}) != 0;
    return s;
}

// Codecs the player can decode (radio-browser lists many exotic ones).
bool playable(const Station& s) {
    if (s.url.empty() || s.name.empty()) return false;
    std::string c = util::lower(s.codec);
    return c.empty() || c == "mp3" || c == "aac" || c == "aac+" || c == "ogg" || c == "opus" || c == "flac" ||
           c == "unknown" || c.find("mp3") != std::string::npos || c.find("aac") != std::string::npos;
}

List fetch(const std::string& path) {
    if (store::kids_profile()) {  // the open directory isn't for a kids profile
        List blocked;
        blocked.error = tr("Not in a kids profile");
        return blocked;
    }
    List list;
    int start;
    {
        std::lock_guard<std::mutex> lk(g_m);
        start = g_server;
    }
    int n = (int)(sizeof(SERVERS) / sizeof(SERVERS[0]));
    for (int k = 0; k < n; k++) {
        int i = (start + k) % n;
        std::string base = util::env_or("COFFEEFLIX_RADIO_API", SERVERS[i]);
        http::Response r = http::get(base + path, {{"User-Agent", "CoffeeFlix/2.0"}}, 15);
        if (!r.ok()) {
            list.error = r.error;
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_m);
            g_server = i;
        }
        json::Doc doc = json::Doc::parse(r.body);
        for (size_t j = 0; j < json::size(doc.get()); j++) {
            Station s = parse(json_array_get(doc.get(), j));
            if (playable(s)) list.items.push_back(std::move(s));
        }
        list.ok = true;
        list.error.clear();
        return list;
    }
    return list;
}

std::string common(int limit) { return util::fmt("limit=%d&hidebroken=true&order=clickcount&reverse=true", limit); }

std::string serialize(const Station& s) {
    json_t* o = json_object();
    json_object_set_new(o, "url", json_string(s.url.c_str()));
    json_object_set_new(o, "country", json_string(s.country.c_str()));
    json_object_set_new(o, "tags", json_string(s.tags.c_str()));
    json_object_set_new(o, "codec", json_string(s.codec.c_str()));
    json_object_set_new(o, "bitrate", json_integer(s.bitrate));
    json_object_set_new(o, "hls", json_boolean(s.hls));
    std::string out = json::dump(o);
    json_decref(o);
    return out;
}

// Taste: genre tag -> weight, kept in the store and faded a little with every play so it follows what is played lately.
std::map<std::string, double> load_taste() {
    std::map<std::string, double> t;
    json::Doc d = json::Doc::parse(store::get_str("radio_taste", "{}"));
    const char* key;
    json_t* v;
    if (json_is_object(d.get()))
        json_object_foreach(d.get(), key, v) t[key] = json_number_value(v);
    return t;
}

// Tags worth learning from: not a country or a codec, not too short.
std::vector<std::string> tags_of(const Station& s) {
    std::vector<std::string> out;
    std::string cur;
    std::string all = util::lower(s.tags) + ",";
    for (char c : all) {
        if (c == ',') {
            cur = util::trim(cur);
            if (cur.size() >= 3 && cur.size() <= 24 && cur != util::lower(s.country) && cur != "radio" && cur != "fm" &&
                cur != "am" && cur != "online" && cur != "music" && cur != "internet" && cur != "stream" &&
                std::find(out.begin(), out.end(), cur) == out.end())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
        if (out.size() >= 6) break;
    }
    return out;
}

void learn(const Station& s, double amount) {
    std::vector<std::string> tags = tags_of(s);
    if (tags.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    auto t = load_taste();
    for (auto& kv : t) kv.second *= 0.97;
    for (auto& tag : tags) t[tag] += amount;
    // Keep the strongest 40.
    std::vector<std::pair<std::string, double>> v(t.begin(), t.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (v.size() > 40) v.resize(40);
    json_t* o = json_object();
    for (auto& kv : v)
        if (kv.second > 0.05) json_object_set_new(o, kv.first.c_str(), json_real(kv.second));
    store::set_str("radio_taste", json::dump(o));
    json_decref(o);
}

}  // namespace

bool has_taste() { return !load_taste().empty(); }

List for_you(int limit) {
    if (store::kids_profile()) return List{};
    List out;
    out.ok = true;
    std::map<std::string, double> t;
    {
        std::lock_guard<std::mutex> lk(g_m);
        t = load_taste();
    }
    std::vector<std::pair<std::string, double>> top_tags(t.begin(), t.end());
    std::sort(top_tags.begin(), top_tags.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (top_tags.size() > 4) top_tags.resize(4);
    std::set<std::string> have;
    for (auto& f : favorites()) have.insert(f.uuid);
    // Stations of each of the strongest tags, scored by how many of the learned tags they carry, then by votes.
    struct Scored { Station s; double score; };
    std::vector<Scored> pool;
    for (auto& tg : top_tags) {
        List l = by_tag(tg.first, 40);
        if (!l.ok) {
            if (pool.empty()) out.error = l.error;
            continue;
        }
        for (auto& s : l.items) {
            if (have.count(s.uuid)) continue;
            have.insert(s.uuid);
            double score = 0;
            for (auto& st : tags_of(s)) {
                auto it = t.find(st);
                if (it != t.end()) score += it->second;
            }
            score = score * 10 + std::min(s.votes, 5000) / 500.0;
            pool.push_back({std::move(s), score});
        }
    }
    std::sort(pool.begin(), pool.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    for (auto& p : pool) {
        if ((int)out.items.size() >= limit) break;
        out.items.push_back(std::move(p.s));
    }
    if (out.items.empty() && !out.error.empty()) out.ok = false;
    return out;
}

List top(int limit) { return fetch("/json/stations/topclick/" + std::to_string(limit) + "?hidebroken=true"); }

List by_country(const std::string& cc, int limit) {
    return fetch("/json/stations/bycountrycodeexact/" + util::url_encode(cc) + "?" + common(limit));
}

List by_tag(const std::string& tag, int limit) {
    return fetch("/json/stations/bytag/" + util::url_encode(tag) + "?" + common(limit));
}

List search(const std::string& q, int limit) {
    return fetch("/json/stations/search?name=" + util::url_encode(q) + "&" + common(limit));
}

const std::vector<Genre>& genres() {
    static const std::vector<Genre> g = {
        {N_("Pop"), "pop", ic::MUSIC},           {N_("Rock"), "rock", ic::ALBUM},
        {N_("Jazz"), "jazz", ic::MUSIC},         {N_("Classical"), "classical", ic::QUEUE_MUSIC},
        {N_("Electronic"), "electronic", ic::GRAPHIC_EQ}, {N_("Hip hop"), "hip hop", ic::MIC},
        {N_("Lo-fi"), "lofi", ic::LOCAL_CAFE},   {N_("News"), "news", ic::NEWSPAPER},
        {N_("Talk"), "talk", ic::PEOPLE},        {N_("Chill"), "chillout", ic::AUTO_AWESOME},
    };
    return g;
}

player::Source make_source(const Station& s) {
    player::Source src;
    src.url = s.url;
    src.title = s.name;
    src.subtitle = s.country.empty() ? s.tags : s.country;
    src.artwork = s.favicon;
    src.live = true;
    src.service = "radio";
    src.id = s.uuid;
    src.remember_position = false;
    learn(s, 1.0);
    // Count the click (helps the directory rank stations); fire and forget.
    std::string uuid = s.uuid;
    tasks::submit(tasks::API, [uuid]() -> std::function<void()> {
        http::get(util::env_or("COFFEEFLIX_RADIO_API", SERVERS[0]) + "/json/url/" + uuid, {{"User-Agent", "CoffeeFlix/2.0"}}, 8);
        return nullptr;
    });
    return src;
}

bool is_favorite(const Station& s) { return store::fav_has("radio", s.uuid); }

bool toggle_favorite(const Station& s) {
    bool on = store::fav_toggle("radio", store::Fav{s.uuid, s.name, s.country, s.favicon, serialize(s)});
    if (on) learn(s, 2.0);
    return on;
}

std::vector<Station> favorites() {
    std::vector<Station> out;
    for (auto& f : store::favs("radio")) {
        Station s;
        s.uuid = f.id;
        s.name = f.title;
        s.favicon = f.image;
        json::Doc d = json::Doc::parse(f.extra);
        s.url = json::str(d.get(), {"url"});
        s.country = json::str(d.get(), {"country"}, f.subtitle);
        s.tags = json::str(d.get(), {"tags"});
        s.codec = json::str(d.get(), {"codec"});
        s.bitrate = (int)json::num(d.get(), {"bitrate"});
        s.hls = json::boolean(d.get(), {"hls"});
        if (!s.url.empty()) out.push_back(std::move(s));
    }
    return out;
}

}  // namespace radio
