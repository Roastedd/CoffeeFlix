// "My Media": local files on the SD card (and network shares).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "platform/platform.hpp"
#include "player/probe.hpp"
#include "player/thumbnailer.hpp"
#include "screens/media_actions.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/smb.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using namespace media;  // Entry, kinds, sorting and opening (shared with the SMB browser)

constexpr int IC_SORT = 0xe164, IC_NEW_FOLDER = 0xe2cc;  // Material Icons: sort, create_new_folder

struct Listing {
    std::vector<Entry> entries;
    bool ok = false;
    std::string error;
};

Listing list_local(const std::string& dir, SortMode sort, const std::string& ui_language) {
    Listing out;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        out.error = tr("Can't open this folder");
        return out;
    }
    std::vector<std::string> subtitles;  // for the videos, after
    while (dirent* de = readdir(d)) {
        std::string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        Entry e;
        e.name = name;
        e.path = util::join_path(dir, name);
        struct stat st;
        if (link_stat(e.path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) {
            e.link = true;  // shown for what it points to, never changed
            if (stat(e.path.c_str(), &st) != 0) continue;
        }
        e.mtime = (int64_t)st.st_mtime;
        if (S_ISDIR(st.st_mode)) {
            e.kind = K_DIR;
        } else {
            e.kind = kind_of(name);
            e.size = (uint64_t)st.st_size;
            if (e.kind == K_OTHER) {  // hide subtitles, nfo, etc.
                if (is_subtitle_file(name)) subtitles.push_back(name);
                continue;
            }
        }
        out.entries.push_back(std::move(e));
    }
    closedir(d);
    for (Entry& e : out.entries) {
        if (e.kind != K_VIDEO) continue;
        e.subs = find_sidecars(dir, e.name, subtitles, ui_language);
        e.subs_listed = true;
    }
    sort_entries(out.entries, sort);
    out.ok = true;
    return out;
}

void ensure_default_folders() {
    std::string root = platform::media_root();
    for (const char* sub : {"Videos", "Music", "Photos", "Books"}) util::make_dirs(util::join_path(root, sub));
}

std::string received_folder() { return util::join_path(platform::media_root(), "Received"); }

// Files can be changed below the top of CoffeeFlix's folder, except in the app's own caches there
// (on the Wii U it is also the app's data folder).
bool editable_folder(const std::string& dir) {
    std::string root = platform::media_root();
    if (!inside(dir, root)) return false;
    std::string top = util::lower(util::split(dir.substr(root.size() + (root.back() == '/' ? 0 : 1)), '/').front());
    return top != "thumbs";
}

bool same_dir(std::string a, std::string b) {
    while (a.size() > 1 && a.back() == '/') a.pop_back();
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    return a == b;
}

const char* sort_label(SortMode m) {
    switch (m) {
        case SORT_NEWEST: return tr("Newest first");
        case SORT_SIZE: return tr("Size");
        default: return tr("Name");
    }
}

std::string upper_ext(const std::string& name) {
    std::string ext = util::file_extension(name);
    for (char& c : ext) c = (char)toupper((unsigned char)c);
    return ext;
}

// Resume points are kept by path: they follow a renamed file, or the files in a renamed folder.
void move_resume_points(const std::string& from, const std::string& to, bool folder) {
    for (store::Resume r : store::resume_list(200)) {
        if (r.service != "local") continue;
        std::string id;
        if (r.id == from) id = to;
        else if (folder && util::starts_with(r.id, from + "/")) id = to + r.id.substr(from.size());
        else continue;
        store::resume_remove("local", r.id);
        if (!folder) r.title = strip_ext(util::file_name(to));
        r.id = id;
        store::resume_save(r);
    }
}

// --- what's in a file ------------------------------------------------------------------------

class InfoScreen : public app::Screen {
public:
    // The folder's files, for Play to go on the way it does from the folder.
    InfoScreen(std::vector<Entry> siblings, size_t index, const player::MediaInfo* known)
        : siblings_(std::move(siblings)), index_(index), e_(siblings_[index]) {
        if (known) {
            info_ = *known;
            have_ = true;
        } else if (e_.kind == K_VIDEO || e_.kind == K_AUDIO) {
            std::string path = e_.path;
            scope_.run<player::MediaInfo>([path] { return player::probe_media(path); },
                                          [this](player::MediaInfo m) {
                                              info_ = std::move(m);
                                              have_ = true;
                                          });
        }
    }

