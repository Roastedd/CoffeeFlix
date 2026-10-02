#include "core/store.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "core/json.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace store {

namespace {

std::recursive_mutex g_m;
json_t* g_root = nullptr;
std::string g_path;
bool g_dirty = false;
double g_dirty_since = 0;

// Writing the file takes ~50 ms on the SD card, and turning it into text tens of ms more on the
// Wii U, so tick() leaves both to a worker: done on the main thread, a video playing at 60 fps
// would skip pictures at every save. Each save is numbered: a write that got overtaken by a
// newer one is skipped.
std::mutex g_file_m;
uint64_t g_saves = 0;    // under g_m
uint64_t g_written = 0;  // under g_file_m
std::atomic<bool> g_saving{false};  // a save is on its way
std::atomic<uint32_t> g_resume_version{0};  // changes with the resume points

json_t* object_in(json_t* obj, const char* name) {
    json_t* s = json_object_get(obj, name);
    if (!json_is_object(s)) {
        s = json_object();
        json_object_set_new(obj, name, s);
    }
    return s;
}

json_t* array_in(json_t* obj, const char* name) {
    json_t* a = json_object_get(obj, name);
    if (!json_is_array(a)) {
        a = json_array();
        json_object_set_new(obj, name, a);
    }
    return a;
}

json_t* section(const char* name) { return object_in(g_root, name); }

// Each Wii U user's own, under "users" > id: their Jellyfin and YouTube sign-ins.
const char* const USER_SETTINGS[] = {"jf_server",        "jf_server_name",  "jf_user_id",       "jf_user_name",
                                     "jf_token",         "jf_device_id",    "yt_account_token", "yt_account_name",
                                     "yt_account_photo", "yt_history_sync"};
const char* const USER_FAVORITES[] = {"yt_account_channel"};
std::string g_user;  // empty: one set for everyone

template <size_t N>
bool listed(const char* const (&list)[N], const char* name) {
    for (const char* l : list)
        if (strcmp(l, name) == 0) return true;
    return false;
}

json_t* user_section(const char* name) { return object_in(object_in(section("users"), g_user.c_str()), name); }

// Where a setting or a favorite list is kept.
json_t* settings_for(const char* key) {
    return !g_user.empty() && listed(USER_SETTINGS, key) ? user_section("settings") : section("settings");
}
json_t* favorites_for(const char* service) {
    return !g_user.empty() && listed(USER_FAVORITES, service) ? user_section("favorites") : section("favorites");
}

// The other accounts of a service, kept for switching: {"settings": {...}, "favorites": {...}}
// each, the service's keys of the lists above (its prefix). Most recently used first.
const char* account_prefix(const char* service) {
    return strcmp(service, "youtube") == 0 ? "yt_" : strcmp(service, "jellyfin") == 0 ? "jf_" : nullptr;
}
json_t* saved_list(const char* service) {
    return array_in(g_user.empty() ? section("accounts") : user_section("accounts"), service);
}

// The service's account in use, as one of the others; what it had in use is left empty.
json_t* take_current(const char* prefix) {
    json_t* o = json_object();
    json_t* settings = object_in(o, "settings");
    json_t* favorites = object_in(o, "favorites");
    auto take = [&](json_t* from, json_t* to, const char* name) {
        json_t* v = json_object_get(from, name);
        if (!v) return;
        json_object_set(to, name, v);
        json_object_del(from, name);
    };
    for (const char* k : USER_SETTINGS)
        if (util::starts_with(k, prefix)) take(settings_for(k), settings, k);
    for (const char* f : USER_FAVORITES)
        if (util::starts_with(f, prefix)) take(favorites_for(f), favorites, f);
    return o;
}

// Moves `name` from everyone's `from` to the user's `to` (unless the user has one already).
bool claim(json_t* from, json_t* to, const char* name) {
    json_t* v = json_object_get(from, name);
    if (!v) return false;
    if (!json_object_get(to, name)) json_object_set(to, name, v);
    json_object_del(from, name);
    return true;
}

void mark_dirty() {
    if (!g_dirty) g_dirty_since = util::now_seconds();
    g_dirty = true;
}

std::string key_of(const std::string& service, const std::string& id) { return service + "\x1f" + id; }

// Under g_m: a copy of everything to save (null when there is nothing), so that the lock isn't held while it is turned
// into text: that takes tens of ms on the Wii U, and every setting read on the main thread waits for the lock.
json_t* snapshot(uint64_t& number) {
    if (!g_root || g_path.empty()) return nullptr;
    json_t* copy = json_deep_copy(g_root);
    g_dirty = false;
    if (copy) number = ++g_saves;
    return copy;
}

void write_json(const std::string& path, const std::string& data, uint64_t number) {
    std::lock_guard<std::mutex> lk(g_file_m);
    if (number <= g_written) return;
    util::make_dirs(util::parent_dir(path));
    if (!util::write_file_atomic(path, data)) log_message(LOG_ERROR, "Store", "Failed to write %s", path.c_str());
    g_written = number;
}

}  // namespace

