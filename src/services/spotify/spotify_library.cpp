// The account's library, read as Spotify's own apps read it (spclient): the playlists (playlist4), what's
// saved (collection v2) and what the songs, albums and artists are (extended metadata). The public Web API
// would be simpler, but it answers 429 to every app signed in the way librespot signs in.
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <mutex>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/podcasts.hpp"
#include "services/spotify/spotify.hpp"
#include "services/spotify/spotify_index.hpp"
#include "services/spotify/spotify_internal.hpp"
#include "services/spotify/spotify_proto.hpp"

namespace spotify {

using namespace detail;

namespace {

// ExtensionKind
constexpr int ARTIST_V4 = 8, ALBUM_V4 = 9, TRACK_V4 = 10, SHOW_V4 = 11, EPISODE_V4 = 12;
constexpr size_t MAX_ITEMS = 10000;  // of a playlist (Spotify's own limit) or the liked songs

int64_t sint(uint64_t v) { return (int64_t)(int32_t)((v >> 1) ^ (~(v & 1) + 1)); }

std::string image_url(const std::vector<uint8_t>& id) { return id.empty() ? "" : "https://i.scdn.co/image/" + hex(id); }

// An ImageGroup's picture of about 300 px (DEFAULT), else whichever there is.
std::string pick_image(PRd group) {
    std::string best;
    uint32_t f;
    uint8_t w;
    while (group.next(f, w)) {
        if (f != 1 || w != 2) { group.skip(w); continue; }
        PRd img;
        group.enter(img);
        std::vector<uint8_t> id;
        uint64_t size = 0;
        while (img.next(f, w))
            if (f == 1 && w == 2) img.read_bytes(id);
            else if (f == 2 && w == 0) img.vi(size);
            else img.skip(w);
        if (!id.empty() && (best.empty() || size == 0)) best = image_url(id);
    }
    return best;
}

std::string name_of(PRd msg) {  // field 2 of an Artist, Album or Show
    std::string name;
    uint32_t f;
    uint8_t w;
    while (msg.next(f, w))
        if (f == 2 && w == 2) msg.read_str(name);
        else msg.skip(w);
    return name;
}

std::vector<uint8_t> gid_of(PRd msg) {  // field 1
    std::vector<uint8_t> gid;
    uint32_t f;
    uint8_t w;
    while (msg.next(f, w))
        if (f == 1 && w == 2) msg.read_bytes(gid);
        else msg.skip(w);
    return gid;
}

void add_name(std::string& list, const std::string& name) {
    if (!name.empty()) list += (list.empty() ? "" : ", ") + name;
}

Item parse_track(const std::string& uri, const std::string& body) {
    Item it;
    it.uri = uri;
    PRd rd = reader(body);
    uint32_t f;
    uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(it.name);
        } else if (f == 7 && w == 0) {
            uint64_t v;
            rd.vi(v);
            it.duration_ms = sint(v);
        } else if (f == 4 && w == 2) {
            PRd a;
            rd.enter(a);
            add_name(it.subtitle, name_of(a));
        } else if (f == 3 && w == 2) {
            PRd alb;
            rd.enter(alb);
            while (alb.next(f, w)) {
                if (f == 17 && w == 2) {
                    PRd g;
                    alb.enter(g);
                    it.image = pick_image(g);
                } else {
                    alb.skip(w);
                }
            }
        } else {
            rd.skip(w);
        }
    }
    return it;
}

Item parse_episode(const std::string& uri, const std::string& body) {
    Item it;
    it.uri = uri;
    PRd rd = reader(body);
    uint32_t f;
    uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(it.name);
        } else if (f == 7 && w == 0) {
            uint64_t v;
            rd.vi(v);
            it.duration_ms = sint(v);
        } else if (f == 71 && w == 2) {
            PRd show;
            rd.enter(show);
            it.subtitle = name_of(show);
        } else if (f == 66 && w == 2) {  // publish_time: Date { year 1, month 2, day 3, hour 4, minute 5 }
            PRd d;
            rd.enter(d);
            int64_t part[6] = {0, 0, 0, 0, 0, 0};
            uint64_t v;
            while (d.next(f, w))
                if (f >= 1 && f <= 5 && w == 0 && d.vi(v)) part[f] = sint(v);
                else d.skip(w);
            it.released = (((part[1] * 100 + part[2]) * 100 + part[3]) * 100 + part[4]) * 100 + part[5];
        } else if (f == 68 && w == 2) {
            PRd g;
            rd.enter(g);
            it.image = pick_image(g);
        } else {
            rd.skip(w);
        }
    }
    return it;
}

