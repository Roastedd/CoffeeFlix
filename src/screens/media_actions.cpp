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
            std::vector<Sidecar> subs = e.subs;
            if (!e.subs_listed) {
                // Network shares: the usual names, looked up one by one.
                std::string base = strip_ext(e.name);
                for (const char* ext : {".srt", ".vtt", ".ass", ".ssa", ".en.srt"}) {
                    Sidecar sc;
                    std::string path = util::parent_dir(e.path) + "/" + base + ext;
                    if (!exists(path) || !match_sidecar(e.name, base + ext, sc)) continue;
                    sc.path = path;
                    subs.push_back(std::move(sc));
                }
                order_sidecars(subs, i18n::current().code);
            }
            for (const Sidecar& sc : subs) s.external_subs.push_back({sc.label, sc.path});
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