void load(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    g_path = path;
    // The .tmp is the only copy after a save was cut off between removing the old file and moving
    // the new one in (util::write_file_atomic).
    for (const std::string& file : {path, path + ".tmp"}) {
        std::string data;
        if (!util::read_file(file, data)) continue;
        json_error_t err;
        json_t* root = json_loadb(data.data(), data.size(), 0, &err);
        if (!root) {
            log_message(LOG_WARNING, "Store", "Corrupt data file %s (%s)", file.c_str(), err.text);
            continue;
        }
        if (file != path) log_message(LOG_WARNING, "Store", "Recovered the data from an interrupted save");
        g_root = root;
        break;
    }
    if (!json_is_object(g_root)) {
        if (g_root) json_decref(g_root);
        g_root = json_object();
    }
    json_object_set_new(g_root, "version", json_integer(2));
}

void save_now() {
    json_t* copy = nullptr;
    std::string path;
    uint64_t number = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_m);
        copy = snapshot(number);
        path = g_path;
    }
    if (!copy) return;
    char* text = json_dumps(copy, JSON_INDENT(1));
    json_decref(copy);
    if (!text) return;
    std::string data = text;
    free(text);
    write_json(path, data, number);
}

void tick() {
    // Without waiting: the lock is busy while a save dumps everything.
    std::unique_lock<std::recursive_mutex> lk(g_m, std::try_to_lock);
    if (!lk.owns_lock() || g_saving || !g_dirty || util::now_seconds() - g_dirty_since <= 3.0) return;
    g_saving = true;
    tasks::submit(tasks::API, []() -> std::function<void()> {
        save_now();
        g_saving = false;
        return nullptr;
    });
}

void set_user(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    g_user = id;
    if (id.empty()) return;
    log_message(LOG_OK, "Store", "Sign-ins of Wii U user %s", id.c_str());
    bool moved = false;
    for (const char* k : USER_SETTINGS) moved |= claim(section("settings"), user_section("settings"), k);
    for (const char* f : USER_FAVORITES) moved |= claim(section("favorites"), user_section("favorites"), f);
    if (!moved) return;
    log_message(LOG_OK, "Store", "The sign-ins saved before are this user's now");
    mark_dirty();
}

// --- settings ---

bool get_bool(const char* key, bool def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    return json::boolean(json_object_get(settings_for(key), key), def);
}
int64_t get_int(const char* key, int64_t def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* v = json_object_get(settings_for(key), key);
    return v ? json::num(v, def) : def;
}
std::string get_str(const char* key, const std::string& def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* v = json_object_get(settings_for(key), key);
    return json_is_string(v) ? json::str(v) : def;
}
void set_bool(const char* key, bool v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(settings_for(key), key, json_boolean(v));
    mark_dirty();
}
void set_int(const char* key, int64_t v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(settings_for(key), key, json_integer(v));
    mark_dirty();
}
void set_str(const char* key, const std::string& v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(settings_for(key), key, json_string(v.c_str()));
    mark_dirty();
}

std::vector<std::string> get_str_all(const char* key) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<std::string> out;
    auto add = [&](json_t* settings) {
        json_t* v = json_object_get(settings, key);
        if (json_is_string(v) && *json_string_value(v)) out.push_back(json_string_value(v));
    };
    // The other accounts kept for switching too.
    auto add_saved = [&](json_t* accounts) {
        const char* service;
        json_t* list;
        json_object_foreach(accounts, service, list)
            for (size_t i = 0; i < json_array_size(list); i++) add(json_object_get(json_array_get(list, i), "settings"));
    };
    add(section("settings"));
    add_saved(json_object_get(g_root, "accounts"));
    const char* id;
    json_t* user;
    json_object_foreach(json_object_get(g_root, "users"), id, user) {
        add(json_object_get(user, "settings"));
        add_saved(json_object_get(user, "accounts"));
    }
    return out;
}