// An Album: its name, artists, year, cover; and the gids of its songs, disc by disc.
Item parse_album(const std::string& uri, const std::string& body, std::vector<std::vector<uint8_t>>* tracks = nullptr) {
    Item it;
    it.uri = uri;
    PRd rd = reader(body);
    uint32_t f;
    uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(it.name);
        } else if (f == 3 && w == 2) {
            PRd a;
            rd.enter(a);
            add_name(it.subtitle, name_of(a));
        } else if (f == 6 && w == 2) {  // Date { year 1 } (sint32, as all of Date)
            PRd d;
            rd.enter(d);
            uint64_t v;
            while (d.next(f, w))
                if (f == 1 && w == 0 && d.vi(v)) it.year = (int)sint(v);
                else d.skip(w);
        } else if (f == 17 && w == 2) {
            PRd g;
            rd.enter(g);
            it.image = pick_image(g);
        } else if (f == 11 && w == 2 && tracks) {  // Disc { track 3 }
            PRd disc;
            rd.enter(disc);
            while (disc.next(f, w)) {
                if (f == 3 && w == 2) {
                    PRd t;
                    disc.enter(t);
                    std::vector<uint8_t> gid = gid_of(t);
                    if (gid.size() == 16) tracks->push_back(std::move(gid));
                } else {
                    disc.skip(w);
                }
            }
        } else {
            rd.skip(w);
        }
    }
    if (tracks) it.count = (int)tracks->size();
    return it;
}

// A Show: name, publisher, cover; and its episodes' gids (in the order the show has them).
Item parse_show(const std::string& uri, const std::string& body, std::vector<std::vector<uint8_t>>* episodes = nullptr) {
    Item it;
    it.uri = uri;
    PRd rd = reader(body);
    uint32_t f;
    uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(it.name);
        } else if (f == 66 && w == 2) {
            rd.read_str(it.subtitle);
        } else if (f == 69 && w == 2) {
            PRd g;
            rd.enter(g);
            it.image = pick_image(g);
        } else if (f == 70 && w == 2) {
            PRd e;
            rd.enter(e);
            std::vector<uint8_t> gid = gid_of(e);
            if (gid.size() == 16) {
                it.count++;
                if (episodes) episodes->push_back(std::move(gid));
            }
        } else {
            rd.skip(w);
        }
    }
    return it;
}

// What extended metadata knows of `uris` (one kind): each one's message, by URI. Asked 100 at a time.
std::map<std::string, std::string> fetch_metadata(const std::vector<std::string>& uris, int kind, std::string& error) {
    std::map<std::string, std::string> out;
    for (size_t at = 0; at < uris.size(); at += 100) {
        std::vector<uint8_t> req;
        for (size_t i = at; i < std::min(uris.size(), at + 100); i++) {
            std::vector<uint8_t> query, entity;
            pb_u32(query, 1, (uint32_t)kind);
            pb_str(entity, 1, uris[i]);
            pb_msg(entity, 2, query);
            pb_msg(req, 2, entity);
        }
        http::Response r = sp_get("/extended-metadata/v0/extended-metadata", nullptr, error, &req);
        if (!r.ok()) return out;
        // BatchedExtensionResponse > extended_metadata (2) > extension_data (3) { entity_uri 2, Any 3 { value 2 } }
        PRd rd = reader(r.body);
        uint32_t f;
        uint8_t w;
        while (rd.next(f, w)) {
            if (f != 2 || w != 2) { rd.skip(w); continue; }
            PRd arr;
            rd.enter(arr);
            while (arr.next(f, w)) {
                if (f != 3 || w != 2) { arr.skip(w); continue; }
                PRd data;
                arr.enter(data);
                std::string uri, value;
                while (data.next(f, w)) {
                    if (f == 2 && w == 2) {
                        data.read_str(uri);
                    } else if (f == 3 && w == 2) {
                        PRd any;
                        data.enter(any);
                        while (any.next(f, w))
                            if (f == 2 && w == 2) any.read_str(value);
                            else any.skip(w);
                    } else {
                        data.skip(w);
                    }
                }
                if (!uri.empty() && !value.empty()) out[uri] = std::move(value);
            }
        }
    }
    return out;
}

