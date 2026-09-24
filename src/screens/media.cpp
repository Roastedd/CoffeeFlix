// "My Media": local files on the SD card (and network shares).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>

#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/smb.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

enum Kind { K_DIR, K_VIDEO, K_AUDIO, K_IMAGE, K_BOOK, K_OTHER };

struct Entry {
    std::string name, path;
    Kind kind = K_OTHER;
    uint64_t size = 0;
};

Kind kind_of(const std::string& name) {
    std::string e = util::file_extension(name);
    static const char* video[] = {"mp4", "m4v", "mkv", "webm", "avi", "mov", "ts", "m2ts", "mpg", "mpeg", "flv", "3gp"};
    static const char* audio[] = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wv", "alac", "oga", "mka"};
    static const char* image[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
    static const char* book[] = {"pdf", "cbz", "epub"};
    for (auto v : video) if (e == v) return K_VIDEO;
    for (auto v : audio) if (e == v) return K_AUDIO;
    for (auto v : image) if (e == v) return K_IMAGE;
    for (auto v : book) if (e == v) return K_BOOK;
    return K_OTHER;
}

int kind_icon(Kind k) {
    switch (k) {
        case K_DIR: return ic::FOLDER;
        case K_VIDEO: return ic::MOVIE;
        case K_AUDIO: return ic::MUSIC;
        case K_IMAGE: return ic::PHOTO;
        case K_BOOK: return ic::BOOK;
        default: return ic::DESCRIPTION;
    }
}

// Natural sort: "Episode 2" before "Episode 10".
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) i2++;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) j2++;
            long long x = atoll(a.substr(i, i2 - i).c_str()), y = atoll(b.substr(j, j2 - j).c_str());
            if (x != y) return x < y;
            i = i2;
            j = j2;
            continue;
        }
        char ca = (char)tolower((unsigned char)a[i]), cb = (char)tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

struct Listing {
    std::vector<Entry> entries;
    bool ok = false;
    std::string error;
};

Listing list_local(const std::string& dir) {
    Listing out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        out.error = "Can't open this folder";
        return out;
    }
    while (dirent* de = readdir(d)) {
        std::string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        Entry e;
        e.name = name;
        e.path = util::join_path(dir, name);
        struct stat st;
        if (stat(e.path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            e.kind = K_DIR;
        } else {
            e.kind = kind_of(name);
            e.size = (uint64_t)st.st_size;
            if (e.kind == K_OTHER) continue;  // hide subtitles, nfo, etc.
        }
        out.entries.push_back(std::move(e));
    }
    closedir(d);
    std::sort(out.entries.begin(), out.entries.end(), [](const Entry& a, const Entry& b) {
        if ((a.kind == K_DIR) != (b.kind == K_DIR)) return a.kind == K_DIR;
        return natural_less(a.name, b.name);
    });
    out.ok = true;
    return out;
}

void ensure_default_folders() {
    std::string root = platform::media_root();
    for (const char* sub : {"Videos", "Music", "Photos", "Books"}) util::make_dirs(util::join_path(root, sub));
}