// --- other accounts ---

std::vector<std::map<std::string, std::string>> saved_accounts(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<std::map<std::string, std::string>> out;
    if (!account_prefix(service)) return out;
    json_t* list = saved_list(service);
    for (size_t i = 0; i < json_array_size(list); i++) {
        std::map<std::string, std::string> m;
        const char* k;
        json_t* v;
        json_object_foreach(json_object_get(json_array_get(list, i), "settings"), k, v)
            if (json_is_string(v)) m[k] = json_string_value(v);
        out.push_back(std::move(m));
    }
    return out;
}

void save_account(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    const char* prefix = account_prefix(service);
    if (!prefix) return;
    json_array_insert_new(saved_list(service), 0, take_current(prefix));
    mark_dirty();
}

void use_account(const char* service, size_t i, bool keep) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    const char* prefix = account_prefix(service);
    json_t* list = prefix ? saved_list(service) : nullptr;
    json_t* chosen = json_array_get(list, i);
    if (!chosen) return;
    json_incref(chosen);
    json_array_remove(list, i);
    json_t* current = take_current(prefix);
    if (keep) json_array_insert_new(list, 0, current);
    else json_decref(current);
    const char* k;
    json_t* v;
    json_object_foreach(json_object_get(chosen, "settings"), k, v)
        if (listed(USER_SETTINGS, k)) json_object_set(settings_for(k), k, v);
    json_object_foreach(json_object_get(chosen, "favorites"), k, v)
        if (listed(USER_FAVORITES, k)) json_object_set(favorites_for(k), k, v);
    json_decref(chosen);
    mark_dirty();
}

void forget_saved_account(const char* service, size_t i) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!account_prefix(service) || json_array_remove(saved_list(service), i) != 0) return;
    mark_dirty();
}

// --- favorites ---

std::vector<Fav> favs(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<Fav> out;
    json_t* a = array_in(favorites_for(service), service);
    for (size_t i = 0; i < json_array_size(a); i++) {
        json_t* o = json_array_get(a, i);
        out.push_back(Fav{json::str(o, {"id"}), json::str(o, {"title"}), json::str(o, {"subtitle"}),
                          json::str(o, {"image"}), json::str(o, {"extra"})});
    }
    return out;
}

bool fav_has(const char* service, const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(favorites_for(service), service);
    for (size_t i = 0; i < json_array_size(a); i++)
        if (json::str(json_array_get(a, i), {"id"}) == id) return true;
    return false;
}

void fav_set(const char* service, const Fav& f, bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(favorites_for(service), service);
    for (size_t i = 0; i < json_array_size(a); i++) {
        if (json::str(json_array_get(a, i), {"id"}) == f.id) {
            json_array_remove(a, i);
            break;
        }
    }
    if (on) {
        json_t* o = json_object();
        json_object_set_new(o, "id", json_string(f.id.c_str()));
        json_object_set_new(o, "title", json_string(f.title.c_str()));
        json_object_set_new(o, "subtitle", json_string(f.subtitle.c_str()));
        json_object_set_new(o, "image", json_string(f.image.c_str()));
        json_object_set_new(o, "extra", json_string(f.extra.c_str()));
        json_array_insert_new(a, 0, o);
    }
    mark_dirty();
}

bool fav_toggle(const char* service, const Fav& f) {
    bool now = !fav_has(service, f.id);
    fav_set(service, f, now);
    return now;
}

void fav_update(const char* service, const Fav& f) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(favorites_for(service), service);
    for (size_t i = 0; i < json_array_size(a); i++) {
        json_t* o = json_array_get(a, i);
        if (json::str(o, {"id"}) != f.id) continue;
        json_object_set_new(o, "title", json_string(f.title.c_str()));
        json_object_set_new(o, "subtitle", json_string(f.subtitle.c_str()));
        json_object_set_new(o, "image", json_string(f.image.c_str()));
        json_object_set_new(o, "extra", json_string(f.extra.c_str()));
        mark_dirty();
        return;
    }
}

void fav_trim(const char* service, size_t max) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(favorites_for(service), service);
    if (json_array_size(a) <= max) return;
    while (json_array_size(a) > max) json_array_remove(a, json_array_size(a) - 1);
    mark_dirty();
}

