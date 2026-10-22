#include "gfx/thumb_cache.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/util.hpp"
#include "gfx/jpeg_tools.hpp"
#include "platform/platform.hpp"

namespace thumb_cache {

namespace {

std::mutex g_m;
size_t g_limit = 3000;
bool g_pruned = false;
int g_since_prune = 0;

std::string dir() { return util::join_path(platform::data_dir(), "thumbs"); }

bool stat_of(const std::string& path, long long& size, long long& mtime) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    size = (long long)st.st_size;
    mtime = (long long)st.st_mtime;
    return true;
}

char hex(unsigned v) { return "0123456789abcdef"[v & 15]; }

void prune() {
    struct Item { std::string path; long long mtime; };
    std::vector<Item> items;
    if (DIR* d = opendir(dir().c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".jpg") != 0) continue;
            long long size, mtime;
            const std::string p = util::join_path(dir(), name);
            if (stat_of(p, size, mtime)) items.push_back({p, mtime});
        }
        closedir(d);
    }
    if (items.size() <= g_limit) return;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mtime < b.mtime; });
    const size_t drop = items.size() - g_limit * 9 / 10;  // down to 90%, so it isn't again at the next picture
    for (size_t i = 0; i < drop; i++) remove(items[i].path.c_str());
}

}  // namespace

std::string file_name_for(const std::string& path, int max_w, int max_h) {
    long long size = 0, mtime = 0;
    if (!stat_of(path, size, mtime)) return "";
    const uint64_t h = util::hash64(path + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "|" + std::to_string(max_w) + "x" +
                                    std::to_string(max_h));
    std::string name;
    for (int i = 60; i >= 0; i -= 4) name += hex((unsigned)(h >> i));
    return util::join_path(dir(), name + ".jpg");
}

std::string lookup(const std::string& path, int max_w, int max_h) {
    const std::string f = file_name_for(path, max_w, max_h);
    std::string data;
    if (f.empty() || !util::read_file(f, data) || !jpeg_tools::is_jpeg(data)) return "";
    return data;
}

void store(const std::string& path, int max_w, int max_h, SDL_Surface* s) {
    if (!s || s->w * s->h < 64 * 64) return;  // a picture that small is as quick to decode as to read
    const std::string f = file_name_for(path, max_w, max_h);
    if (f.empty()) return;
    const std::string jpg = jpeg_tools::encode(s, 82);
    if (jpg.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    util::make_dirs(dir());
    util::write_file_atomic(f, jpg);
    if (!g_pruned || ++g_since_prune >= 500) {
        g_pruned = true;
        g_since_prune = 0;
        prune();
    }
}

std::string read_prefix(const std::string& path, size_t bytes) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string out(bytes, '\0');
    out.resize(fread(&out[0], 1, bytes, f));
    fclose(f);
    return out;
}

void set_limit(size_t files) { g_limit = files; }

}  // namespace thumb_cache