// Songs and episodes for `uris`, in their order; ones Spotify knows nothing of are left out.
std::vector<Item> hydrate(const std::vector<std::string>& uris, std::string& error) {
    std::vector<std::string> tracks, episodes;
    for (const std::string& u : uris) {
        if (util::starts_with(u, "spotify:track:")) tracks.push_back(u);
        else if (util::starts_with(u, "spotify:episode:")) episodes.push_back(u);
    }
    const auto t = fetch_metadata(tracks, TRACK_V4, error);
    const auto e = fetch_metadata(episodes, EPISODE_V4, error);
    std::vector<Item> out;
    for (const std::string& u : uris) {
        if (auto it = t.find(u); it != t.end()) out.push_back(parse_track(u, it->second));
        else if (auto ie = e.find(u); ie != e.end()) out.push_back(parse_episode(u, ie->second));
    }
    if (!out.empty()) error.clear();
    return out;
}

std::vector<Item> hydrate_albums(const std::vector<std::string>& uris, std::string& error) {
    const auto m = fetch_metadata(uris, ALBUM_V4, error);
    std::vector<Item> out;
    for (const std::string& u : uris)
        if (auto it = m.find(u); it != m.end()) out.push_back(parse_album(u, it->second));
    if (!out.empty()) error.clear();
    return out;
}

// One collection set ("collection", "artist", "listenlater"): its URIs, most recently added first.
std::vector<std::string> collection(const char* set, std::string& error) {
    struct Entry {
        std::string uri;
        int64_t added = 0;
    };
    std::vector<Entry> entries;
    std::string token;
    for (int page = 0; page < 40; page++) {
        std::vector<uint8_t> req;
        pb_str(req, 1, user_name());
        pb_str(req, 2, set);
        if (!token.empty()) pb_str(req, 3, token);
        pb_u32(req, 4, 300);
        http::Response r =
            sp_get("/collection/v2/paging", nullptr, error, &req, "application/vnd.collection-v2.spotify.proto");
        if (!r.ok()) return {};
        std::string next;
        PRd rd = reader(r.body);
        uint32_t f;
        uint8_t w;
        while (rd.next(f, w)) {
            if (f == 1 && w == 2) {  // CollectionItem { uri 1, added_at 2, is_removed 3 }
                PRd item;
                rd.enter(item);
                Entry e;
                bool removed = false;
                uint64_t v;
                while (item.next(f, w)) {
                    if (f == 1 && w == 2) item.read_str(e.uri);
                    else if (f == 2 && w == 0 && item.vi(v)) e.added = (int64_t)v;
                    else if (f == 3 && w == 0 && item.vi(v)) removed = v != 0;
                    else item.skip(w);
                }
                entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& x) { return x.uri == e.uri; }),
                              entries.end());
                if (!removed && !e.uri.empty()) entries.push_back(std::move(e));
            } else if (f == 2 && w == 2) {
                rd.read_str(next);
            } else {
                rd.skip(w);
            }
        }
        if (next.empty() || next == token) break;
        token = next;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.added > b.added; });
    std::vector<std::string> out;
    for (Entry& e : entries) out.push_back(std::move(e.uri));
    return out;
}

std::vector<std::string> only(const std::vector<std::string>& uris, const char* prefix) {
    std::vector<std::string> out;
    for (const std::string& u : uris)
        if (util::starts_with(u, prefix)) out.push_back(u);
    return out;
}

