// Media servers (DLNA / UPnP): the servers found on the network and a folder
// browser that plays videos, music and photos straight from them.
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/dlna.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

int kind_icon(dlna::Kind k) {
    switch (k) {
        case dlna::VIDEO: return ic::MOVIE;
        case dlna::AUDIO: return ic::MUSIC;
        case dlna::IMAGE: return ic::PHOTO;
        default: return ic::FOLDER;
    }
}

player::Source source_for(const dlna::Server& server, const dlna::Item& it) {
    player::Source s;
    s.url = it.url;
    s.title = it.title;
    s.subtitle = it.kind == dlna::AUDIO && !it.artist.empty() ? it.artist : server.name;
    s.artwork = it.art;
    s.service = "dlna";
    s.id = it.url;  // stable on real servers; lets Continue Watching reopen it directly
    if (it.kind == dlna::VIDEO) {
        s.start = store::resume_position("dlna", it.url);
        if (!it.subtitles.empty()) s.external_subs.push_back({tr("Subtitles"), it.subtitles});
    } else {
        s.remember_position = false;
    }
    return s;
}

class BrowseScreen : public app::Screen {
public:
    BrowseScreen(dlna::Server server, std::string object_id, std::string title, std::string where)
        : server_(std::move(server)), object_id_(std::move(object_id)), title_(std::move(title)), where_(std::move(where)) {}

    app::Section section() const override { return app::SEC_MEDIA; }

    void on_enter() override { reload(); }  // also refreshes resume badges after playback

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(id(server_.udn.c_str()), object_id_);
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        text::draw_fit(font::display, x0, y, W - x0 - 60, title_, t.text);
        y += 66;
        text::draw_fit(font::small, x0, y, W - x0 - 60, where_, t.text3);
        y += 40;

        bool failed = !loading_ && !listing_.ok;
        if (failed) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 300), ic::ERROR_OUTLINE,
                                   tr("Can't open this folder"), listing_.error.c_str()))
                reload();
            y += 340;
        } else if (!loading_ && listing_.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::FOLDER, tr("Nothing to play here"),
                        tr("This folder has no videos, music or photos."));
            y += 300;
        } else {
            GridSpec gs;
            gs.count = (int)listing_.items.size();
            gs.cols = 4;
            gs.item_w = 252;
            gs.loading = loading_;
            gs.shape = CARD_WIDE;
            gs.item = [this](int i) { return card(listing_.items[i]); };
            gs.on_click = [this](int i) { open((size_t)i); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", failed ? tr("Try again") : tr("Open")}, {"B", tr("Back")}});
    }

private:
    CardInfo card(const dlna::Item& it) const {
        CardInfo c;
        c.title = it.title;
        c.icon = kind_icon(it.kind);
        c.image = it.art;
        c.image_w = 360;
        switch (it.kind) {
            case dlna::CONTAINER:
                c.subtitle = it.child_count >= 0 ? util::fmt(it.child_count == 1 ? tr("%d item") : tr("%d items"), it.child_count)
                                                 : tr("Folder");
                break;
            case dlna::VIDEO:
                c.subtitle = it.duration > 0 ? util::format_duration(it.duration) : tr("Video");
                if (it.size) c.subtitle += " \xC2\xB7 " + util::format_bytes(it.size);
                if (store::resume_position("dlna", it.url) > 0) c.badge = tr("Resume");
                break;
            case dlna::AUDIO:
                c.subtitle = !it.artist.empty() ? it.artist : it.album;
                if (it.duration > 0) c.badge = util::format_duration(it.duration);
                break;
            case dlna::IMAGE: c.subtitle = tr("Photo"); break;
        }
        return c;
    }

    void open(size_t i) {
        const dlna::Item& it = listing_.items[i];
        switch (it.kind) {
            case dlna::CONTAINER:
                app::push(std::make_unique<BrowseScreen>(server_, it.id, it.title, where_ + " \xE2\x80\xBA " + it.title));
                break;
            case dlna::VIDEO: play_video(source_for(server_, it)); break;
            case dlna::AUDIO: {
                // Queue the folder's tracks starting at this one.
                std::vector<player::Source> q;
                int idx = 0;
                for (const dlna::Item& o : listing_.items) {
                    if (o.kind != dlna::AUDIO) continue;
                    if (&o == &it) idx = (int)q.size();
                    q.push_back(source_for(server_, o));
                }
                play_audio_queue(std::move(q), idx);
                break;
            }
            case dlna::IMAGE: {
                std::vector<std::string> urls;
                int idx = 0;
                for (const dlna::Item& o : listing_.items) {
                    if (o.kind != dlna::IMAGE) continue;
                    if (&o == &it) idx = (int)urls.size();
                    urls.push_back(o.url);
                }
                app::push(make_photo_viewer(std::move(urls), idx));
                break;
            }
        }
    }

    void reload() {
        loading_ = listing_.items.empty();
        scope_.reset();
        dlna::Server server = server_;
        std::string oid = object_id_;
        scope_.run<dlna::Listing>([server, oid] { return dlna::browse(server, oid); },
                                  [this](dlna::Listing l) {
                                      // Keep what is shown if a refresh fails.
                                      if (l.ok || listing_.items.empty()) listing_ = std::move(l);
                                      loading_ = false;
                                  });
    }

    dlna::Server server_;
    std::string object_id_, title_, where_;
    dlna::Listing listing_;
    bool loading_ = true;
    tasks::Scope scope_;
    Page page_;
};

