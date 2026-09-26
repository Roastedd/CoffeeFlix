#include "core/i18n.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace i18n {

const Language LANGUAGES[] = {
    {"en", "English", N_("English")},
    {"tl", "Tagalog", N_("Tagalog")},
    {"es", "Espa\xC3\xB1ol", N_("Spanish")},
    {"fr", "Fran\xC3\xA7" "ais", N_("French")},
    {"de", "Deutsch", N_("German")},
    {"it", "Italiano", N_("Italian")},
    {"pt", "Portugu\xC3\xAAs", N_("Portuguese")},
    {"nl", "Nederlands", N_("Dutch")},
    {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", N_("Japanese")},
    {"zh", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", N_("Chinese")},
    {"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", N_("Korean")},
};
const int LANGUAGE_COUNT = sizeof(LANGUAGES) / sizeof(LANGUAGES[0]);

namespace {

struct Catalog {
    std::deque<std::string> text;  // keys and values; a deque never moves them
    std::unordered_map<std::string_view, const char*> map;
};

std::string g_content, g_chosen;
const Language* g_current = &LANGUAGES[0];
const Language* g_system = &LANGUAGES[0];
std::atomic<const Catalog*> g_catalog{nullptr};
std::atomic<int> g_generation{0};
// Loaded catalogs stay until the app closes: other threads may still hold their text.
std::mutex g_mutex;
std::map<std::string, std::unique_ptr<Catalog>> g_loaded;

const Language* find(const std::string& code) {
    for (int i = 0; i < LANGUAGE_COUNT; i++)
        if (code == LANGUAGES[i].code) return &LANGUAGES[i];
    return nullptr;
}

// The printf conversions in s ("%s of %d" -> "sd"), which a translation has to keep, in order.
std::string conversions(const char* s) {
    std::string out;
    for (const char* p = s; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*", *p)) p++;
        while (*p && strchr("hljztL", *p)) out += *p++;
        if (!*p) break;
        out += *p;
    }
    return out;
}

const Catalog* load(const Language& lang) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_loaded.find(lang.code);
    if (it != g_loaded.end()) return it->second.get();
    auto cat = std::make_unique<Catalog>();
    std::string path = g_content + "/lang/" + lang.code + ".json", body;
    json::Doc doc = util::read_file(path, body) ? json::Doc::parse(body) : json::Doc();
    if (!json_is_object(doc.get())) {
        log_message(LOG_WARNING, "Language", "No translations in %s", path.c_str());
    } else {
        const char* key;
        json_t* value;
        int skipped = 0;
        json_object_foreach(doc.get(), key, value) {
            const char* v = json_string_value(value);
            if (!v || !*v) continue;
            // A translation that formats differently could read the wrong arguments: English instead.
            if (conversions(key) != conversions(v)) {
                skipped++;
                continue;
            }
            const std::string& k = cat->text.emplace_back(key);
            cat->map[k] = cat->text.emplace_back(v).c_str();
        }
        log_message(skipped ? LOG_WARNING : LOG_OK, "Language", "%s: %zu translations%s", lang.english, cat->map.size(),
                    skipped ? util::fmt(", %d left out (their %% codes don't match)", skipped).c_str() : "");
    }
    return (g_loaded[lang.code] = std::move(cat)).get();
}

void apply() {
    const Language* lang = find(g_chosen);
    g_current = lang ? lang : g_system;
    g_catalog = g_current == &LANGUAGES[0] ? nullptr : load(*g_current);
    g_generation++;
}

}  // namespace

void init(const std::string& content_dir) {
    g_content = content_dir;
    std::string sys = platform::system_language();
    // The console's Traditional Chinese gets the Simplified text rather than none.
    if (sys.rfind("zh", 0) == 0) sys = "zh";
    const Language* s = find(sys);
    g_system = s ? s : &LANGUAGES[0];
    g_chosen = store::get_str("language");
    apply();
    log_message(LOG_OK, "Language", "%s (the system's is %s)", g_current->english, sys.c_str());
}

const std::string& chosen() { return g_chosen; }

void choose(const std::string& code) {
    g_chosen = find(code) ? code : "";
    store::set_str("language", g_chosen);
    apply();
}

const Language& current() { return *g_current; }
const Language& system() { return *g_system; }
int generation() { return g_generation; }

const char* tr(const char* english) {
    const Catalog* cat = g_catalog;
    if (!cat || !english) return english;
    auto it = cat->map.find(english);
    return it == cat->map.end() ? english : it->second;
}

}  // namespace i18n