    app::Section section() const override { return app::SEC_MEDIA; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        bool media = e_.kind == K_VIDEO || e_.kind == K_AUDIO;
        std::string image = e_.kind == K_VIDEO ? "thumb://" + e_.path : e_.kind == K_IMAGE ? e_.path : "";
        const float img_w = 400;  // the picture and Play, on the right
        float text_w = W - x0 - 60 - img_w - 40;

        float y = 64;
        y += text::draw_wrapped(font::headline, Rect(x0, y, text_w, 90), e_.name, t.text, 2) + 8;
        text::draw_fit(font::small, x0, y, text_w, util::parent_dir(e_.path), t.text3);
        y += 46;
        float button_y = 70;
        if (!image.empty()) {
            Rect ir(W - 60 - img_w, 70, img_w, img_w * 9 / 16);
            button_y = ir.y + ir.h + 24;
            const images::Image* img = images::get(image, 400, 0);
            if (img && img->ready) {
                gfx::fill_rrect(ir, 14, Color(20, 18, 24, 255));
                gfx::image_cover(img->tex, img->w, img->h, ir, 14, Color(255, 255, 255, (uint8_t)(255 * images::fade(img))));
            } else if (img && !img->failed) {
                skeleton(ir, 14);
            } else {
                gfx::fill_rrect(ir, 14, t.surface);
                text::icon(kind_icon(e_.kind), 64, ir.cx(), ir.cy(), t.text3);
            }
        }

        auto row = [&](const char* label, const std::string& value) {
            text::draw_fit(font::body, x0, y, 180, label, t.text3);
            text::draw_fit(font::body_bold, x0 + 190, y, text_w - 190, value, t.text);
            y += 40;
        };
        row(tr("Type"), upper_ext(e_.name));
        row(tr("Size"), util::format_bytes(e_.size));
        if (media && have_ && info_.ok) {
            if (info_.duration > 0) row(tr("Length"), util::format_duration(info_.duration));
            if (!info_.video_codec.empty()) row(tr("Video"), video_line());
            row(tr("Sound"), sound_line());
        }
        if (!e_.subs.empty()) {
            std::string labels;
            for (const Sidecar& s : e_.subs) labels += (labels.empty() ? "" : ", ") + s.label;
            row(tr("Subtitles"), labels);
        }
        if (media) verdict_box(x0, y + 16, text_w);

        bool plays = e_.kind == K_VIDEO || e_.kind == K_AUDIO;
        Id g = id("media_info");
        if (button(id(g, "open"), Rect(W - 60 - img_w, button_y, img_w, 52), plays ? tr("Play") : tr("Open"),
                   plays ? ic::PLAY : kind_icon(e_.kind), BTN_PRIMARY, g, F_DEFAULT))
            open_file(siblings_, index_, "local", util::file_exists);
        hint_bar({{"A", plays ? tr("Play") : tr("Open")}, {"B", tr("Back")}});
    }

private:
    std::string video_line() const {
        const player::MediaInfo& m = info_;
        bool turned = m.rotation % 2 == 1;  // as it's shown: a phone video on its side is upright
        std::string s = player::codec_label(m.video_codec);
        if (m.width > 0) s += util::fmt(" \xC2\xB7 %d\xC3\x97%d", turned ? m.height : m.width, turned ? m.width : m.height);
        if (m.fps > 0) s += std::fabs(m.fps - std::round(m.fps)) < 0.01f ? util::fmt(" \xC2\xB7 %.0f fps", m.fps)
                                                                           : util::fmt(" \xC2\xB7 %.2f fps", m.fps);
        if (m.bit_depth > 8) s += util::fmt(" \xC2\xB7 %d-bit", m.bit_depth);
        if (m.hdr) s += " \xC2\xB7 HDR";
        return s;
    }

    std::string sound_line() const {
        const player::MediaInfo& m = info_;
        if (m.audio_tracks == 0) return tr("None");
        std::string s = player::codec_label(m.audio_codec);
        if (m.audio_tracks > 1) s += " \xC2\xB7 " + util::fmt(tr("%d tracks"), m.audio_tracks);
        if (!m.audio_supported) s += " \xC2\xB7 " + std::string(tr("not supported"));
        return s;
    }

