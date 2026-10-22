#include "core/stats.hpp"

#include <jansson.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace stats {

namespace {

constexpr const char* DATA_KEY = "stats_data";
constexpr const char* ON_KEY = "stats_on";
constexpr size_t MAX_DAYS = 400;
constexpr size_t MAX_TITLES = 50;
constexpr double WRITE_EVERY = 60;
constexpr double COUNT_AT = 0.8, COUNT_MAX = 300, COUNT_MIN = 30;  // see playing()

struct Doc {
    std::string profile;
    json_t* root = nullptr;
    bool dirty = false;
    double written_at = 0;
    std::map<std::string, double> played;  // this run: seconds of each title
    std::set<std::string> counted;         // this run: titles already counted as watched
    bool opened = false;
};
Doc& doc() {
    static Doc* d = new Doc;  // never destroyed
    return *d;
}

json_t* fresh() {
    json_t* r = json_object();
    json_object_set_new(r, "v", json_integer(1));
    return r;
}

json_t* parse(const std::string& text) {
    if (text.empty()) return fresh();
    json_error_t err;
    json_t* r = json_loads(text.c_str(), 0, &err);
    if (!r || !json_is_object(r)) {
        if (r) json_decref(r);
        return fresh();
    }
    return r;
}

json_t* object_in(json_t* o, const char* key) {
    json_t* v = json_object_get(o, key);
    if (!json_is_object(v)) {
        v = json_object();
        json_object_set_new(o, key, v);
    }
    return v;
}

// A day as "20261015", and as a number of days (for counting back).
std::string day_key(const std::tm& t) { return util::fmt("%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); }
int day_number(int y, int m, int d) {
    std::tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    return (int)(mktime(&t) / 86400);
}
int day_number_of_key(const std::string& k) {
    int y = 0, m = 0, d = 0;
    if (sscanf(k.c_str(), "%4d%2d%2d", &y, &m, &d) != 3) return 0;
    return day_number(y, m, d);
}

void save(Doc& d) {
    if (!d.root) return;
    char* text = json_dumps(d.root, JSON_COMPACT);
    if (text) {
        store::set_str(DATA_KEY, text);
        free(text);
    }
    d.dirty = false;
    d.written_at = util::now_seconds();
}

// The profile in use's document, read when the profile changed.
Doc& current() {
    Doc& d = doc();
    const std::string profile = store::active_profile();
    if (!d.root || d.profile != profile) {
        if (d.root) json_decref(d.root);
        d.root = parse(store::get_str(DATA_KEY));
        d.profile = profile;
        d.dirty = false;
        d.played.clear();
        d.counted.clear();
        d.opened = false;
    }
    return d;
}

void trim(json_t* root) {
    json_t* days = object_in(root, "days");
    if (json_object_size(days) > MAX_DAYS) {
        std::vector<std::string> keys;
        const char* k;
        json_t* v;
        json_object_foreach(days, k, v) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (size_t i = 0; i + MAX_DAYS < keys.size(); i++) json_object_del(days, keys[i].c_str());
    }
    json_t* titles = object_in(root, "titles");
    while (json_object_size(titles) > MAX_TITLES) {  // the one with the least time goes
        const char* worst = nullptr;
        double least = 1e18;
        const char* k;
        json_t* v;
        json_object_foreach(titles, k, v) {
            const double x = json_number_value(json_object_get(v, "x"));
            if (x < least) {
                least = x;
                worst = k;
            }
        }
        if (!worst) break;
        json_object_del(titles, worst);
    }
}

void note_since(json_t* root, const std::tm& now) {
    if (!json_is_string(json_object_get(root, "since")))
        json_object_set_new(root, "since", json_string(util::fmt("%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday).c_str()));
}

}  // namespace

bool enabled() { return store::get_bool(ON_KEY, true); }
void set_enabled(bool on) { store::set_bool(ON_KEY, on); }

void playing(const Played& p, double seconds) {
    if (!enabled() || seconds <= 0 || p.service.empty()) return;
    seconds = std::min(seconds, 30.0);
    Doc& d = current();
    std::tm now;
    util::local_tm(&now);
    note_since(d.root, now);
    json_t* day = object_in(object_in(d.root, "days"), day_key(now).c_str());
    json_t* slot = json_object_get(day, p.service.c_str());
    if (!json_is_array(slot)) {
        slot = json_pack("[f,i]", 0.0, 0);
        json_object_set_new(day, p.service.c_str(), slot);
    }
    json_array_set_new(slot, 0, json_real(json_number_value(json_array_get(slot, 0)) + seconds));

    const std::string key = p.service + "\x1f" + (p.id.empty() ? p.title : p.id);
    json_t* titles = object_in(d.root, "titles");
    json_t* t = json_object_get(titles, key.c_str());
    if (!json_is_object(t)) {
        t = json_object();
        json_object_set_new(titles, key.c_str(), t);
    }
    json_object_set_new(t, "t", json_string(p.title.c_str()));
    json_object_set_new(t, "s", json_string(p.service.c_str()));
    json_object_set_new(t, "x", json_real(json_number_value(json_object_get(t, "x")) + seconds));

    // Watched: 80% of it, or five minutes, whichever is less (and half a minute at least), once for a run.
    d.played[key] += seconds;
    const double need = p.duration > 0 ? std::clamp(p.duration * COUNT_AT, COUNT_MIN, COUNT_MAX) : COUNT_MAX;
    if (!d.counted.count(key) && d.played[key] >= need) {
        d.counted.insert(key);
        json_array_set_new(slot, 1, json_integer(json_integer_value(json_array_get(slot, 1)) + 1));
        json_object_set_new(t, "p", json_integer(json_integer_value(json_object_get(t, "p")) + 1));
    }
    trim(d.root);
    d.dirty = true;
}

void app_opened() {
    if (!enabled()) return;
    Doc& d = current();
    if (d.opened) return;
    d.opened = true;
    std::tm now;
    util::local_tm(&now);
    note_since(d.root, now);
    json_object_set_new(d.root, "opened", json_integer(json_integer_value(json_object_get(d.root, "opened")) + 1));
    d.dirty = true;
}

void tick() {
    Doc& d = doc();
    if (d.dirty && util::now_seconds() - d.written_at >= WRITE_EVERY) save(d);
}

void flush() {
    Doc& d = doc();
    if (d.dirty) save(d);
}

void reset() {
    Doc& d = current();
    json_decref(d.root);
    d.root = fresh();
    d.played.clear();
    d.counted.clear();
    d.dirty = false;
    store::set_str(DATA_KEY, "");
}

Report report(const std::string& profile) {
    Report r;
    json_t* root;
    json_t* owned = nullptr;
    if (profile.empty() || profile == store::active_profile()) {
        root = current().root;
    } else {
        root = owned = parse(store::get_str_of(profile, DATA_KEY));
    }
    std::tm now;
    util::local_tm(&now);
    const int today = day_number(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    std::map<std::string, double> services;
    json_t* days = json_object_get(root, "days");
    const char* k;
    json_t* v;
    json_object_foreach(days, k, v) {
        const int age = today - day_number_of_key(k);
        const char* service;
        json_t* slot;
        json_object_foreach(v, service, slot) {
            const double s = json_number_value(json_array_get(slot, 0));
            const int w = (int)json_integer_value(json_array_get(slot, 1));
            r.all.seconds += s;
            r.all.watched += w;
            if (age < 30) {
                r.month.seconds += s;
                r.month.watched += w;
                services[service] += s;
            }
            if (age < 7) {
                r.week.seconds += s;
                r.week.watched += w;
            }
        }
    }
    r.opened = (int)json_integer_value(json_object_get(root, "opened"));
    r.since = json_is_string(json_object_get(root, "since")) ? json_string_value(json_object_get(root, "since")) : "";
    r.any = r.all.seconds > 0 || r.opened > 0;
    for (const auto& [service, seconds] : services)
        if (seconds > 0) r.services.push_back({service, seconds});
    std::sort(r.services.begin(), r.services.end(), [](const Share& a, const Share& b) { return a.seconds > b.seconds; });
    json_t* titles = json_object_get(root, "titles");
    json_object_foreach(titles, k, v) {
        Title t;
        t.title = json_is_string(json_object_get(v, "t")) ? json_string_value(json_object_get(v, "t")) : "";
        t.service = json_is_string(json_object_get(v, "s")) ? json_string_value(json_object_get(v, "s")) : "";
        t.plays = (int)json_integer_value(json_object_get(v, "p"));
        t.seconds = json_number_value(json_object_get(v, "x"));
        if (!t.title.empty() && t.seconds >= 60) r.top.push_back(std::move(t));  // a minute of it, at least
    }
    std::sort(r.top.begin(), r.top.end(), [](const Title& a, const Title& b) { return a.seconds > b.seconds; });
    if (r.top.size() > 5) r.top.resize(5);
    if (owned) json_decref(owned);
    return r;
}

const char* label(const std::string& s) {
    if (s == "youtube") return "YouTube";
    if (s == "jellyfin") return "Jellyfin";
    if (s == "navidrome") return "Navidrome";
    if (s == "twitch") return "Twitch";
    if (s == "radio") return tr("Radio");
    if (s == "podcast") return tr("Podcasts");
    if (s == "local" || s == "smb" || s == "dlna") return tr("My Media");
    return tr("Other");
}

std::string rounded(double seconds) {
    if (seconds < 60) return tr("Under a minute");
    const double hours = seconds / 3600;
    if (hours < 1) return util::fmt(tr("About %d min"), std::max(1, (int)(seconds / 60.0 / 5.0 + 0.5) * 5));
    // Half hours up to ten hours, then whole hours.
    if (hours < 10) {
        const double half = std::round(hours * 2) / 2;
        return util::fmt(tr("About %s h"), half == std::floor(half) ? util::fmt("%d", (int)half).c_str() : util::fmt("%.1f", half).c_str());
    }
    return util::fmt(tr("About %s h"), util::fmt("%d", (int)std::round(hours)).c_str());
}

}  // namespace stats