// A SelectedListContent (a playlist, or the rootlist of them): the items' URIs, and the attributes of each
// item's playlist where there are any (meta_items, the rootlist's).
struct ListPage {
    int64_t total = 0;
    std::string name;  // the playlist's own (attributes 3 > name 1)
    std::vector<std::string> uris;
    std::vector<Item> meta;  // name, image, count, owner
    std::vector<std::string> revisions;  // each meta item's playlist's revision (hex), "" when not given
};

ListPage parse_list(const std::string& body) {
    ListPage p;
    PRd rd = reader(body);
    uint32_t f;
    uint8_t w;
    auto attributes = [](PRd a, Item& it) {
        uint32_t f;
        uint8_t w;
        while (a.next(f, w)) {
            if (f == 1 && w == 2) {
                a.read_str(it.name);
            } else if (f == 3 && w == 2) {  // picture: an image id
                std::vector<uint8_t> id;
                a.read_bytes(id);
                if (it.image.empty()) it.image = image_url(id);
            } else if (f == 13 && w == 2) {  // sized pictures { url 2 }: the first one's
                PRd pic;
                a.enter(pic);
                std::string url;
                while (pic.next(f, w))
                    if (f == 2 && w == 2) pic.read_str(url);
                    else pic.skip(w);
                if (!url.empty() && it.image.find("i.scdn.co") == std::string::npos) it.image = url;
            } else {
                a.skip(w);
            }
        }
    };
    while (rd.next(f, w)) {
        if (f == 2 && w == 0) {
            uint64_t v;
            rd.vi(v);
            p.total = (int64_t)v;
        } else if (f == 3 && w == 2) {
            PRd a;
            rd.enter(a);
            Item it;
            attributes(a, it);
            p.name = it.name;
        } else if (f == 5 && w == 2) {
            PRd contents;
            rd.enter(contents);
            while (contents.next(f, w)) {
                if (f == 3 && w == 2) {  // Item { uri 1 }
                    PRd item;
                    contents.enter(item);
                    std::string uri;
                    while (item.next(f, w))
                        if (f == 1 && w == 2) item.read_str(uri);
                        else item.skip(w);
                    p.uris.push_back(uri);
                } else if (f == 4 && w == 2) {  // MetaItem { attributes 2, length 3, owner 5 }
                    PRd meta;
                    contents.enter(meta);
                    Item it;
                    std::string revision;
                    uint64_t v;
                    while (meta.next(f, w)) {
                        if (f == 1 && w == 2) {  // revision: changes whenever the playlist does
                            std::vector<uint8_t> rev;
                            meta.read_bytes(rev);
                            revision = hex(rev);
                        } else if (f == 2 && w == 2) {
                            PRd a;
                            meta.enter(a);
                            attributes(a, it);
                        } else if (f == 3 && w == 0 && meta.vi(v)) {
                            it.count = (int)v;
                        } else if (f == 5 && w == 2) {
                            meta.read_str(it.subtitle);
                        } else if (f == 9 && w == 0 && meta.vi(v)) {  // status code: 4xx or 5xx, gone or private
                            if (sint(v) >= 400) it.year = -1;     // (marked; dropped by playlists())
                        } else {
                            meta.skip(w);
                        }
                    }
                    p.meta.push_back(std::move(it));
                    p.revisions.push_back(std::move(revision));
                } else {
                    contents.skip(w);
                }
            }
        } else {
            rd.skip(w);
        }
    }
    return p;
}

std::string id_of(const std::string& uri) { return uri.substr(uri.rfind(':') + 1); }

}  // namespace

