#include "core/store.hpp"

#include <algorithm>
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

// Writing the file takes ~50 ms on the SD card, so tick() leaves it to a worker. Each save is
// numbered: a write that got overtaken by a newer one is skipped.
std::mutex g_file_m;
uint64_t g_saves = 0;    // under g_m
uint64_t g_written = 0;  // under g_file_m

json_t* section(const char* name) {
    json_t* s = json_object_get(g_root, name);
    if (!json_is_object(s)) {
        s = json_object();
        json_object_set_new(g_root, name, s);
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

void mark_dirty() {
    if (!g_dirty) g_dirty_since = util::now_seconds();
    g_dirty = true;
}

std::string key_of(const std::string& service, const std::string& id) { return service + "\x1f" + id; }

// Under g_m. Empty when there is nothing to save.
std::string dump(uint64_t& number) {
    if (!g_root || g_path.empty()) return "";
    char* s = json_dumps(g_root, JSON_INDENT(1));
    g_dirty = false;
    if (!s) return "";
    std::string out = s;
    free(s);
    number = ++g_saves;
    return out;
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
    std::string data;
    if (util::read_file(path, data)) {
        json_error_t err;
        g_root = json_loadb(data.data(), data.size(), 0, &err);
        if (!g_root) log_message(LOG_WARNING, "Store", "Corrupt data file (%s), starting fresh", err.text);
    }
    if (!json_is_object(g_root)) {
        if (g_root) json_decref(g_root);
        g_root = json_object();
    }
    json_object_set_new(g_root, "version", json_integer(2));
}

void save_now() {
    std::string data, path;
    uint64_t number = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_m);
        data = dump(number);
        path = g_path;
    }
    if (!data.empty()) write_json(path, data, number);
}

void tick() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_dirty || util::now_seconds() - g_dirty_since <= 3.0) return;
    uint64_t number = 0;
    std::string data = dump(number);
    if (data.empty()) return;
    tasks::submit(tasks::API, [path = g_path, data = std::move(data), number]() -> std::function<void()> {
        write_json(path, data, number);
        return nullptr;
    });
}

// --- settings ---

bool get_bool(const char* key, bool def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    return json::boolean(json_object_get(section("settings"), key), def);
}
int64_t get_int(const char* key, int64_t def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* v = json_object_get(section("settings"), key);
    return v ? json::num(v, def) : def;
}
std::string get_str(const char* key, const std::string& def) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* v = json_object_get(section("settings"), key);
    return json_is_string(v) ? json::str(v) : def;
}
void set_bool(const char* key, bool v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(section("settings"), key, json_boolean(v));
    mark_dirty();
}
void set_int(const char* key, int64_t v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(section("settings"), key, json_integer(v));
    mark_dirty();
}
void set_str(const char* key, const std::string& v) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_set_new(section("settings"), key, json_string(v.c_str()));
    mark_dirty();
}

// --- favorites ---

std::vector<Fav> favs(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    std::vector<Fav> out;
    json_t* a = array_in(section("favorites"), service);
    for (size_t i = 0; i < json_array_size(a); i++) {
        json_t* o = json_array_get(a, i);
        out.push_back(Fav{json::str(o, {"id"}), json::str(o, {"title"}), json::str(o, {"subtitle"}),
                          json::str(o, {"image"}), json::str(o, {"extra"})});
    }
    return out;
}

bool fav_has(const char* service, const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(section("favorites"), service);
    for (size_t i = 0; i < json_array_size(a); i++)
        if (json::str(json_array_get(a, i), {"id"}) == id) return true;
    return false;
}

void fav_set(const char* service, const Fav& f, bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(section("favorites"), service);
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
    json_t* a = array_in(section("favorites"), service);
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
    json_t* a = array_in(section("favorites"), service);
    if (json_array_size(a) <= max) return;
    while (json_array_size(a) > max) json_array_remove(a, json_array_size(a) - 1);
    mark_dirty();
}

void fav_clear(const char* service) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_array_clear(array_in(section("favorites"), service));
    mark_dirty();
}

void fav_replace(const char* service, const std::vector<Fav>& list) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_t* a = array_in(section("favorites"), service);
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
    mark_dirty();
}

void resume_clear() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    json_object_clear(section("resume"));
    mark_dirty();
}

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