    void verdict_box(float x, float y, float w) {
        const Theme& t = theme();
        std::string detail = have_ ? info_.reason : "";
        if (have_ && info_.verdict == player::Verdict::CONVERT && !info_.video_codec.empty())
            detail += std::string(detail.empty() ? "" : ". ") +
                      tr("Convert it to an 8-bit H.264 MP4 at 1080p or lower on your computer, then send it again.");
        Rect box(x, y, w, detail.empty() ? 88 : 150);
        gfx::fill_rrect(box, 18, t.surface);
        if (!have_) {
            spinner(box.x + 44, box.cy(), 16, t.accent, 4);
            text::draw(font::body, box.x + 84, box.cy() - 12, tr("Checking\xE2\x80\xA6"), t.text2);
            return;
        }
        player::Verdict v = info_.verdict;
        Color c = v == player::Verdict::READY ? t.good : v == player::Verdict::UNKNOWN ? t.text3 : t.warn;
        int icon = v == player::Verdict::READY     ? ic::CHECK_CIRCLE
                   : v == player::Verdict::LIMITED ? ic::INFO
                   : v == player::Verdict::CONVERT ? ic::WARNING
                                                   : ic::ERROR_OUTLINE;
        text::icon(icon, 36, box.x + 44, box.y + 44, c);
        text::draw(font::title, box.x + 84, box.y + 26, player::verdict_label(v), t.text);
        if (!detail.empty()) text::draw_wrapped(font::body, Rect(box.x + 84, box.y + 64, box.w - 110, 80), detail, t.text2, 3);
    }

    std::vector<Entry> siblings_;
    size_t index_;
    Entry e_;
    player::MediaInfo info_;
    bool have_ = false;
    tasks::Scope scope_;
};

// --- a folder --------------------------------------------------------------------------------------

class MediaScreen : public app::Screen {
public:
    explicit MediaScreen(std::string path = "", std::string title = "") : path_(std::move(path)), title_(std::move(title)) {
        if (path_.empty()) {
            ensure_default_folders();
            root_ = true;
        }
        dir_ = root_ ? platform::media_root() : path_;
        received_ = same_dir(dir_, received_folder());
        editable_ = editable_folder(dir_);
        sort_ = sort_from_key(store::get_str(sort_setting()), received_ ? SORT_NEWEST : SORT_NAME);
        reload();
    }

    app::Section section() const override { return app::SEC_MEDIA; }

    void on_enter() override {
        reload();
    }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(path_.empty() ? "media_root" : path_.c_str());
        Id grid_id = id(g, "grid");
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        float title_right = W - 60;
        if (root_) {
            if (button(id(g, "receive"), Rect(W - 340, y + 18, 280, 48), tr("Receive files"), ic::FILE_UPLOAD, BTN_PRIMARY, g))
                app::push(make_receive_files());
            title_right = W - 360;
        } else {
            // Header buttons, from the right.
            const char* sl = sort_label(sort_);
            float sw = measure_button(sl, IC_SORT);
            title_right -= sw;
            if (button(id(g, "sort"), Rect(title_right, y + 10, sw, 48), sl, IC_SORT, BTN_NORMAL, g)) choose_sort();
            if (editable_) {
                float nw = measure_button(tr("New folder"), IC_NEW_FOLDER);
                title_right -= nw + 14;
                if (button(id(g, "newfolder"), Rect(title_right, y + 10, nw, 48), tr("New folder"), IC_NEW_FOLDER, BTN_NORMAL, g))
                    ask_new_folder();
            }
            title_right -= 20;
        }
        text::draw_fit(font::display, x0, y, title_right - x0, root_ ? tr("My Media") : title_, t.text);
        y += 66;
        if (!root_) {
            text::draw_fit(font::small, x0, y, W - x0 - 60, path_, t.text3);
            y += 40;
        } else {
            text::draw(font::body, x0, y, tr("Your videos, music and photos on the SD card and network."), t.text2);
            y += 50;
            y = draw_roots(x0, y, g);
            text::draw(font::title, x0, y, tr("CoffeeFlix folder"), t.text);
            const char* sl = sort_label(sort_);
            float sw = measure_button(sl, IC_SORT);
            if (button(id(g, "sort"), Rect(W - 60 - sw, y - 6, sw, 44), sl, IC_SORT, BTN_GHOST, g)) choose_sort();
            y += 50;
        }