std::vector<Listed> playlists_with_revisions(std::string& error) {
    http::Response r = sp_get("/playlist/v2/user/" + util::url_encode(user_name()) +
                                  "/rootlist?decorate=revision,attributes,length,owner,status_code&from=0&length=500",
                              nullptr, error);
    if (!r.ok()) return {};
    ListPage p = parse_list(r.body);
    std::vector<Listed> out;
    const bool paired = p.meta.size() == p.uris.size() && p.revisions.size() == p.uris.size();
    if (!paired) log_message(LOG_WARNING, "Spotify", "Rootlist: %zu items, %zu descriptions", p.uris.size(), p.meta.size());
    for (size_t i = 0; i < p.uris.size(); i++) {
        if (!util::starts_with(p.uris[i], "spotify:playlist:")) continue;  // folders' start and end
        Listed l;
        if (paired) l.playlist = p.meta[i], l.revision = p.revisions[i];
        if (l.playlist.year < 0) continue;  // deleted, or private to someone else: Spotify's apps leave it out too
        l.playlist.uri = p.uris[i];
        // The owner: a username, which is an id more often than a name (the name isn't in the rootlist). Only
        // Spotify's own playlists say whose they are.
        l.playlist.subtitle = l.playlist.subtitle == "spotify" ? "Spotify" : "";
        if (l.playlist.name.empty()) l.playlist.name = tr("Playlist");
        out.push_back(std::move(l));
    }
    return out;
}

static std::vector<Item> playlists_uncached(std::string& error) {
    std::vector<Item> out;
    for (Listed& l : playlists_with_revisions(error)) out.push_back(std::move(l.playlist));
    return out;
}

Parts playlist_parts(const std::string& uri) {
    struct State {
        std::string id;
        size_t from = 0;
        int64_t total = -1;
    };
    auto st = std::make_shared<State>();
    st->id = id_of(uri);
    return [st](std::string& error) {
        Part part;
        if (st->from >= MAX_ITEMS || (st->total >= 0 && (int64_t)st->from >= st->total)) return part;
        http::Response r = sp_get("/playlist/v2/playlist/" + st->id + "?decorate=revision,attributes,length&from=" +
                                      std::to_string(st->from) + "&length=100",
                                  nullptr, error);
        if (!r.ok()) return part;
        ListPage p = parse_list(r.body);
        st->total = p.total;
        st->from += p.uris.size();
        part.items = hydrate(p.uris, error);
        part.more = !p.uris.empty() && (int64_t)st->from < p.total && st->from < MAX_ITEMS;
        return part;
    };
}

Parts liked_parts() {
    struct State {
        bool listed = false;
        std::vector<std::string> uris;
        size_t at = 0;
    };
    auto st = std::make_shared<State>();
    return [st](std::string& error) {
        Part part;
        if (!st->listed) {
            st->uris = only(collection("collection", error), "spotify:track:");
            if (st->uris.size() > MAX_ITEMS) st->uris.resize(MAX_ITEMS);
            st->listed = true;
        }
        const size_t end = std::min(st->uris.size(), st->at + 200);
        part.items = hydrate(std::vector<std::string>(st->uris.begin() + st->at, st->uris.begin() + end), error);
        st->at = end;
        part.more = st->at < st->uris.size();
        return part;
    };
}

static std::vector<Item> playlist_items_uncached(const std::string& uri, std::string& error) {
    std::vector<Item> out;
    Parts next = playlist_parts(uri);
    for (;;) {
        Part part = next(error);
        for (Item& it : part.items) out.push_back(std::move(it));
        if (!part.more) break;
    }
    return out;
}

std::string playlist_cover(const std::string& uri) {
    std::string error;
    http::Response r = sp_get("/playlist/v2/playlist/" + id_of(uri) + "?decorate=revision&from=0&length=12", nullptr, error);
    if (!r.ok()) return "";
    const std::vector<Item> items = hydrate(parse_list(r.body).uris, error);
    const std::string prefix = "https://i.scdn.co/image/";
    std::vector<std::string> covers;  // image ids (40 hex characters), each album once
    for (const Item& it : items) {
        if (!util::starts_with(it.image, prefix)) continue;
        const std::string id = it.image.substr(prefix.size());
        if (id.size() == 40 && std::find(covers.begin(), covers.end(), id) == covers.end()) covers.push_back(id);
        if (covers.size() == 4) break;
    }
    if (covers.empty()) return "";
    if (covers.size() < 4) return prefix + covers[0];
    return "https://mosaic.scdn.co/300/" + covers[0] + covers[1] + covers[2] + covers[3];
}

