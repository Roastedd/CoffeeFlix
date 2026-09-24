// "My Media": local files on the SD card (and network shares).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>

#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "screens/media_actions.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/smb.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using namespace media;  // Entry, kinds, sorting and opening (shared with the SMB browser)

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
    sort_entries(out.entries);
    out.ok = true;
    return out;
}

void ensure_default_folders() {
    std::string root = platform::media_root();
    for (const char* sub : {"Videos", "Music", "Photos", "Books"}) util::make_dirs(util::join_path(root, sub));
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
            gs.item = [this](int i) { return entry_card(listing_.entries[i], "local"); };
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
        if (e.kind == K_DIR) app::push(std::make_unique<MediaScreen>(e.path, e.name));
        else open_file(listing_.entries, i, "local", util::file_exists);
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
