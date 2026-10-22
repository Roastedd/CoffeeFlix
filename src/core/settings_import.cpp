#include "core/settings_import.hpp"

#include <cstdio>

#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace settings_import {

namespace {

// A setting of the other install: a user's (the Wii U keeps sign-ins per user) before the shared one.
std::string find(json_t* root, const char* key) {
    json_t* users = json_object_get(root, "users");
    const char* id;
    json_t* user;
    json_object_foreach(users, id, user) {
        json_t* v = json_object_get(json_object_get(user, "settings"), key);
        if (json_is_string(v) && *json_string_value(v)) return json_string_value(v);
    }
    json_t* v = json_object_get(json_object_get(root, "settings"), key);
    return json_is_string(v) && *json_string_value(v) ? json_string_value(v) : "";
}

// Takes `keys` from the file when this install has none of them yet (what is here is never replaced).
int take_group(json_t* root, std::initializer_list<const char*> keys, const char* lead) {
    if (!store::get_str(lead).empty()) return 0;
    if (find(root, lead).empty()) return 0;
    for (const char* k : keys) {
        const std::string v = find(root, k);
        if (!v.empty()) store::set_str(k, v);
    }
    return 1;
}


}  // namespace

Result import_text(const std::string& text) {
    Result r;
    r.found = true;
    auto doc = json::Doc::parse(text);
    if (!json_is_object(doc.get())) {
        r.summary = "That file isn't a CoffeeFlix settings file";
        return r;
    }
    json_t* root = doc.get();
    const int sources = 0;
    const int jellyfin = take_group(root, {"jf_server", "jf_server_name", "jf_user_id", "jf_user_name", "jf_token", "jf_device_id"}, "jf_server");
    const int youtube = take_group(root, {"yt_account_token", "yt_account_name", "yt_account_photo"}, "yt_account_token");
    int history = 0;
    r.imported = sources + jellyfin + youtube + history;
    std::string parts;
    auto add = [&](const std::string& s) { parts += (parts.empty() ? "" : ", ") + s; };
    if (jellyfin) add("the Jellyfin sign-in");
    if (youtube) add("the YouTube sign-in");
    if (history) add("what you watched");
    r.summary = r.imported ? "Imported " + parts + " from your Wii U" : "Nothing new in that file: everything in it is already here";
    if (r.imported) store::save_now();
    return r;
}

Result run() {
    const std::string media = platform::media_root();
    for (const std::string& path : {util::join_path(media, "coffeeflix.json"), util::join_path(util::join_path(media, "Received"), "coffeeflix.json")}) {
        std::string text;
        if (!util::file_exists(path) || !util::read_file(path, text)) continue;
        Result r = import_text(text);
        if (r.imported || r.summary.rfind("Nothing new", 0) == 0) std::remove(path.c_str());  // it holds tokens: no copy left lying around
        log_message(LOG_OK, "Import", "%s (%s)", r.summary.c_str(), path.c_str());
        return r;
    }
    return {};
}

}  // namespace settings_import
