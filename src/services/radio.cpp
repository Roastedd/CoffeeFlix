#include "services/radio.hpp"

#include <mutex>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"

namespace radio {

namespace {

// Mirrors of the API; the first reachable one is remembered.
const char* SERVERS[] = {"https://de1.api.radio-browser.info", "https://de2.api.radio-browser.info",
                         "https://fi1.api.radio-browser.info", "https://nl1.api.radio-browser.info",
                         "https://at1.api.radio-browser.info"};
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

}  // namespace

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
        {"Pop", "pop", ic::MUSIC},          {"Rock", "rock", ic::ALBUM},     {"Jazz", "jazz", ic::MUSIC},
        {"Classical", "classical", ic::QUEUE_MUSIC}, {"Electronic", "electronic", ic::GRAPHIC_EQ},
        {"Hip hop", "hip hop", ic::MIC},     {"Lo-fi", "lofi", ic::LOCAL_CAFE}, {"News", "news", ic::NEWSPAPER},
        {"Talk", "talk", ic::PEOPLE},        {"Chill", "chillout", ic::AUTO_AWESOME},
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
    return store::fav_toggle("radio", store::Fav{s.uuid, s.name, s.country, s.favicon, serialize(s)});
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