static std::vector<Item> liked_songs_uncached(std::string& error) {
    std::vector<std::string> uris = only(collection("collection", error), "spotify:track:");
    if (uris.size() > MAX_ITEMS) uris.resize(MAX_ITEMS);
    return hydrate(uris, error);
}

static std::vector<Item> saved_albums_uncached(std::string& error) {
    return hydrate_albums(only(collection("collection", error), "spotify:album:"), error);
}

static std::vector<Item> saved_episodes_uncached(std::string& error) {
    return hydrate(only(collection("listenlater", error), "spotify:episode:"), error);
}

static std::vector<Item> followed_shows_uncached(std::string& error) {
    const std::vector<std::string> uris = only(collection("show", error), "spotify:show:");
    const auto m = fetch_metadata(uris, SHOW_V4, error);
    std::vector<Item> out;
    for (const std::string& u : uris)
        if (auto it = m.find(u); it != m.end()) out.push_back(parse_show(u, it->second));
    log_message(LOG_OK, "Spotify", "Podcasts: %zu followed, %zu with details", uris.size(), out.size());
    if (!out.empty()) error.clear();
    return out;
}

bool find_feed(const Item& show, podcasts::Show& out, bool* searched) {
    auto norm = [](const std::string& s) {
        std::string n;
        for (unsigned char c : s)
            if (std::isalnum(c) || c >= 0x80) n += (char)std::tolower(c);
        return n;
    };
    const std::string name = norm(show.name), by = norm(show.subtitle);
    if (name.empty()) return false;
    const podcasts::Shows found = podcasts::search(show.name);
    if (searched) *searched = found.ok;
    for (const podcasts::Show& s : found.items) {
        const std::string t = norm(s.title), a = norm(s.author);
        const bool same_name = t == name;
        const bool same_by = !by.empty() && !a.empty() && (a.find(by) != std::string::npos || by.find(a) != std::string::npos);
        const bool close_name = t.find(name) != std::string::npos || name.find(t) != std::string::npos;
        if ((same_name && (by.empty() || same_by || found.items.size() == 1)) || (close_name && same_by)) {
            out = s;
            return true;
        }
    }
    return false;
}

Item latest_episode(const std::string& show_uri) {
    Item out;
    http::Response r = http::get("https://open.spotify.com/embed/show/" + id_of(show_uri), {}, 15);
    if (!r.ok()) return out;
    const std::string open = "<script id=\"__NEXT_DATA__\" type=\"application/json\">";
    const size_t a = r.body.find(open);
    const size_t b = a == std::string::npos ? a : r.body.find("</script>", a);
    if (b == std::string::npos) return out;
    json::Doc d = json::Doc::parse(r.body.substr(a + open.size(), b - a - open.size()));
    json_t* e = json::at(d.get(), {"props", "pageProps", "state", "data", "entity"});
    if (json::str(e, {"type"}) != "episode" || !json::boolean(e, {"isPlayable"}, true)) return out;
    out.uri = json::str(e, {"uri"});
    out.name = json::str(e, {"title"}, json::str(e, {"name"}));
    out.subtitle = json::str(e, {"subtitle"});
    out.duration_ms = std::atoll(json::str(e, {"duration"}).c_str());
    if (!json::str(e, {"duration"}).size()) out.duration_ms = json::num(e, {"duration"});
    json_t* covers = json::at(e, {"relatedEntityCoverArt"});
    for (size_t i = 0; i < json::size(covers); i++) {
        const std::string url = json::str(json_array_get(covers, i), {"url"});
        if (!url.empty() && (out.image.empty() || json::num(json_array_get(covers, i), {"maxWidth"}) == 300)) out.image = url;
    }
    if (!util::starts_with(out.uri, "spotify:episode:")) out = Item();
    return out;
}

