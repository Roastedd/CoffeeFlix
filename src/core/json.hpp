// Null-safe helpers over jansson for digging through deeply nested API
// responses (InnerTube, Jellyfin, Twitch...). Every accessor tolerates missing
// keys and wrong types by returning an empty value.
#pragma once

#include <jansson.h>

#include <cstdint>
#include <string>
#include <initializer_list>
#include <cstring>
#include <cstdlib>
#include <type_traits>
#include <variant>

namespace json {

// Owning document handle.
class Doc {
public:
    Doc() = default;
    explicit Doc(json_t* root) : root_(root) {}
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&& o) noexcept : root_(o.root_) { o.root_ = nullptr; }
    Doc& operator=(Doc&& o) noexcept {
        if (this != &o) {
            if (root_) json_decref(root_);
            root_ = o.root_;
            o.root_ = nullptr;
        }
        return *this;
    }
    ~Doc() { if (root_) json_decref(root_); }

    static Doc parse(const std::string& s) {
        json_error_t err;
        return Doc(json_loadb(s.data(), s.size(), 0, &err));
    }
    json_t* get() const { return root_; }
    explicit operator bool() const { return root_ != nullptr; }

private:
    json_t* root_ = nullptr;
};

using Key = std::variant<const char*, int>;

// at(j, {"a", "b", 0, "c"}) -> j.a.b[0].c or nullptr.
inline json_t* at(json_t* j, std::initializer_list<Key> path) {
    for (const Key& k : path) {
        if (!j) return nullptr;
        if (std::holds_alternative<const char*>(k)) {
            j = json_is_object(j) ? json_object_get(j, std::get<const char*>(k)) : nullptr;
        } else {
            int i = std::get<int>(k);
            if (!json_is_array(j)) return nullptr;
            if (i < 0) i += (int)json_array_size(j);
            j = (i >= 0) ? json_array_get(j, (size_t)i) : nullptr;
        }
    }
    return j;
}

inline std::string str(json_t* j, const std::string& def = "") {
    if (json_is_string(j)) return std::string(json_string_value(j), json_string_length(j));
    if (json_is_integer(j)) return std::to_string(json_integer_value(j));
    if (json_is_real(j)) return std::to_string(json_real_value(j));
    return def;
}
inline std::string str(json_t* j, std::initializer_list<Key> path, const std::string& def = "") {
    return str(at(j, path), def);
}

inline int64_t num(json_t* j, int64_t def = 0) {
    if (json_is_integer(j)) return json_integer_value(j);
    if (json_is_real(j)) return (int64_t)json_real_value(j);
    if (json_is_string(j)) {
        const char* s = json_string_value(j);
        char* end = nullptr;
        long long v = strtoll(s, &end, 10);
        return end != s ? (int64_t)v : def;
    }
    if (json_is_boolean(j)) return json_is_true(j) ? 1 : 0;
    return def;
}
inline int64_t num(json_t* j, std::initializer_list<Key> path, int64_t def = 0) { return num(at(j, path), def); }

inline double real(json_t* j, double def = 0) {
    if (json_is_number(j)) return json_number_value(j);
    if (json_is_string(j)) return atof(json_string_value(j));
    return def;
}
inline double real(json_t* j, std::initializer_list<Key> path, double def = 0) { return real(at(j, path), def); }

// A template so a key path like boolean(j, {"isLive"}) can never bind to `def`:
// clang picks a plain bool overload for it and rejects the narrowing.
template <typename B, typename = std::enable_if_t<std::is_same_v<B, bool>>>
inline bool boolean(json_t* j, B def) {
    if (json_is_boolean(j)) return json_is_true(j);
    if (json_is_integer(j)) return json_integer_value(j) != 0;
    return def;
}
inline bool boolean(json_t* j) { return boolean(j, false); }
inline bool boolean(json_t* j, std::initializer_list<Key> path, bool def = false) { return boolean(at(j, path), def); }

inline size_t size(json_t* j) { return json_is_array(j) ? json_array_size(j) : 0; }

// YouTube "runs" style text: {"simpleText": "..."} or {"runs": [{"text": ".."}, ...]}
inline std::string yt_text(json_t* j) {
    if (!j) return "";
    if (json_t* s = json_object_get(j, "simpleText")) return str(s);
    if (json_t* c = json_object_get(j, "content")) return str(c);  // newer "view models"
    std::string out;
    json_t* runs = json_object_get(j, "runs");
    for (size_t i = 0; i < size(runs); i++) out += str(at(json_array_get(runs, i), {"text"}));
    return out;
}

// Depth-first search for the first object containing `key` (bounded depth).
inline json_t* find_key(json_t* j, const char* key, int max_depth = 12) {
    if (!j || max_depth < 0) return nullptr;
    if (json_is_object(j)) {
        if (json_t* v = json_object_get(j, key)) return v;
        const char* k;
        json_t* v;
        json_object_foreach(j, k, v) {
            if (json_t* r = find_key(v, key, max_depth - 1)) return r;
        }
    } else if (json_is_array(j)) {
        size_t i;
        json_t* v;
        json_array_foreach(j, i, v) {
            if (json_t* r = find_key(v, key, max_depth - 1)) return r;
        }
    }
    return nullptr;
}

// Visits every object that has `key` (all depths), e.g. every "videoRenderer".
template <typename F>
inline void for_each_key(json_t* j, const char* key, F&& fn, int max_depth = 24) {
    if (!j || max_depth < 0) return;
    if (json_is_object(j)) {
        const char* k;
        json_t* v;
        json_object_foreach(j, k, v) {
            if (strcmp(k, key) == 0) fn(v);
            else for_each_key(v, key, fn, max_depth - 1);
        }
    } else if (json_is_array(j)) {
        size_t i;
        json_t* v;
        json_array_foreach(j, i, v) for_each_key(v, key, fn, max_depth - 1);
    }
}

inline std::string dump(json_t* j) {
    // ENCODE_ANY: without it jansson returns NULL for anything but objects and arrays.
    char* s = json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY);
    std::string out = s ? s : "";
    free(s);
    return out;
}

}  // namespace json