void fav_clear(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_array_clear(array_in(favorites_for(service), service));
    mark_dirty();
}

void fav_replace(const char* service, const std::vector<Fav>& list) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(favorites_for(service), service);
    json_array_clear(a);
    for (const Fav& f : list) {
        json_t* o = json_object();
        json_object_set_new(o, "id", json_string(f.id.c_str()));
        json_object_set_new(o, "title", json_string(f.title.c_str()));
        json_object_set_new(o, "subtitle", json_string(f.subtitle.c_str()));
        json_object_set_new(o, "image", json_string(f.image.c_str()));
        json_object_set_new(o, "extra", json_string(f.extra.c_str()));
        json_array_append_new(a, o);
    }
    mark_dirty();
}

// --- resume ---

void resume_save(const Resume& r) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* obj = section("resume");
    std::string key = key_of(r.service, r.id);
    bool finished = r.duration > 0 && (r.position > r.duration * 0.95 || r.duration - r.position < 30);
    g_resume_version++;
    if (r.position < 15 || finished) {
        json_object_del(obj, key.c_str());
        mark_dirty();
        return;
    }
    json_t* o = json_object();
    json_object_set_new(o, "service", json_string(r.service.c_str()));
    json_object_set_new(o, "id", json_string(r.id.c_str()));
    json_object_set_new(o, "title", json_string(r.title.c_str()));
    json_object_set_new(o, "subtitle", json_string(r.subtitle.c_str()));
    json_object_set_new(o, "image", json_string(r.image.c_str()));
    json_object_set_new(o, "extra", json_string(r.extra.c_str()));
    json_object_set_new(o, "position", json_real(r.position));
    json_object_set_new(o, "duration", json_real(r.duration));
    json_object_set_new(o, "updated", json_integer(util::unix_time()));
    json_object_set_new(o, "video", json_boolean(r.video));
    json_object_set_new(obj, key.c_str(), o);

    // Keep the list bounded.
    if (json_object_size(obj) > 60) {
        const char* oldest = nullptr;
        int64_t oldest_t = INT64_MAX;
        const char* k;
        json_t* v;
        json_object_foreach(obj, k, v) {
            int64_t t = json::num(v, {"updated"});
            if (t < oldest_t) { oldest_t = t; oldest = k; }
        }
        if (oldest) json_object_del(obj, oldest);
    }
    mark_dirty();
}

double resume_position(const std::string& service, const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* o = json_object_get(section("resume"), key_of(service, id).c_str());
    return json::real(o, {"position"}, 0);
}

std::vector<Resume> resume_list(size_t max) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<Resume> out;
    const char* k;
    json_t* o;
    json_object_foreach(section("resume"), k, o) {
        Resume r;
        r.service = json::str(o, {"service"});
        r.id = json::str(o, {"id"});
        r.title = json::str(o, {"title"});
        r.subtitle = json::str(o, {"subtitle"});
        r.image = json::str(o, {"image"});
        r.extra = json::str(o, {"extra"});
        r.position = json::real(o, {"position"});
        r.duration = json::real(o, {"duration"});
        r.updated = json::num(o, {"updated"});
        r.video = json::boolean(o, {"video"}, true);
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const Resume& a, const Resume& b) { return a.updated > b.updated; });
    if (out.size() > max) out.resize(max);
    return out;
}

void resume_remove(const std::string& service, const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_del(section("resume"), key_of(service, id).c_str());
    g_resume_version++;
    mark_dirty();
}

void resume_clear() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_clear(section("resume"));
    g_resume_version++;
    mark_dirty();
}

uint32_t resume_version() { return g_resume_version; }

// --- recent searches ---

std::vector<std::string> recent_searches(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<std::string> out;
    json_t* a = array_in(section("searches"), service);
    for (size_t i = 0; i < json_array_size(a); i++) out.push_back(json::str(json_array_get(a, i)));
    return out;
}

void add_recent_search(const char* service, const std::string& q) {
    std::string t = util::trim(q);
    if (t.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(section("searches"), service);
    for (size_t i = 0; i < json_array_size(a); i++) {
        if (util::lower(json::str(json_array_get(a, i))) == util::lower(t)) {
            json_array_remove(a, i);
            break;
        }
    }
    json_array_insert_new(a, 0, json_string(t.c_str()));
    while (json_array_size(a) > 12) json_array_remove(a, json_array_size(a) - 1);
    mark_dirty();
}

}  // namespace store