std::string strip_ext(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

class MediaScreen : public app::Screen {
public:
    explicit MediaScreen(std::string path = "", std::string title = "") : path_(std::move(path)), title_(std::move(title)) {
        if (path_.empty()) {
            ensure_default_folders();
            root_ = true;
        }
        reload();
    }

    app::Section section() const override { return app::SEC_MEDIA; }

    void on_enter() override {
        if (!root_) reload();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(path_.empty() ? "media_root" : path_.c_str());
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        text::draw(font::display, x0, y, root_ ? "My Media" : title_, t.text);
        y += 66;
        if (!root_) {
            text::draw_fit(font::small, x0, y, W - x0 - 60, path_, t.text3);
            y += 40;
        } else {
            text::draw(font::body, x0, y, "Your videos, music and photos on the SD card and network.", t.text2);
            y += 50;
            y = draw_roots(x0, y, g);
            text::draw(font::title, x0, y, "CoffeeFlix folder", t.text);
            y += 50;
        }

        if (!loading_ && !listing_.ok) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::ERROR_OUTLINE, listing_.error.c_str(), "");
            y += 280;
        } else if (!loading_ && listing_.entries.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::FOLDER, "Nothing here yet",
                        "Copy videos, music or photos into sd:/wiiu/apps/coffeeflix with an SD card reader or FTP.");
            y += 300;
        } else {
            GridSpec gs;
            gs.count = (int)listing_.entries.size();
            gs.cols = 4;
            gs.item_w = 252;
            gs.loading = loading_;
            gs.shape = CARD_WIDE;
            gs.item = [this](int i) {
                const Entry& e = listing_.entries[i];
                CardInfo c;
                c.title = e.kind == K_DIR ? e.name : strip_ext(e.name);
                c.icon = kind_icon(e.kind);
                if (e.kind == K_IMAGE) {
                    c.image = e.path;
                    c.image_w = 360;
                } else if (e.kind == K_VIDEO) {
                    c.image = "thumb://" + e.path;
                    c.image_w = 320;
                }
                if (e.kind == K_DIR) {
                    c.subtitle = "Folder";
                } else {
                    std::string ext = util::file_extension(e.name);
                    for (auto& ch : ext) ch = (char)toupper((unsigned char)ch);
                    c.subtitle = ext + " \xC2\xB7 " + util::format_bytes(e.size);
                }
                if (e.kind == K_VIDEO && store::resume_position("local", e.path) > 0) c.badge = "Resume";
                return c;
            };
            gs.on_click = [this](int i) { open((size_t)i); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", "Open"}, {"B", "Back"}});
    }

private:
    float draw_roots(float x0, float y, Id g) {
        platform::Volume vols[8];
        int n = platform::volumes(vols, 8);
        auto shares = smb::saved_shares();
        ShelfSpec ss;
        ss.count = n + 1;
        ss.shape = CARD_WIDE;
        ss.item_w = 230;
        std::vector<platform::Volume> v(vols, vols + n);
        size_t nshares = shares.size();
        ss.item = [v, nshares](int i) {
            CardInfo c;
            if (i < (int)v.size()) {
                c.title = v[i].label;
                c.icon = v[i].icon;
                c.subtitle = v[i].path;
            } else {
                c.title = "Network shares";
                c.icon = ic::LAN;
                c.subtitle = nshares == 0 ? "Add a PC or NAS" : util::fmt("%zu saved", nshares);
            }
            return c;
        };
        ss.on_click = [v](int i) {
            if (i < (int)v.size()) app::push(make_media_folder(v[i].path, v[i].label));
            else app::push(smb_browser_screen());
        };
        return y + shelf(id(g, "roots"), x0, y, ss, &page_) + 10;
    }

    void open(size_t i) {
        const Entry& e = listing_.entries[i];
        switch (e.kind) {
            case K_DIR: app::push(std::make_unique<MediaScreen>(e.path, e.name)); break;
            case K_VIDEO: {
                player::Source s;
                s.url = e.path;
                s.title = strip_ext(e.name);
                s.subtitle = util::file_name(util::parent_dir(e.path));
                s.service = "local";
                s.id = e.path;
                s.start = store::resume_position("local", e.path);
                std::string base = util::parent_dir(e.path) + "/" + strip_ext(e.name);
                for (const char* ext : {".srt", ".vtt", ".en.srt"})
                    if (util::file_exists(base + ext)) s.external_subs.push_back({util::file_name(base + ext), base + ext});
                play_video(s);
                break;
            }
            case K_AUDIO: {
                std::vector<player::Source> q;
                int idx = 0;
                for (const Entry& o : listing_.entries) {
                    if (o.kind != K_AUDIO) continue;
                    if (o.path == e.path) idx = (int)q.size();
                    player::Source s;
                    s.url = o.path;
                    s.service = "local";
                    s.id = o.path;
                    s.remember_position = false;
                    // Folder art (cover.jpg / folder.jpg) if there is no embedded cover.
                    for (const char* art : {"cover.jpg", "folder.jpg", "cover.png", "Folder.jpg", "Cover.jpg"}) {
                        std::string p = util::join_path(util::parent_dir(o.path), art);
                        if (util::file_exists(p)) { s.artwork = p; break; }
                    }
                    q.push_back(std::move(s));
                }
                play_audio_queue(std::move(q), idx);
                break;
            }
            case K_IMAGE: {
                std::vector<std::string> paths;
                int idx = 0;
                for (const Entry& o : listing_.entries) {
                    if (o.kind != K_IMAGE) continue;
                    if (o.path == e.path) idx = (int)paths.size();
                    paths.push_back(o.path);
                }
                app::push(make_photo_viewer(std::move(paths), idx));
                break;
            }
            case K_BOOK: app::push(make_reader(e.path)); break;
            default: break;
        }
    }

    void reload() {
        std::string dir = root_ ? platform::media_root() : path_;
        loading_ = listing_.entries.empty();
        scope_.reset();
        scope_.run<Listing>([dir] { return list_local(dir); },
                            [this](Listing l) {
                                listing_ = std::move(l);
                                loading_ = false;
                            });
    }

    std::string path_, title_;
    bool root_ = false;
    bool loading_ = true;
    Listing listing_;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_media() { return std::make_unique<MediaScreen>(); }
std::unique_ptr<app::Screen> make_media_folder(const std::string& path, const std::string& title) {
    return std::make_unique<MediaScreen>(path, title);
}

}  // namespace screens