static std::vector<Item> followed_artists_uncached(std::string& error) {
    const std::vector<std::string> uris = only(collection("artist", error), "spotify:artist:");
    const auto m = fetch_metadata(uris, ARTIST_V4, error);
    std::vector<Item> out;
    for (const std::string& u : uris) {
        auto it = m.find(u);
        if (it == m.end()) continue;
        Item a;
        a.uri = u;
        PRd rd = reader(it->second);
        uint32_t f;
        uint8_t w;
        while (rd.next(f, w)) {
            if (f == 2 && w == 2) {
                rd.read_str(a.name);
            } else if (f == 17 && w == 2) {
                PRd g;
                rd.enter(g);
                a.image = pick_image(g);
            } else {
                rd.skip(w);
            }
        }
        out.push_back(std::move(a));
    }
    if (!out.empty()) error.clear();
    return out;
}

bool album(const std::string& uri, Album& out, std::string& error) {
    const auto m = fetch_metadata({uri}, ALBUM_V4, error);
    auto it = m.find(uri);
    if (it == m.end()) {
        if (error.empty()) error = tr("Spotify doesn't know this album");
        return false;
    }
    std::vector<std::vector<uint8_t>> gids;
    out.info = parse_album(uri, it->second, &gids);
    std::vector<std::string> uris;
    for (const auto& g : gids) uris.push_back("spotify:track:" + gid_to_id(g));
    out.tracks = hydrate(uris, error);
    for (Item& t : out.tracks)
        if (t.image.empty()) t.image = out.info.image;
    return true;
}

bool artist(const std::string& uri, Artist& out, std::string& error) {
    const auto m = fetch_metadata({uri}, ARTIST_V4, error);
    auto it = m.find(uri);
    if (it == m.end()) {
        if (error.empty()) error = tr("Spotify doesn't know this artist");
        return false;
    }
    out.info.uri = uri;
    std::vector<std::string> top, albums, singles;
    std::string top_country;
    PRd rd = reader(it->second);
    uint32_t f;
    uint8_t w;
    auto groups = [](PRd g, std::vector<std::string>& into) {  // AlbumGroup { album 1 { gid 1 } }
        uint32_t f;
        uint8_t w;
        while (g.next(f, w)) {
            if (f != 1 || w != 2) { g.skip(w); continue; }
            PRd a;
            g.enter(a);
            std::vector<uint8_t> gid = gid_of(a);
            if (gid.size() == 16) into.push_back("spotify:album:" + gid_to_id(gid));
        }
    };
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(out.info.name);
        } else if (f == 17 && w == 2) {
            PRd g;
            rd.enter(g);
            out.info.image = pick_image(g);
        } else if (f == 4 && w == 2 && top.empty()) {  // TopTracks { country 1, track 2 }: the first country's
            PRd t;
            rd.enter(t);
            while (t.next(f, w)) {
                if (f == 1 && w == 2) {
                    t.read_str(top_country);
                } else if (f == 2 && w == 2) {
                    PRd track;
                    t.enter(track);
                    std::vector<uint8_t> gid = gid_of(track);
                    if (gid.size() == 16) top.push_back("spotify:track:" + gid_to_id(gid));
                } else {
                    t.skip(w);
                }
            }
        } else if ((f == 5 || f == 6) && w == 2) {
            PRd g;
            rd.enter(g);
            groups(g, f == 5 ? albums : singles);
        } else {
            rd.skip(w);
        }
    }
    if (top.size() > 10) top.resize(10);
    if (albums.size() > 50) albums.resize(50);
    if (singles.size() > 50) singles.resize(50);
    out.top_tracks = hydrate(top, error);
    out.albums = hydrate_albums(albums, error);
    out.singles = hydrate_albums(singles, error);
    error.clear();
    return true;
}


// --- what was loaded, for a while (going back and forth, and the search) ---------------------------

