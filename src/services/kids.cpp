#include "services/kids.hpp"

#include <jansson.h>

#include "core/json.hpp"
#include "core/store.hpp"

namespace kids {

namespace {

constexpr const char* KEY = "kids_channels";

json::Doc read() {
    json::Doc d = json::Doc::parse(store::get_str(KEY, "[]"));
    return json_is_array(d.get()) ? std::move(d) : json::Doc(json_array());
}

void write(json_t* list) { store::set_str(KEY, json::dump(list)); }

json::Doc read_key(const char* key) {
    json::Doc d = json::Doc::parse(store::get_str(key, "[]"));
    return json_is_array(d.get()) ? std::move(d) : json::Doc(json_array());
}

// An entry with a text key, in a list kept as JSON under `store_key`: refreshes it if there, adds it if there's room.
bool put(const char* store_key, const char* key_field, const std::string& key,
         const std::vector<std::pair<const char*, std::string>>& fields, size_t max) {
    if (key.empty()) return false;
    json::Doc d = read_key(store_key);
    json_t* list = d.get();
    json_t* o = nullptr;
    for (size_t i = 0; i < json_array_size(list) && !o; i++)
        if (json::str(json_array_get(list, i), {key_field}) == key) o = json_array_get(list, i);
    if (!o) {
        if (json_array_size(list) >= max) return false;
        o = json_object();
        json_array_append_new(list, o);
    }
    json_object_set_new(o, key_field, json_string(key.c_str()));
    for (const auto& f : fields)
        if (!f.second.empty() || !json_object_get(o, f.first)) json_object_set_new(o, f.first, json_string(f.second.c_str()));
    store::set_str(store_key, json::dump(list));
    return true;
}

void drop(const char* store_key, const char* key_field, const std::string& key) {
    json::Doc d = read_key(store_key);
    json_t* list = d.get();
    for (size_t i = 0; i < json_array_size(list); i++) {
        if (json::str(json_array_get(list, i), {key_field}) == key) {
            json_array_remove(list, i);
            store::set_str(store_key, json::dump(list));
            return;
        }
    }
}

}  // namespace

std::vector<Channel> channels() {
    std::vector<Channel> out;
    json::Doc d = read();
    for (size_t i = 0; i < json_array_size(d.get()); i++) {
        json_t* o = json_array_get(d.get(), i);
        Channel c{json::str(o, {"id"}), json::str(o, {"name"}), json::str(o, {"avatar"})};
        if (!c.id.empty()) out.push_back(std::move(c));
    }
    return out;
}

bool allowed(const std::string& channel_id) {
    if (channel_id.empty()) return false;
    for (const Channel& c : channels())
        if (c.id == channel_id) return true;
    return false;
}

bool add(const Channel& c) {
    if (c.id.empty()) return false;
    json::Doc d = read();
    json_t* list = d.get();
    for (size_t i = 0; i < json_array_size(list); i++) {
        json_t* o = json_array_get(list, i);
        if (json::str(o, {"id"}) != c.id) continue;
        if (!c.name.empty()) json_object_set_new(o, "name", json_string(c.name.c_str()));
        if (!c.avatar.empty()) json_object_set_new(o, "avatar", json_string(c.avatar.c_str()));
        write(list);
        return true;
    }
    if (json_array_size(list) >= MAX_CHANNELS) return false;
    json_t* o = json_object();
    json_object_set_new(o, "id", json_string(c.id.c_str()));
    json_object_set_new(o, "name", json_string(c.name.c_str()));
    json_object_set_new(o, "avatar", json_string(c.avatar.c_str()));
    json_array_append_new(list, o);
    write(list);
    return true;
}

void remove(const std::string& channel_id) {
    json::Doc d = read();
    json_t* list = d.get();
    for (size_t i = 0; i < json_array_size(list); i++) {
        if (json::str(json_array_get(list, i), {"id"}) == channel_id) {
            json_array_remove(list, i);
            write(list);
            return;
        }
    }
}

const std::vector<Channel>& suggested() {
    static const std::vector<Channel> list = {
        {"UCrNnk0wFBnCS1awGjq_ijGQ", "PBS KIDS", ""},
        {"UCoookXUzPciGrEZEXmh4Jjg", "Sesame Street", ""},
        {"UCVzLLZkDuFGAE2BGdBuBNBg", "Bluey - Official Channel", ""},
        {"UCAOtE1V7Ots4DjM8JLlrYgg", "Peppa Pig - Official Channel", ""},
        {"UCNcdbMyA59zE-Vk668bKWOg", "Disney Jr.", ""},
        {"UCXVCgDuD_QCkI7gTKU7-tpg", "Nat Geo Kids", ""},
        {"UC2ri4rEb8abnNwXvTjg5ARw", "Khan Academy Kids", ""},
        {"UC0fXNmjDoC7ckPyT6qM8Urw", "CBeebies", ""},
        {"UCPlwvN0w4qFSP1FllALB92w", "Numberblocks", ""},
        {"UC_qs3c0ehDvZkbiEbOj6Drg", "Alphablocks", ""},
        {"UCLsooMJoIpl_7ux2jvdPB-Q", "Super Simple Songs", ""},
        {"UC0qrcKuayvbMtPj-WXNFylA", "Thomas & Friends", ""},
        {"UCRFIPG2u1DxKLNuE3y2SjHA", "SciShow Kids", ""},
        {"UCONtPx56PSebXJOxbFv-2jQ", "Crash Course Kids", ""},
    };
    return list;
}

std::vector<Podcast> podcasts() {
    std::vector<Podcast> out;
    json::Doc d = read_key("kids_podcasts");
    for (size_t i = 0; i < json_array_size(d.get()); i++) {
        json_t* o = json_array_get(d.get(), i);
        Podcast p{json::str(o, {"feed_url"}), json::str(o, {"name"}), json::str(o, {"artwork"})};
        if (!p.feed_url.empty()) out.push_back(std::move(p));
    }
    return out;
}
bool podcast_allowed(const std::string& feed_url) {
    if (feed_url.empty()) return false;
    for (const Podcast& p : podcasts())
        if (p.feed_url == feed_url) return true;
    return false;
}
bool add_podcast(const Podcast& p) { return put("kids_podcasts", "feed_url", p.feed_url, {{"name", p.name}, {"artwork", p.artwork}}, MAX_AUDIO); }
void remove_podcast(const std::string& feed_url) { drop("kids_podcasts", "feed_url", feed_url); }

std::vector<Station> stations() {
    std::vector<Station> out;
    json::Doc d = read_key("kids_radio");
    for (size_t i = 0; i < json_array_size(d.get()); i++) {
        json_t* o = json_array_get(d.get(), i);
        Station s{json::str(o, {"url"}), json::str(o, {"name"})};
        if (!s.url.empty()) out.push_back(std::move(s));
    }
    return out;
}
bool station_allowed(const std::string& url) {
    if (url.empty()) return false;
    for (const Station& s : stations())
        if (s.url == url) return true;
    return false;
}
bool add_station(const Station& s) { return put("kids_radio", "url", s.url, {{"name", s.name}}, MAX_AUDIO); }
void remove_station(const std::string& url) { drop("kids_radio", "url", url); }

const std::vector<Podcast>& suggested_podcasts() {
    static const std::vector<Podcast> list = {
        {"https://rss.art19.com/wow-in-the-world", "Wow in the World", ""},
        {"https://feeds.megaphone.fm/LEME2692268862", "Story Pirates", ""},
        {"https://feeds.megaphone.fm/LEME2532531912", "Brains On!", ""},
        {"https://feeds.megaphone.fm/LEME2157145514", "Smash Boom Best", ""},
        {"https://rss.wbur.org/circleround/podcast", "Circle Round", ""},
        {"https://podcasts.vpr.net/but-why", "But Why", ""},
        {"https://feeds.megaphone.fm/ARML4771062206", "Tumble Science Podcast for Kids", ""},
        {"https://feeds.simplecast.com/fS_LWlfx", "Terrestrials", ""},
        {"https://feeds.megaphone.fm/STM4306131571", "Stories Podcast", ""},
        {"https://feeds.megaphone.fm/ESP4751925729", "Greeking Out", ""},
        {"https://www.abc.net.au/feeds/7388142/podcast.xml", "Short & Curly", ""},
        {"https://feed.podbean.com/earsnacks/feed.xml", "Ear Snacks", ""},
    };
    return list;
}

const std::vector<Station>& suggested_stations() {
    static const std::vector<Station> list = {
        {"https://listen-funkids.sharp-stream.com/funkids.mp3", "Fun Kids"},
        {"https://classicalkids.stream.publicradio.org/classicalkids.aac", "YourClassical Children"},
        {"https://allclassical-ord.streamguys1.com/ICAN", "ICAN Radio"},
        {"http://kinderlieder-123.stream.laut.fm/kinderlieder-123", "Kinderlieder 1-2-3"},
        {"http://icestreaming.rai.it/11.mp3", "Rai Radio Kids"},
    };
    return list;
}

int add_suggested_audio() {
    int added = 0;
    for (const Podcast& p : suggested_podcasts()) {
        const bool had = podcast_allowed(p.feed_url);
        if (add_podcast(p) && !had) added++;
    }
    for (const Station& s : suggested_stations()) {
        const bool had = station_allowed(s.url);
        if (add_station(s) && !had) added++;
    }
    return added;
}

int seed_defaults_once() {
    if (store::get_bool("kids_seeded", false)) return 0;
    store::set_bool("kids_seeded", true);
    return add_suggested() + add_suggested_audio();
}

int add_suggested() {
    int added = 0;
    for (const Channel& c : suggested()) {
        const bool had = allowed(c.id);
        if (add(c) && !had) added++;
    }
    return added;
}

}  // namespace kids