class ServersScreen : public app::Screen {
public:
    ServersScreen() : servers_(dlna::known_servers()) { search(); }

    app::Section section() const override { return app::SEC_MEDIA; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("dlna_servers");
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        text::draw(font::display, x0, y, tr("Media servers"), t.text);
        y += 66;
        text::draw(font::body, x0, y, tr("Plex, Jellyfin, Emby, NAS and PC media servers on your network (DLNA)."), t.text2);
        y += 50;

        if (!searching_ && servers_.empty()) {
            if (empty_state_action(id(g, "again"), Rect(x0, y, W - x0 - 60, 320), ic::DNS, tr("No media servers found"),
                                   tr("Make sure the server has DLNA sharing turned on and is on the same network as your Wii U."),
                                   tr("Search again"), ic::REFRESH))
                search();
            y += 360;
        } else {
            GridSpec gs;
            gs.count = (int)servers_.size();
            gs.cols = 4;
            gs.item_w = 252;
            gs.loading = searching_;
            gs.shape = CARD_WIDE;
            gs.item = [this](int i) {
                CardInfo c;
                c.title = servers_[i].name;
                c.subtitle = servers_[i].model;
                c.icon = ic::DNS;
                c.image = servers_[i].icon;
                return c;
            };
            gs.on_click = [this](int i) {
                const dlna::Server& s = servers_[i];
                app::push(std::make_unique<BrowseScreen>(s, "0", s.name, s.name));
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
            if (input().pressed_(BTN_Y) && !searching_) {
                input().eat(BTN_Y);
                search();
            }
        }
        page_.end(y + page_.scroll());
        if (servers_.empty()) hint_bar({{"A", tr("Search again")}, {"B", tr("Back")}});
        else hint_bar({{"A", tr("Open")}, {"Y", tr("Search again")}, {"B", tr("Back")}});
    }

private:
    void search() {
        searching_ = true;
        scope_.reset();
        scope_.run<dlna::Servers>([] { return dlna::discover(); },
                                  [this](dlna::Servers s) {
                                      servers_ = std::move(s.items);
                                      searching_ = false;
                                  });
    }

    std::vector<dlna::Server> servers_;
    bool searching_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> dlna_servers_screen() { return std::make_unique<ServersScreen>(); }

}  // namespace screens
