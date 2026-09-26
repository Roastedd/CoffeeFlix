#include "screens/media_actions.hpp"

#include <algorithm>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "services/smb.hpp"
#include "ui/ui.hpp"

namespace screens::media {

Kind kind_of(const std::string& name) {
    std::string e = util::file_extension(name);
    static const char* video[] = {"mp4", "m4v", "mkv", "webm", "avi", "mov", "ts", "m2ts", "mpg", "mpeg", "flv", "3gp"};
    static const char* audio[] = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wv", "alac", "oga", "mka"};
    static const char* image[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
    static const char* book[] = {"cbz", "epub"};
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

void sort_entries(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if ((a.kind == K_DIR) != (b.kind == K_DIR)) return a.kind == K_DIR;
        return util::natural_less(a.name, b.name);
    });
}

std::string strip_ext(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

CardInfo entry_card(const Entry& e, const char* service) {
    CardInfo c;
    c.title = e.kind == K_DIR ? e.name : strip_ext(e.name);
    c.icon = kind_icon(e.kind);
    if (e.kind == K_IMAGE) {
        c.image = e.path;
        c.image_w = 360;
    } else if (e.kind == K_VIDEO && !smb::is_url(e.path)) {
        c.image = "thumb://" + e.path;  // a frame from the video (local files only)
        c.image_w = 320;
    }
    if (e.kind == K_DIR) {
        c.subtitle = tr("Folder");
    } else {
        std::string ext = util::file_extension(e.name);
        for (auto& ch : ext) ch = (char)toupper((unsigned char)ch);
        c.subtitle = ext + " \xC2\xB7 " + util::format_bytes(e.size);
    }
    if (e.kind == K_VIDEO && store::resume_position(service, e.path) > 0) c.badge = tr("Resume");
    return c;
}

void open_file(const std::vector<Entry>& siblings, size_t index, const char* service,
               const std::function<bool(const std::string&)>& exists) {
    const Entry& e = siblings[index];
    switch (e.kind) {
        case K_VIDEO: {
            player::Source s;
            s.url = e.path;
            s.title = strip_ext(e.name);
            s.subtitle = util::file_name(util::parent_dir(e.path));
            s.service = service;
            s.id = e.path;
            s.start = store::resume_position(service, e.path);
            std::string base = util::parent_dir(e.path) + "/" + strip_ext(e.name);
            for (const char* ext : {".srt", ".vtt", ".en.srt"})
                if (exists(base + ext)) s.external_subs.push_back({util::file_name(base + ext), base + ext});
            play_video(s);
            break;
        }
        case K_AUDIO: {
            std::vector<player::Source> q;
            int idx = 0;
            for (const Entry& o : siblings) {
                if (o.kind != K_AUDIO) continue;
                if (o.path == e.path) idx = (int)q.size();
                player::Source s;
                s.url = o.path;
                s.service = service;
                s.id = o.path;
                s.remember_position = false;
                // Folder art (cover.jpg / folder.jpg) if there is no embedded cover.
                for (const char* art : {"cover.jpg", "folder.jpg", "cover.png", "Folder.jpg", "Cover.jpg"}) {
                    std::string p = util::join_path(util::parent_dir(o.path), art);
                    if (exists(p)) { s.artwork = p; break; }
                }
                q.push_back(std::move(s));
            }
            play_audio_queue(std::move(q), idx);
            break;
        }
        case K_IMAGE: {
            std::vector<std::string> paths;
            int idx = 0;
            for (const Entry& o : siblings) {
                if (o.kind != K_IMAGE) continue;
                if (o.path == e.path) idx = (int)paths.size();
                paths.push_back(o.path);
            }
            app::push(make_photo_viewer(std::move(paths), idx));
            break;
        }
        case K_BOOK:
            // The reader opens documents from the SD card only.
            if (smb::is_url(e.path)) ui::toast(tr("Copy books to the SD card to read them"), ic::BOOK);
            else app::push(make_reader(e.path));
            break;
        default: break;
    }
}

}  // namespace screens::media
