// Persistent app data (settings, accounts, favorites, resume points) kept in a
// single JSON file on the SD card and written back lazily.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <jansson.h>

namespace store {

void load(const std::string& path);
void save_now();
void tick();  // call once per frame; saves in the background a few seconds after the last change

// Sign-ins belong to the Wii U user CoffeeFlix runs as (platform::user_id): their settings and
// favorite lists are kept apart for each user, the rest is everyone's. Call once, after load();
// sign-ins saved before there was one set per user go to the first user to start CoffeeFlix.
void set_user(const std::string& id);

// --- settings ---------------------------------------------------------------
bool get_bool(const char* key, bool def);
int64_t get_int(const char* key, int64_t def);
std::string get_str(const char* key, const std::string& def = "");
void set_bool(const char* key, bool v);
void set_int(const char* key, int64_t v);
void set_str(const char* key, const std::string& v);
std::vector<std::string> get_str_all(const char* key);  // every user's value (to keep them out of logs)

// --- other accounts of a service ("youtube", "jellyfin"), to switch between -----------------
// Besides the account in use, whose sign-in is in the settings (and favorite lists) above, the
// Wii U user's others: each one's text settings, most recently used first.
std::vector<std::map<std::string, std::string>> saved_accounts(const char* service);
// Up to 5 accounts a service: the account menus stop adding more (they have no room).
constexpr size_t MAX_SAVED_ACCOUNTS = 4;
// Puts the account in use among the others, first, and leaves its settings empty.
void save_account(const char* service);
// Takes other account `i` into use; the one in use goes among the others (first) with `keep`,
// else it's dropped.
void use_account(const char* service, size_t i, bool keep);
void forget_saved_account(const char* service, size_t i);

// --- favorites per service ("radio", "podcast", "twitch", "youtube_channel") --
struct Fav {
    std::string id, title, subtitle, image, extra;  // extra: service-specific (stream url, feed url...)
};
std::vector<Fav> favs(const char* service);
bool fav_has(const char* service, const std::string& id);
void fav_set(const char* service, const Fav& f, bool on);  // add or remove
bool fav_toggle(const char* service, const Fav& f);         // returns new state
void fav_update(const char* service, const Fav& f);         // replaces an entry in place, if present
void fav_trim(const char* service, size_t max);             // drops the oldest beyond `max`
void fav_clear(const char* service);
void fav_replace(const char* service, const std::vector<Fav>& list);  // the whole list, in order

// --- resume points / continue watching ----------------------------------------
struct Resume {
    std::string service;  // "local", "youtube", "jellyfin", "podcast", "smb"
    std::string id;       // service-specific key (path, video id, item id, episode url)
    std::string title, subtitle, image, extra;
    double position = 0, duration = 0;
    int64_t updated = 0;
    bool video = true;
};
void resume_save(const Resume& r);  // removes the entry when (nearly) finished
double resume_position(const std::string& service, const std::string& id);
std::vector<Resume> resume_list(size_t max = 20);
void resume_remove(const std::string& service, const std::string& id);
void resume_clear();
// Changes whenever a resume point is saved, removed or cleared: a screen showing resume_list() asks
// again only then, not every frame.
uint32_t resume_version();

// --- recent searches ---------------------------------------------------------------
std::vector<std::string> recent_searches(const char* service);
void add_recent_search(const char* service, const std::string& q);

}  // namespace store
