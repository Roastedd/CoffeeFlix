#include "app/personal.hpp"

#include <algorithm>
#include <vector>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace personal {

namespace {
constexpr const char* HIDDEN_KEY = "hidden_sections";  // ids, comma separated
constexpr const char* START_KEY = "startup_page";
constexpr const char* LAST_KEY = "last_section";

std::vector<std::string> hidden_ids() {
    std::vector<std::string> out;
    const std::string raw = store::get_str(HIDDEN_KEY);
    size_t at = 0;
    while (at <= raw.size()) {
        size_t comma = raw.find(',', at);
        if (comma == std::string::npos) comma = raw.size();
        if (comma > at) out.push_back(raw.substr(at, comma - at));
        at = comma + 1;
    }
    return out;
}

void save_hidden(const std::vector<std::string>& ids) {
    std::string raw;
    for (const auto& i : ids) raw += (raw.empty() ? "" : ",") + i;
    store::set_str(HIDDEN_KEY, raw);
}
}  // namespace

const char* const START_LAST = "last";

// The labels stay as the side menu has them: the names of services are not translated.
const SectionInfo SECTIONS[] = {
    {"home", app::SEC_HOME, N_("Home"), false, true},
    {"search", app::SEC_SEARCH, N_("Search"), true, true},
    {"youtube", app::SEC_YOUTUBE, "YouTube", true, true},
    {"jellyfin", app::SEC_JELLYFIN, "Jellyfin", true, true},
    {"navidrome", app::SEC_NAVIDROME, "Navidrome", true, true},
    {"twitch", app::SEC_TWITCH, "Twitch", true, true},
    {"radio", app::SEC_RADIO, N_("Radio"), true, true},
    {"podcasts", app::SEC_PODCASTS, N_("Podcasts"), true, true},
    {"media", app::SEC_MEDIA, N_("My Media"), true, true},
    {"settings", app::SEC_SETTINGS, N_("Settings"), false, false},
};
const size_t SECTION_COUNT = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

const SectionInfo* info(app::Section s) {
    for (const auto& i : SECTIONS)
        if (i.section == s) return &i;
    return nullptr;
}

const SectionInfo* info_for_id(std::string_view id) {
    for (const auto& i : SECTIONS)
        if (id == i.id) return &i;
    return nullptr;
}

bool hidden(app::Section s) {
    const SectionInfo* i = info(s);
    if (!i || !i->can_hide) return false;
    for (const auto& h : hidden_ids())
        if (h == i->id) return true;
    return false;
}

void set_hidden(app::Section s, bool hide) {
    const SectionInfo* i = info(s);
    if (!i || !i->can_hide) return;
    std::vector<std::string> ids = hidden_ids();
    ids.erase(std::remove(ids.begin(), ids.end(), std::string(i->id)), ids.end());
    if (hide) ids.push_back(i->id);
    save_hidden(ids);
}

int hidden_count() {
    int n = 0;
    for (const auto& i : SECTIONS)
        if (i.can_hide && hidden(i.section)) n++;
    return n;
}

void show_all() { save_hidden({}); }

app::Section section_of_service(const std::string& service) {
    if (service == "youtube") return app::SEC_YOUTUBE;
    if (service == "jellyfin") return app::SEC_JELLYFIN;
    if (service == "local" || service == "smb" || service == "dlna") return app::SEC_MEDIA;
    if (service == "podcast") return app::SEC_PODCASTS;
    return app::SEC_NONE;
}

bool reduce_motion() {
    static double read_at = -1;
    static bool value = false;
    const double now = util::now_seconds();
    if (read_at < 0 || now - read_at > 0.5) {
        value = store::get_bool("reduce_motion", false);
        read_at = now;
    }
    return value;
}

std::string start_id() {
    const std::string id = store::get_str(START_KEY, "home");
    if (id == START_LAST) return id;
    const SectionInfo* i = info_for_id(id);
    return i && i->can_start ? id : std::string("home");
}

void set_start_id(const std::string& id) { store::set_str(START_KEY, id); }

app::Section start_section() {
    std::string id = start_id();
    if (id == START_LAST) id = store::get_str(LAST_KEY, "home");
    const SectionInfo* i = info_for_id(id);
    if (!i || !i->can_start || !app::section_visible(i->section)) return app::SEC_HOME;
    return i->section;
}

void note_section(app::Section s) {
    const SectionInfo* i = info(s);
    if (!i || !i->can_start) return;
    if (store::get_str(LAST_KEY, "home") != i->id) store::set_str(LAST_KEY, i->id);
}

}  // namespace personal
