// Persistent app data (settings, accounts, favorites, resume points) kept in a
// single JSON file on the SD card and written back lazily.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <jansson.h>

namespace store {

void load(const std::string& path);
void save_now();
void tick();  // call once per frame; saves a few seconds after the last change

// --- settings ---------------------------------------------------------------
bool get_bool(const char* key, bool def);
int64_t get_int(const char* key, int64_t def);
std::string get_str(const char* key, const std::string& def = "");
void set_bool(const char* key, bool v);
void set_int(const char* key, int64_t v);
void set_str(const char* key, const std::string& v);

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

// --- recent searches ---------------------------------------------------------------
std::vector<std::string> recent_searches(const char* service);
void add_recent_search(const char* service, const std::string& q);

}  // namespace store
