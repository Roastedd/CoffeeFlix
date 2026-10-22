#include "screens/surprise.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "core/store.hpp"
#include "core/util.hpp"
#include "screens/media_actions.hpp"

namespace screens::media {

namespace {

constexpr int SUBFOLDER_WEIGHT = 3;  // a folder counts as this many files when choosing where to go next
constexpr size_t RECENT_MAX = 20;
constexpr const char* RECENT_KEY = "surprise_recent";

struct Names {
    std::vector<std::string> files, dirs;
};

bool has_extension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot != std::string::npos && dot > 0 && name.size() - dot <= 6;
}

// One listing, names only: no stat for each entry, which is what makes a card slow. A folder is what the system
// says is one, or (where it does not say) a name without an extension.
Names list_names(const std::string& dir, SurpriseScope scope, const std::vector<std::string>& skip) {
    Names out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (std::find(skip.begin(), skip.end(), name) != skip.end()) continue;
        const Kind kind = kind_of(name);
        if (kind != K_OTHER) {
            if (surprise_matches(scope, kind)) out.files.push_back(name);
            continue;
        }
        if (de->d_type == DT_DIR || (de->d_type == DT_UNKNOWN && !has_extension(name))) out.dirs.push_back(name);
    }
    closedir(d);
    return out;
}

}  // namespace

bool surprise_matches(SurpriseScope scope, Kind kind) {
    switch (scope) {
        case SURPRISE_VIDEOS: return kind == K_VIDEO;
        case SURPRISE_MUSIC: return kind == K_AUDIO;
        case SURPRISE_PHOTOS: return kind == K_IMAGE;
        default: return kind == K_VIDEO || kind == K_AUDIO || kind == K_IMAGE;  // not the books: they are for reading
    }
}

Surprise pick_surprise(const SurpriseOptions& o) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto out_of_time = [&] {
        return std::chrono::duration<double>(Clock::now() - start).count() > o.seconds || (o.cancel && o.cancel->load());
    };
    std::mt19937_64 rng(o.seed);
    std::unordered_map<std::string, Names> listed;  // each folder is read once, however many times it is gone through
    auto names = [&](const std::string& dir, bool top) -> const Names& {
        auto it = listed.find(dir);
        if (it == listed.end()) it = listed.emplace(dir, list_names(dir, o.scope, top ? o.skip_top : std::vector<std::string>())).first;
        return it->second;
    };
    std::unordered_set<std::string> dead;  // folders that came to nothing: not gone into again, so a card full of
                                           // folders without what is asked for does not use up the tries
    auto descend = [&](const std::string& root) -> std::string {
        std::string dir = root;
        for (int depth = 0; depth <= o.max_depth; depth++) {
            const Names& n = names(dir, depth == 0);
            std::vector<std::string> dirs;
            for (const std::string& d : n.dirs)
                if (!dead.count(util::join_path(dir, d))) dirs.push_back(d);
            const size_t total = n.files.size() + (size_t)SUBFOLDER_WEIGHT * dirs.size();
            if (total == 0) {
                dead.insert(dir);
                return "";
            }
            const size_t r = (size_t)(rng() % total);
            if (r < n.files.size()) return util::join_path(dir, n.files[r]);
            dir = util::join_path(dir, dirs[(r - n.files.size()) / SUBFOLDER_WEIGHT]);
        }
        return "";
    };
    auto recent = [&](const std::string& path) { return std::find(o.avoid.begin(), o.avoid.end(), path) != o.avoid.end(); };

    Surprise result;
    std::string repeat;  // a pick that was one of the recent ones: used only if nothing else turns up
    for (const std::string& root : o.roots) {
        for (int attempt = 0; attempt < 80 && !out_of_time(); attempt++) {
            const std::string path = descend(root);
            if (path.empty()) continue;
            if (recent(path)) {
                repeat = path;
                continue;
            }
            result.ok = true;
            result.path = path;
            result.kind = kind_of(util::file_name(path));
            return result;
        }
        if (out_of_time()) break;
    }
    if (!repeat.empty()) {  // all there is has been picked lately
        result.ok = true;
        result.path = repeat;
        result.kind = kind_of(util::file_name(repeat));
    }
    return result;
}

std::vector<std::string> recent_surprises() {
    std::vector<std::string> out;
    const std::string raw = store::get_str(RECENT_KEY);
    size_t at = 0;
    while (at < raw.size()) {
        size_t nl = raw.find('\n', at);
        if (nl == std::string::npos) nl = raw.size();
        if (nl > at) out.push_back(raw.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

void remember_surprise(const std::string& path) {
    std::vector<std::string> list = recent_surprises();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > RECENT_MAX) list.resize(RECENT_MAX);
    std::string raw;
    for (const auto& p : list) raw += p + "\n";
    store::set_str(RECENT_KEY, raw);
}

void open_surprise(const std::string& path) {
    std::vector<Entry> siblings;
    Entry e;
    e.name = util::file_name(path);
    e.path = path;
    e.kind = kind_of(e.name);
    if (e.kind == K_AUDIO || e.kind == K_IMAGE) {  // the rest of the folder follows
        const std::string dir = util::parent_dir(path);
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* de = readdir(d)) {
                const std::string name = de->d_name;
                if (name.empty() || name[0] == '.' || kind_of(name) != e.kind) continue;
                Entry s;
                s.name = name;
                s.path = util::join_path(dir, name);
                s.kind = e.kind;
                siblings.push_back(std::move(s));
            }
            closedir(d);
        }
        sort_entries(siblings, SORT_NAME);
    }
    size_t index = 0;
    for (size_t i = 0; i < siblings.size(); i++)
        if (siblings[i].path == path) index = i;
    if (siblings.empty()) siblings.push_back(e);
    open_file(siblings, index, "local", util::file_exists);
}

std::vector<player::Source> music_queue_from(const std::string& path, uint64_t seed) {
    std::vector<std::string> songs;
    const std::string dir = util::parent_dir(path);
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* de = readdir(d)) {
            const std::string name = de->d_name;
            if (name.empty() || name[0] == '.' || kind_of(name) != K_AUDIO) continue;
            const std::string full = util::join_path(dir, name);
            if (full != path) songs.push_back(full);
        }
        closedir(d);
    }
    std::sort(songs.begin(), songs.end());
    std::mt19937_64 rng(seed);
    std::shuffle(songs.begin(), songs.end(), rng);
    songs.insert(songs.begin(), path);
    std::vector<player::Source> queue;
    for (const std::string& song : songs) {
        player::Source s;
        s.url = song;
        s.service = "local";
        s.id = song;
        s.remember_position = false;
        for (const char* art : {"cover.jpg", "folder.jpg", "cover.png", "Folder.jpg", "Cover.jpg"}) {
            const std::string p = util::join_path(dir, art);
            if (util::file_exists(p)) { s.artwork = p; break; }
        }
        queue.push_back(std::move(s));
    }
    return queue;
}

}  // namespace screens::media