namespace {
constexpr double KEEP = 600;     // seconds
constexpr size_t KEEP_LISTS = 24;
struct Kept {
    double at = 0;
    std::vector<Item> items;
};
std::mutex g_kept_mu;
std::map<std::string, Kept> g_kept;

template <typename Load>
std::vector<Item> kept(const std::string& key, std::string& error, Load load) {
    {
        std::lock_guard<std::mutex> lk(g_kept_mu);
        auto it = g_kept.find(key);
        if (it != g_kept.end() && util::now_seconds() - it->second.at < KEEP) return it->second.items;
    }
    std::vector<Item> items = load();
    if (items.empty()) return items;  // a failure (or nothing): asked again next time
    std::lock_guard<std::mutex> lk(g_kept_mu);
    if (g_kept.size() >= KEEP_LISTS) {  // the oldest goes
        auto oldest = g_kept.begin();
        for (auto it = g_kept.begin(); it != g_kept.end(); ++it)
            if (it->second.at < oldest->second.at) oldest = it;
        g_kept.erase(oldest);
    }
    g_kept[key] = Kept{util::now_seconds(), items};
    return items;
}
}  // namespace

namespace detail {
void forget_library() {
    std::lock_guard<std::mutex> lk(g_kept_mu);
    g_kept.clear();
}
}  // namespace detail

std::vector<Item> playlists(std::string& error) {
    return kept("playlists", error, [&] { return playlists_uncached(error); });
}
std::vector<Item> playlist_items(const std::string& uri, std::string& error) {
    return kept("playlist:" + uri, error, [&] { return playlist_items_uncached(uri, error); });
}
std::vector<Item> liked_songs(std::string& error) {
    return kept("liked", error, [&] { return liked_songs_uncached(error); });
}
std::vector<Item> saved_albums(std::string& error) {
    return kept("albums", error, [&] { return saved_albums_uncached(error); });
}
std::vector<Item> saved_episodes(std::string& error) {
    return kept("episodes", error, [&] { return saved_episodes_uncached(error); });
}
std::vector<Item> followed_shows(std::string& error) {
    return kept("shows", error, [&] { return followed_shows_uncached(error); });
}
std::vector<Item> followed_artists(std::string& error) {
    return kept("artists", error, [&] { return followed_artists_uncached(error); });
}

// --- searching the library -----------------------------------------------------------------------------

namespace {

// Lower case for comparing: ASCII, and the accented Latin letters (two bytes in UTF-8, the capital's second byte
// 0x80-0x9E going up by 0x20: Ä Ö Ü É...).
std::string fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9E &&
            (unsigned char)s[i + 1] != 0x97) {  // not ×
            out += (char)c;
            out += (char)((unsigned char)s[++i] + 0x20);
        } else {
            out += (char)std::tolower(c);
        }
    }
    return out;
}

std::vector<std::string> words(const std::string& q) {
    std::vector<std::string> out;
    std::string w;
    for (char c : fold(q) + " ") {
        if (c == ' ' || c == '\t') {
            if (!w.empty()) out.push_back(w), w.clear();
        } else {
            w += c;
        }
    }
    return out;
}

std::vector<Item> matching(const std::vector<Item>& items, const std::vector<std::string>& ws, size_t max) {
    std::vector<Item> out;
    for (const Item& it : items) {
        const std::string text = fold(it.name + " " + it.subtitle);
        bool all = true;
        for (const std::string& w : ws)
            if (text.find(w) == std::string::npos) {
                all = false;
                break;
            }
        if (all) out.push_back(it);
        if (out.size() >= max) break;
    }
    return out;
}

}  // namespace

Found search_library(const std::string& query, std::string& error) {
    Found f;
    const std::vector<std::string> ws = words(query);
    if (ws.empty()) return f;
    std::string e;  // a list that fails to load leaves its part empty; the others still count
    f.songs = matching(liked_songs(e), ws, 30);  // songs as rows: a few dozen, the shelves below stay in reach
    f.episodes = matching(saved_episodes(e), ws, 30);
    f.albums = matching(saved_albums(e), ws, 50);
    f.artists = matching(followed_artists(e), ws, 50);
    f.playlists = matching(playlists(e), ws, 50);
    f.podcasts = matching(followed_shows(e), ws, 50);
    std::set<std::string> liked;
    for (const Item& s : f.songs) liked.insert(s.uri);
    for (Item& s : index::search(ws, 60, fold))
        if (!liked.count(s.uri) && f.in_playlists.size() < 30) f.in_playlists.push_back(std::move(s));
    if (f.empty()) error = e;
    return f;
}

}  // namespace spotify