        if (restore_focus_ && !loading_) apply_focus(grid_id);
        int focused = -1;
        if (!loading_ && !listing_.ok) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::ERROR_OUTLINE, listing_.error.c_str(), "");
            y += 280;
        } else if (!loading_ && listing_.entries.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::FOLDER, tr("Nothing here yet"),
                        tr("Use Receive files in My Media to send files over Wi-Fi."));
            y += 300;
        } else {
            GridSpec gs;
            gs.count = (int)listing_.entries.size();
            gs.cols = 4;
            gs.item_w = 252;
            gs.loading = loading_;
            gs.shape = CARD_WIDE;
            gs.item = [this](int i) { return card_for(listing_.entries[i]); };
            gs.on_click = [this](int i) { open((size_t)i); };
            gs.on_focus = [&focused](int i) { focused = i; };
            gs.on_x = [this](int i) {
                if (has_more(listing_.entries[i])) more((size_t)i);
            };
            y += grid(grid_id, x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        pump_probes();
        if (focused >= 0 && has_more(listing_.entries[focused]))
            hint_bar({{"A", tr("Open")}, {"X", tr("More")}, {"B", tr("Back")}});
        else if (focused >= 0) hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
        else hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    float draw_roots(float x0, float y, Id g) {
        platform::Volume vols[8];
        int n = platform::volumes(vols, 8);
        auto shares = smb::saved_shares();
        ShelfSpec ss;
        ss.count = n + 3;
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
            } else if (i == (int)v.size()) {
                c.title = tr("Network shares");
                c.icon = ic::LAN;
                c.subtitle = nshares == 0 ? tr("Add a PC or NAS") : util::fmt(tr("%zu saved"), nshares);
            } else if (i == (int)v.size()+1) {
                c.title = tr("Received files"); c.icon = ic::FOLDER; c.subtitle = tr("Sent from your devices");
            } else {
                c.title = tr("Media servers");
                c.icon = ic::DNS;
                c.subtitle = tr("Found automatically (DLNA)");
            }
            return c;
        };
        ss.on_click = [v](int i) {
            if (i < (int)v.size()) app::push(make_media_folder(v[i].path, v[i].label));
            else if (i == (int)v.size()) app::push(smb_browser_screen());
            else if (i == (int)v.size()+1) { auto folder=received_folder(); util::make_dirs(folder); app::push(make_media_folder(folder,tr("Received files"))); }
            else app::push(dlna_servers_screen());
        };
        return y + shelf(id(g, "roots"), x0, y, ss, &page_) + 10;
    }

    const char* sort_setting() const { return received_ ? "media_sort_received" : "media_sort"; }

    CardInfo card_for(const Entry& e) const {
        CardInfo c = entry_card(e, "local");
        auto it = infos_.find(e.path);
        if (it == infos_.end()) return c;
        const player::MediaInfo& m = it->second;
        if (m.duration >= 1) c.subtitle += " \xC2\xB7 " + util::format_duration(m.duration);
        if (m.verdict == player::Verdict::CONVERT) {
            c.tag = player::verdict_label(m.verdict);
            c.tag_color = theme().warn;
        } else if (m.verdict == player::Verdict::LIMITED) {
            c.tag = player::verdict_label(m.verdict);
            c.tag_text = theme().warn;
        }
        return c;
    }

    void open(size_t i) {
        const Entry& e = listing_.entries[i];
        if (e.kind == K_DIR) app::push(std::make_unique<MediaScreen>(e.path, e.name));
        else open_file(listing_.entries, i, "local", util::file_exists);
    }

    void open_path(const std::string& path) {
        for (size_t i = 0; i < listing_.entries.size(); i++)
            if (listing_.entries[i].path == path) return open(i);
    }

    // Files in CoffeeFlix's folders can be renamed and deleted; links and the top folders aren't.
    bool can_change(const Entry& e) const {
        return editable_ && !e.link && e.kind != K_OTHER && inside(e.path, platform::media_root());
    }

    // --- the X menu ----------------------------------------------------------------------------

    bool has_more(const Entry& e) const { return e.kind != K_DIR || can_change(e); }  // more than Open

    void more(size_t i) {
        const Entry e = listing_.entries[i];
        std::vector<MenuItem> items;
        bool plays = e.kind == K_VIDEO || e.kind == K_AUDIO;
        items.push_back({plays ? tr("Play") : tr("Open"), plays ? ic::PLAY : e.kind == K_DIR ? ic::FOLDER : kind_icon(e.kind),
                         [this, path = e.path] { open_path(path); }});
        if (e.kind != K_DIR) items.push_back({tr("Info"), ic::INFO, [this, path = e.path] { show_info(path); }});
        if (can_change(e)) {
            items.push_back({tr("Rename"), ic::EDIT, [this, e] { ask_rename(e); }});
            items.push_back({tr("Delete"), ic::DELETE, [this, e, i] { confirm_delete(e, i); }});
        }
        CardInfo c = card_for(e);
        show_menu(e.name, c.subtitle, std::move(items));
    }

    void show_info(const std::string& path) {
        for (size_t i = 0; i < listing_.entries.size(); i++) {
            if (listing_.entries[i].path != path) continue;
            auto it = infos_.find(path);
            return app::push(std::make_unique<InfoScreen>(listing_.entries, i, it == infos_.end() ? nullptr : &it->second));
        }
    }

    void choose_sort() {
        std::vector<MenuItem> items;
        for (SortMode m : {SORT_NAME, SORT_NEWEST, SORT_SIZE})
            items.push_back({sort_label(m), m == sort_ ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED, [this, m] { set_sort(m); }});
        show_menu(tr("Sort by"), received_ ? tr("Received files") : root_ ? tr("CoffeeFlix folder") : title_, std::move(items));
    }

    void set_sort(SortMode m) {
        if (m == sort_) return;
        sort_ = m;
        store::set_str(sort_setting(), sort_key(m));
        sort_entries(listing_.entries, m);
        forget_group(ui::id(ui::id(id(path_.empty() ? "media_root" : path_.c_str()), "grid"), "cells"));
    }

    void ask_rename(const Entry& e) {
        bool dir = e.kind == K_DIR;
        prompt_text(tr("Rename"), dir ? e.name : strip_ext(e.name), dir ? tr("Folder name") : tr("File name"),
                    [this, e](std::string typed) {
                        std::string problem = name_problem(renamed(e.name, typed, e.kind == K_DIR));
                        if (util::trim(typed).empty()) problem = tr("Enter a name");
                        if (!problem.empty()) return toast(problem, ic::ERROR_OUTLINE, theme().warn);
                        std::string root = platform::media_root();
                        ops_.run<FileOp>(
                            [root, e, typed] {
                                FileOp r = rename_entry(root, e, typed);
                                if (r.ok && r.path != e.path && e.kind == K_VIDEO) player::forget_thumbnail(e.path);
                                return r;
                            },
                            [this, e](FileOp r) {
                                if (!r.ok) return toast(r.error, ic::ERROR_OUTLINE, theme().warn);
                                if (r.path == e.path) return;
                                move_resume_points(e.path, r.path, e.kind == K_DIR);
                                focus_path_ = r.path;
                                reload();
                            });
                    });
    }

    void confirm_delete(const Entry& e, size_t index) {
        if (e.kind == K_DIR) return remove_entry(e, index);  // only when empty: nothing is lost
        show_menu(util::fmt(tr("Delete \"%s\"?"), e.name.c_str()),
                  e.subs.empty() ? tr("It can't be brought back.") : tr("Its subtitle files are deleted too."),
                  {{tr("Cancel"), ic::CLOSE, [] {}}, {tr("Delete"), ic::DELETE, [this, e, index] { remove_entry(e, index); }}});
    }

    void remove_entry(const Entry& e, size_t index) {
        std::string root = platform::media_root();
        ops_.run<FileOp>(
            [root, e] {
                FileOp r = e.kind == K_DIR ? delete_folder(root, e.path) : delete_file(root, e);
                if (r.ok && e.kind == K_VIDEO) player::forget_thumbnail(e.path);
                return r;
            },
            [this, e, index](FileOp r) {
                if (!r.ok) return toast(r.error, ic::ERROR_OUTLINE, theme().warn);
                store::resume_remove("local", e.path);
                infos_.erase(e.path);
                toast(util::fmt(tr("Deleted %s"), e.name.c_str()), ic::DELETE);
                focus_index_ = (int)index;
                reload();
            });
    }

    void ask_new_folder() {
        prompt_text(tr("New folder"), "", tr("Folder name"), [this](std::string typed) {
            std::string problem = name_problem(typed);
            if (!problem.empty()) return toast(problem, ic::ERROR_OUTLINE, theme().warn);
            std::string root = platform::media_root(), dir = dir_;
            ops_.run<FileOp>([root, dir, typed] { return make_folder(root, dir, typed); },
                             [this](FileOp r) {
                                 if (!r.ok) return toast(r.error, ic::ERROR_OUTLINE, theme().warn);
                                 focus_path_ = r.path;
                                 reload();
                             });
        });
    }

    // After a change: the renamed or new item, else the one that took the deleted one's place.
    void apply_focus(Id grid_id) {
        restore_focus_ = false;
        int idx = -1;
        for (size_t i = 0; i < listing_.entries.size() && !focus_path_.empty(); i++)
            if (listing_.entries[i].path == focus_path_) idx = (int)i;
        if (idx < 0 && focus_index_ >= 0 && !listing_.entries.empty())
            idx = std::min(focus_index_, (int)listing_.entries.size() - 1);
        if (idx >= 0) set_focus(ui::id(ui::id(grid_id, "cells"), (int64_t)idx));
        else if (focus_index_ >= 0) reset_focus();
        focus_path_.clear();
        focus_index_ = -1;
    }

    // --- durations and verdicts, one file at a time ------------------------------------------------

    void queue_probes() {
        probe_scope_.reset();
        probing_ = false;
        probe_queue_.clear();
        probe_next_ = 0;
        for (const Entry& e : listing_.entries)
            if (e.kind == K_VIDEO) probe_queue_.push_back(e.path);
    }

    // Called while the folder is on screen: leaving it (a subfolder, the player) pauses the queue.
    void pump_probes() {
        if (probing_ || probe_next_ >= probe_queue_.size()) return;
        probing_ = true;
        size_t end = std::min(probe_queue_.size(), probe_next_ + 48);
        std::vector<std::string> batch(probe_queue_.begin() + probe_next_, probe_queue_.begin() + end);
        using Results = std::vector<std::pair<std::string, player::MediaInfo>>;
        probe_scope_.run<Results>(
            [batch] {
                // Those already known at once, else one file read.
                Results out;
                for (const std::string& path : batch) {
                    player::MediaInfo m;
                    if (player::probe_cached(path, m)) {
                        out.emplace_back(path, std::move(m));
                        continue;
                    }
                    if (out.empty()) out.emplace_back(path, player::probe_media(path));
                    break;
                }
                return out;
            },
            [this](Results results) {
                probing_ = false;
                probe_next_ += std::max<size_t>(1, results.size());
                for (auto& r : results) infos_[r.first] = std::move(r.second);
                if (probe_next_ >= probe_queue_.size())
                    tasks::submit(tasks::API, [] {
                        player::flush_probe_cache();
                        return std::function<void()>();
                    });
            });
    }

    std::string path_, title_, dir_;
    bool root_ = false, received_ = false, editable_ = false;
    bool loading_ = true;
    SortMode sort_ = SORT_NAME;
    Listing listing_;
    tasks::Scope scope_, ops_, probe_scope_;
    Page page_;

    std::unordered_map<std::string, player::MediaInfo> infos_;  // by path
    std::vector<std::string> probe_queue_;
    size_t probe_next_ = 0;
    bool probing_ = false;

    bool restore_focus_ = false;
    std::string focus_path_;
    int focus_index_ = -1;

    void reload() {
        std::string dir = dir_;
        SortMode sort = sort_;
        std::string lang = i18n::current().code;
        loading_ = listing_.entries.empty();
        scope_.reset();
        scope_.run<Listing>([dir, sort, lang] { return list_local(dir, sort, lang); },
                            [this](Listing l) {
                                listing_ = std::move(l);
                                loading_ = false;
                                if (!focus_path_.empty() || focus_index_ >= 0) restore_focus_ = true;
                                queue_probes();
                            });
    }
};

}  // namespace

std::unique_ptr<app::Screen> make_media() { return std::make_unique<MediaScreen>(); }
std::unique_ptr<app::Screen> make_media_folder(const std::string& path, const std::string& title) {
    return std::make_unique<MediaScreen>(path, title);
}

}  // namespace screens
