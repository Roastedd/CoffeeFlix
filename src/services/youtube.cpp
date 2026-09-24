#include "services/youtube.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <mutex>
#include <set>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"
#include "services/hls.hpp"
#include "services/yt_recs.hpp"

namespace youtube {

const char* PARAMS_VIDEOS = "EgIQAQ%3D%3D";          // type: video
const char* PARAMS_POPULAR_WEEK = "CAMSBAgDEAE%3D";  // sort: views, upload: this week, type: video

namespace {

std::string api_base() {
    static const std::string base = util::env_or("COFFEEFLIX_YT_API", "https://www.youtube.com/youtubei/v1/");
    return base;
}

struct Client {
    const char* name;
    int id;  // X-YouTube-Client-Name
    const char* version;
    const char* user_agent;
    const char* device_make;
    const char* device_model;
    const char* os_name;
    const char* os_version;
    int android_sdk;
};

const Client WEB{"WEB", 1, "2.20250922.01.00",
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
                 "", "", "Windows", "10.0", 0};
// Playback clients. VISIONOS needs neither a PO token nor the JS player (yt-dlp's default
// without a JS runtime as of 2026.08, and Flow's primary client). ANDROID_VR's file URLs now
// stop after about a minute without a PO token, so it's only used for live HLS; IOS returns
// SABR-only or 403ing URLs and is gone.
const Client VISIONOS{"VISIONOS", 101, "1.02",
                      "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
                      "Apple", "RealityDevice17,1", "visionOS", "26.5.23O471", 0};
const Client ANDROID_VR{"ANDROID_VR", 28, "1.65.10",
                        "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
                        "Oculus", "Quest 3", "Android", "12L", 32};

std::mutex g_visitor_m;
std::string g_visitor;

std::string visitor() {
    std::lock_guard<std::mutex> lk(g_visitor_m);
    if (g_visitor.empty()) g_visitor = store::get_str("yt_visitor", "");
    return g_visitor;
}

void remember_visitor(json_t* root) {
    std::string v = json::str(root, {"responseContext", "visitorData"});
    if (v.empty()) return;
    std::lock_guard<std::mutex> lk(g_visitor_m);
    if (v != g_visitor) {
        g_visitor = v;
        store::set_str("yt_visitor", v);
    }
}

json_t* context(const Client& c) {
    json_t* client = json_object();
    json_object_set_new(client, "clientName", json_string(c.name));
    json_object_set_new(client, "clientVersion", json_string(c.version));
    json_object_set_new(client, "hl", json_string("en"));
    json_object_set_new(client, "gl", json_string(store::get_str("yt_region", "US").c_str()));
    if (*c.device_make) json_object_set_new(client, "deviceMake", json_string(c.device_make));
    if (*c.device_model) json_object_set_new(client, "deviceModel", json_string(c.device_model));
    if (*c.os_name) json_object_set_new(client, "osName", json_string(c.os_name));
    if (*c.os_version) json_object_set_new(client, "osVersion", json_string(c.os_version));
    if (c.android_sdk) json_object_set_new(client, "androidSdkVersion", json_integer(c.android_sdk));
    // The app clients repeat their user agent in the context, like the real apps.
    if (&c != &WEB) json_object_set_new(client, "userAgent", json_string(c.user_agent));
    std::string vd = visitor();
    if (!vd.empty()) json_object_set_new(client, "visitorData", json_string(vd.c_str()));
    json_t* ctx = json_object();
    json_object_set_new(ctx, "client", client);
    return ctx;
}

json::Doc call(const char* endpoint, const Client& c, json_t* body, std::string& error) {
    json_object_set_new(body, "context", context(c));
    std::string payload = json::dump(body);
    json_decref(body);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent", c.user_agent},
        {"X-YouTube-Client-Name", std::to_string(c.id)},
        {"X-YouTube-Client-Version", c.version},
        {"Origin", "https://www.youtube.com"},
    };
    std::string vd = visitor();
    if (!vd.empty()) headers.emplace_back("X-Goog-Visitor-Id", vd);
    http::Response r = http::post_json(api_base() + endpoint + "?prettyPrint=false", payload, headers, 20);
    if (!r.ok()) {
        error = r.error.empty() ? "YouTube request failed" : r.error;
        return json::Doc();
    }
    double t0 = util::now_seconds();
    json::Doc doc = json::Doc::parse(r.body);
    double took = util::now_seconds() - t0;
    if (took > 1.0 || getenv("COFFEEFLIX_HTTP_TRACE"))
        log_message(took > 1.0 ? LOG_WARNING : LOG_DEBUG, "YouTube", "%s: %zu KB of JSON parsed in %.2f s", endpoint,
                    r.body.size() / 1024, took);
    if (!doc) error = "Unexpected response from YouTube";
    else remember_visitor(doc.get());
    return doc;
}

std::string first_text(json_t* r, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string s = json::yt_text(json_object_get(r, k));
        if (!s.empty()) return s;
    }
    return "";
}

std::string browse_id(json_t* r) {
    for (const char* k : {"ownerText", "longBylineText", "shortBylineText"}) {
        std::string id = json::str(r, {k, "runs", 0, "navigationEndpoint", "browseEndpoint", "browseId"});
        if (!id.empty()) return id;
    }
    return "";
}

bool parse_renderer(json_t* r, Video& v) {
    v.id = json::str(r, {"videoId"});
    if (v.id.size() != 11) return false;
    v.title = first_text(r, {"title", "headline"});
    v.channel = first_text(r, {"ownerText", "longBylineText", "shortBylineText"});
    v.channel_id = browse_id(r);
    v.duration = first_text(r, {"lengthText"});
    v.views = first_text(r, {"shortViewCountText", "viewCountText"});
    v.published = first_text(r, {"publishedTimeText"});
    json_t* overlays = json_object_get(r, "thumbnailOverlays");
    for (size_t i = 0; i < json::size(overlays); i++) {
        json_t* ts = json::at(json_array_get(overlays, i), {"thumbnailOverlayTimeStatusRenderer"});
        if (!ts) continue;
        if (v.duration.empty()) v.duration = json::yt_text(json_object_get(ts, "text"));
        if (json::str(ts, {"style"}) == "LIVE") v.live = true;
    }
    json_t* badges = json_object_get(r, "badges");
    for (size_t i = 0; i < json::size(badges); i++)
        if (json::str(json_array_get(badges, i), {"metadataBadgeRenderer", "style"}) == "BADGE_STYLE_TYPE_LIVE_NOW") v.live = true;
    return !v.title.empty();
}

// Newer "lockupViewModel" layout.
bool parse_lockup(json_t* l, Video& v) {
    std::string type = json::str(l, {"contentType"});
    if (!type.empty() && type != "LOCKUP_CONTENT_TYPE_VIDEO") return false;
    v.id = json::str(l, {"contentId"});
    if (v.id.size() != 11) return false;
    json_t* meta = json::at(l, {"metadata", "lockupMetadataViewModel"});
    v.title = json::str(meta, {"title", "content"});
    json_t* rows = json::at(meta, {"metadata", "contentMetadataViewModel", "metadataRows"});
    std::vector<std::string> parts;
    for (size_t i = 0; i < json::size(rows); i++) {
        json_t* mp = json::at(json_array_get(rows, i), {"metadataParts"});
        for (size_t k = 0; k < json::size(mp); k++) parts.push_back(json::str(json_array_get(mp, k), {"text", "content"}));
    }
    // Usually [channel] [views, age], but channel pages leave the channel out: sort the parts by
    // what they say (the requests ask for English).
    for (const std::string& p : parts) {
        bool views = p.find(" view") != std::string::npos || p.find(" watching") != std::string::npos ||
                     p == "No views";
        bool age = p.find(" ago") != std::string::npos || util::starts_with(p, "Streamed") ||
                   util::starts_with(p, "Premiere") || util::starts_with(p, "Scheduled");
        if (views && v.views.empty()) v.views = p;
        else if (age && v.published.empty()) v.published = p;
        else if (!views && !age && v.channel.empty()) v.channel = p;
    }
    v.channel_id = json::str(json::find_key(meta, "browseEndpoint", 12), {"browseId"});
    json::for_each_key(json::at(l, {"contentImage"}), "thumbnailBadgeViewModel", [&](json_t* b) {
        std::string t = json::str(b, {"text"});
        if (t == "LIVE" || json::str(b, {"badgeStyle"}) == "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE") v.live = true;
        else if (v.duration.empty() && !t.empty()) v.duration = t;
    });
    return !v.title.empty();
}

// Shorts shelf items ("shortsLockupViewModel", older "reelItemRenderer").
bool parse_short(json_t* l, Video& v) {
    v.id = json::str(l, {"onTap", "innertubeCommand", "reelWatchEndpoint", "videoId"});
    if (v.id.empty()) v.id = json::str(l, {"videoId"});
    if (v.id.size() != 11) return false;
    v.title = json::str(l, {"overlayMetadata", "primaryText", "content"});
    if (v.title.empty()) v.title = json::yt_text(json_object_get(l, "headline"));
    v.views = json::str(l, {"overlayMetadata", "secondaryText", "content"});
    if (v.views.empty()) v.views = json::yt_text(json_object_get(l, "viewCountText"));
    v.duration = "Short";
    return !v.title.empty();
}

// A page can carry several continuations (hidden shelves, other tabs); the one for the main
// list ends the biggest array.
void find_continuation(json_t* j, int depth, size_t& best_n, std::string& best) {
    if (!j || depth > 30) return;
    if (json_is_array(j)) {
        size_t n = json_array_size(j);
        std::string t = json::str(json::at(j, {-1}), {"continuationItemRenderer", "continuationEndpoint", "continuationCommand", "token"});
        if (!t.empty() && n > best_n) {
            best_n = n;
            best = t;
        }
        for (size_t i = 0; i < n; i++) find_continuation(json_array_get(j, i), depth + 1, best_n, best);
    } else if (json_is_object(j)) {
        const char* k;
        json_t* v;
        json_object_foreach(j, k, v) find_continuation(v, depth + 1, best_n, best);
    }
}

std::string main_continuation(json_t* root) {
    size_t n = 0;
    std::string token;
    find_continuation(root, 0, n, token);
    return token;
}

// Shorts are only picked up where they're asked for (the channel's Shorts tab), not from the
// shelves search and the watch page sprinkle in.
Results parse_results(json_t* root, bool shorts = false) {
    Results res;
    std::set<std::string> seen;
    auto add = [&](Video& v) {
        if (seen.insert(v.id).second) res.items.push_back(std::move(v));
    };
    for (const char* key : {"videoRenderer", "compactVideoRenderer", "videoWithContextRenderer", "gridVideoRenderer"}) {
        json::for_each_key(root, key, [&](json_t* r) {
            Video v;
            if (parse_renderer(r, v)) add(v);
        });
    }
    json::for_each_key(root, "lockupViewModel", [&](json_t* l) {
        Video v;
        if (parse_lockup(l, v)) add(v);
    });
    for (const char* key : {"shortsLockupViewModel", "reelItemRenderer"}) {
        if (!shorts) break;
        json::for_each_key(root, key, [&](json_t* l) {
            Video v;
            if (parse_short(l, v)) add(v);
        });
    }
    res.continuation = main_continuation(root);
    res.ok = true;
    return res;
}

std::string https(std::string url) {
    if (util::starts_with(url, "//")) url = "https:" + url;
    return url;
}

// Largest image of a {"sources": [...]} or {"thumbnails": [...]} list.
std::string best_image(json_t* img) {
    json_t* list = json_object_get(img, "sources");
    if (!list) list = json_object_get(img, "thumbnails");
    return https(json::str(json::at(list, {-1}), {"url"}));
}

std::vector<std::string> metadata_parts(json_t* rows) {
    std::vector<std::string> out;
    for (size_t i = 0; i < json::size(rows); i++) {
        json_t* mp = json::at(json_array_get(rows, i), {"metadataParts"});
        for (size_t k = 0; k < json::size(mp); k++) {
            std::string t = json::str(json_array_get(mp, k), {"text", "content"});
            if (t.empty()) t = json::str(json_array_get(mp, k), {"avatarStack", "avatarStackViewModel", "text", "content"});
            if (!t.empty()) out.push_back(t);
        }
    }
    return out;
}

void parse_channel_header(json_t* root, Channel& c) {
    json_t* meta = json::at(root, {"metadata", "channelMetadataRenderer"});
    c.id = json::str(meta, {"externalId"});
    c.name = json::str(meta, {"title"});
    c.description = json::str(meta, {"description"});
    c.avatar = best_image(json::at(meta, {"avatar"}));

    json_t* h = json::at(root, {"header", "pageHeaderRenderer", "content", "pageHeaderViewModel"});
    if (h) {
        std::string name = json::str(h, {"title", "dynamicTextViewModel", "text", "content"});
        if (!name.empty()) c.name = name;
        std::string av = best_image(json::at(h, {"image", "decoratedAvatarViewModel", "avatar", "avatarViewModel", "image"}));
        if (!av.empty()) c.avatar = av;
        c.banner = best_image(json::at(h, {"banner", "imageBannerViewModel", "image"}));
        for (const std::string& p : metadata_parts(json::at(h, {"metadata", "contentMetadataViewModel", "metadataRows"}))) {
            if (p[0] == '@') c.handle = p;
            else if (p.find("subscriber") != std::string::npos) c.subscribers = p;
            else if (p.find("video") != std::string::npos) c.videos = p;
        }
        std::string d = json::str(h, {"description", "descriptionPreviewViewModel", "description", "content"});
        if (c.description.empty()) c.description = d;
    } else if (json_t* old = json::at(root, {"header", "c4TabbedHeaderRenderer"})) {
        if (c.name.empty()) c.name = json::str(old, {"title"});
        c.subscribers = json::yt_text(json_object_get(old, "subscriberCountText"));
        c.handle = json::yt_text(json_object_get(old, "channelHandleText"));
        c.videos = json::yt_text(json_object_get(old, "videosCountText"));
        if (c.avatar.empty()) c.avatar = best_image(json::at(old, {"avatar"}));
        c.banner = best_image(json::at(old, {"banner"}));
    }
}

bool parse_playlist_lockup(json_t* l, Playlist& p) {
    if (json::str(l, {"contentType"}) != "LOCKUP_CONTENT_TYPE_PLAYLIST") return false;
    p.id = json::str(l, {"contentId"});
    json_t* meta = json::at(l, {"metadata", "lockupMetadataViewModel"});
    p.title = json::str(meta, {"title", "content"});
    json_t* thumb = json::at(l, {"contentImage", "collectionThumbnailViewModel", "primaryThumbnail", "thumbnailViewModel"});
    p.thumbnail = best_image(json::at(thumb, {"image"}));
    json::for_each_key(thumb, "thumbnailBadgeViewModel", [&](json_t* b) {
        if (p.count.empty()) p.count = json::str(b, {"text"});
    });
    for (const std::string& part : metadata_parts(json::at(meta, {"metadata", "contentMetadataViewModel", "metadataRows"})))
        if (p.channel.empty() && part != "View full playlist" && part != "Playlist" && part.find("Updated") == std::string::npos)
            p.channel = part;
    return !p.id.empty() && !p.title.empty();
}

bool parse_playlist_renderer(json_t* r, Playlist& p) {
    p.id = json::str(r, {"playlistId"});
    p.title = first_text(r, {"title"});
    p.count = first_text(r, {"videoCountShortText", "videoCountText"});
    p.channel = first_text(r, {"shortBylineText", "longBylineText"});
    p.thumbnail = best_image(json::at(r, {"thumbnail"}));
    if (p.thumbnail.empty()) p.thumbnail = best_image(json::at(r, {"thumbnails", 0}));
    return !p.id.empty() && !p.title.empty();
}

Results fail(const std::string& error) {
    Results r;
    r.error = error;
    return r;
}

json::Doc browse(const std::string& browse_id, const char* params, const std::string& continuation, std::string& err) {
    json_t* body = json_object();
    if (!continuation.empty()) {
        json_object_set_new(body, "continuation", json_string(continuation.c_str()));
    } else {
        json_object_set_new(body, "browseId", json_string(browse_id.c_str()));
        if (params) json_object_set_new(body, "params", json_string(params));
    }
    return call("browse", WEB, body, err);
}

// Channel page tabs (the "params" the web app sends when you click them).
const char* const TAB_VIDEOS = "EgZ2aWRlb3PyBgQKAjoA";
const char* const TAB_SHORTS = "EgZzaG9ydHPyBgUKA5oBAA==";
const char* const TAB_LIVE = "EgdzdHJlYW1z8gYECgJ6AA==";
const char* const TAB_PLAYLISTS = "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA";
const char* const PARAMS_CHANNELS = "EgIQAg==";

// --- playback ------------------------------------------------------------------

struct Format {
    std::string url, mime;
    int itag = 0, width = 0, height = 0, bitrate = 0, fps = 0;
    // Audio: videos with dubs have one set of formats per language.
    std::string audio_id, audio_label;  // "en-US.4", "English (US) original"
    bool audio_original = false, audio_dubbed = false, drc = false;
};

// Decodes URL-safe base64 (padding optional); stops at the first character outside it.
std::string base64url_decode(const std::string& in) {
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char ch : in) {
        int v = ch >= 'A' && ch <= 'Z'   ? ch - 'A'
                : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
                : ch >= '0' && ch <= '9' ? ch - '0' + 52
                : ch == '-' || ch == '+' ? 62
                : ch == '_' || ch == '/' ? 63
                                         : -1;
        if (v < 0) break;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::vector<Format> formats(json_t* arr) {
    std::vector<Format> out;
    for (size_t i = 0; i < json::size(arr); i++) {
        json_t* f = json_array_get(arr, i);
        Format fm;
        fm.url = json::str(f, {"url"});
        if (fm.url.empty()) continue;  // signatureCipher needs the JS player
        fm.mime = json::str(f, {"mimeType"});
        fm.itag = (int)json::num(f, {"itag"});
        fm.width = (int)json::num(f, {"width"});
        fm.height = (int)json::num(f, {"height"});
        fm.bitrate = (int)json::num(f, {"bitrate"});
        fm.fps = (int)json::num(f, {"fps"});
        json_t* track = json_object_get(f, "audioTrack");
        fm.audio_id = json::str(track, {"id"});
        fm.audio_label = json::str(track, {"displayName"});
        fm.audio_original = json::boolean(track, {"audioIsDefault"});
        // xtags is a small protobuf naming the kind of track: "acont" = "original" / "dubbed-auto".
        fm.audio_dubbed = base64url_decode(json::str(f, {"xtags"})).find("dubbed-auto") != std::string::npos;
        fm.drc = json::boolean(f, {"isDrc"});
        out.push_back(fm);
    }
    return out;
}

// AAC audio in the language asked for (src.audio_language, "" = the original), or the original
// when that isn't there. Videos with dubs have a full set of formats per language, and the
// original isn't the one with the highest bitrate. Fills src.audio_languages when there is a choice.
const Format* pick_audio(const std::vector<Format>& formats, player::Source& src) {
    std::vector<const Format*> aac;
    for (const Format& f : formats)
        if (util::starts_with(f.mime, "audio/mp4")) aac.push_back(&f);
    auto best = [&](auto match) {
        const Format* b = nullptr;
        for (const Format* f : aac) {
            if (!match(*f)) continue;
            // Full dynamic range over the "stable volume" (DRC) copy, then the higher bitrate.
            if (!b || (b->drc && !f->drc) || (b->drc == f->drc && f->bitrate > b->bitrate)) b = f;
        }
        return b;
    };
    const Format* a = nullptr;
    if (!src.audio_language.empty()) a = best([&](const Format& f) { return f.audio_id == src.audio_language; });
    if (!a) a = best([](const Format& f) { return f.audio_original || f.audio_id.empty(); });
    if (!a) a = best([](const Format& f) { return !f.audio_dubbed; });
    if (!a) a = best([](const Format&) { return true; });

    src.audio_languages.clear();
    for (const Format* f : aac) {
        if (f->audio_id.empty()) continue;
        bool seen = false;
        for (auto& l : src.audio_languages) seen = seen || l.first == f->audio_id;
        if (seen) continue;
        std::string label = f->audio_label.empty() ? f->audio_id : f->audio_label;
        if (f->audio_dubbed) label += " (auto-dubbed)";
        src.audio_languages.emplace_back(f->audio_id, label);
    }
    // The original first, the rest by name.
    std::string original = a && a->audio_original ? a->audio_id : "";
    for (const Format* f : aac)
        if (f->audio_original) original = f->audio_id;
    std::sort(src.audio_languages.begin(), src.audio_languages.end(), [&](const auto& x, const auto& y) {
        if ((x.first == original) != (y.first == original)) return x.first == original;
        return x.second < y.second;
    });
    if (src.audio_languages.size() < 2) src.audio_languages.clear();
    if (a) src.audio_language = a->audio_id;
    return a;
}

bool from_hls(const std::string& manifest, int max_height, const Client& c, player::Source& src, std::string& error) {
    http::Response r = http::get(manifest, {{"User-Agent", c.user_agent}}, 15);
    if (!r.ok()) {
        error = "Couldn't load the stream playlist";
        return false;
    }
    hls::Master m = hls::parse(r.body, manifest);
    if (!m.is_master) {
        src.url = manifest;
        return true;
    }
    const hls::Variant* v = hls::pick(m, max_height);
    if (!v) {
        error = "No compatible stream (H.264) found";
        return false;
    }
    src.url = v->url;
    if (const hls::Rendition* a = hls::audio_for(m, *v)) src.audio_url = a->url;
    return true;
}

// Caption tracks as WebVTT subtitles: your language first, uploaded before auto-generated.
void add_captions(json_t* tracks, player::Source& src) {
    struct Track {
        int rank;
        std::string label, url;
    };
    std::string lang = store::get_str("yt_caption_lang", "en");
    std::vector<Track> list;
    for (size_t i = 0; i < json::size(tracks); i++) {
        json_t* t = json_array_get(tracks, i);
        std::string url = json::str(t, {"baseUrl"});
        if (url.empty()) continue;
        if (util::starts_with(url, "/")) url = "https://www.youtube.com" + url;
        size_t f = url.find("&fmt=");
        if (f != std::string::npos) {
            size_t e = url.find('&', f + 1);
            url.erase(f, e == std::string::npos ? std::string::npos : e - f);
        }
        url += "&fmt=vtt";
        std::string code = json::str(t, {"languageCode"});
        bool asr = json::str(t, {"kind"}) == "asr";
        bool mine = code == lang || util::starts_with(code, lang + "-");
        std::string label = json::yt_text(json_object_get(t, "name"));
        if (label.empty()) label = code;
        list.push_back({(mine ? 0 : 2) + (asr ? 1 : 0), label, url});
    }
    std::stable_sort(list.begin(), list.end(), [](const Track& a, const Track& b) { return a.rank < b.rank; });
    src.external_subs.clear();
    for (size_t i = 0; i < list.size() && i < 12; i++) src.external_subs.emplace_back(list[i].label, list[i].url);
}

bool try_client(const Client& c, const std::string& id, int max_height, player::Source& src, std::string& error) {
    json_t* body = json_object();
    json_object_set_new(body, "videoId", json_string(id.c_str()));
    json_object_set_new(body, "contentCheckOk", json_true());
    json_object_set_new(body, "racyCheckOk", json_true());
    json::Doc doc = call("player", c, body, error);
    if (!doc) return false;
    json_t* root = doc.get();

    std::string status = json::str(root, {"playabilityStatus", "status"});
    if (status != "OK") {
        error = json::str(root, {"playabilityStatus", "reason"});
        if (error.empty()) error = json::yt_text(json::at(root, {"playabilityStatus", "errorScreen", "playerErrorMessageRenderer", "reason"}));
        if (error.empty()) error = "This video can't be played (" + status + ")";
        return false;
    }

    json_t* details = json_object_get(root, "videoDetails");
    if (src.title.empty()) src.title = json::str(details, {"title"});
    if (src.subtitle.empty()) src.subtitle = json::str(details, {"author"});
    if (src.channel_id.empty()) src.channel_id = json::str(details, {"channelId"});
    bool live = json::boolean(details, {"isLive"}) ||
                (json::boolean(details, {"isLiveContent"}) && json::num(details, {"lengthSeconds"}) == 0);
    src.live = live;
    src.user_agent = c.user_agent;
    add_captions(json::at(root, {"captions", "playerCaptionsTracklistRenderer", "captionTracks"}), src);

    json_t* sd = json_object_get(root, "streamingData");
    std::string hls_url = json::str(sd, {"hlsManifestUrl"});
    if (live) {
        if (hls_url.empty()) {
            error = "Live stream isn't available";
            return false;
        }
        return from_hls(hls_url, max_height, c, src, error);
    }
    if (&c == &ANDROID_VR) {
        error = "This video can't be played right now";
        return false;
    }

    std::vector<Format> adaptive = formats(json_object_get(sd, "adaptiveFormats"));
    const Format* best_v = nullptr;
    const Format* best_a = nullptr;
    // 60 fps doubles the decoding and drawing work, which the Wii U can't keep up with at
    // 720p and above: only with the "Allow 60 fps" setting, or when there is nothing else.
    int max_fps = store::get_bool("allow_60fps", false) ? 61 : 31;
    for (int pass = 0; pass < 2 && !best_v; pass++) {
        for (const Format& f : adaptive) {
            if (!util::starts_with(f.mime, "video/mp4") || f.mime.find("avc1") == std::string::npos || f.height <= 0 ||
                f.height > max_height || (pass == 0 && f.fps > max_fps))
                continue;
            bool better = !best_v || f.height > best_v->height ||
                          (f.height == best_v->height && f.fps <= 30 && best_v->fps > 30) ||
                          (f.height == best_v->height && f.fps == best_v->fps && f.bitrate > best_v->bitrate);
            if (better) best_v = &f;
        }
    }
    best_a = pick_audio(adaptive, src);
    if (best_v && best_a) {
        src.url = best_v->url;
        src.audio_url = best_a->url;
        log_message(LOG_OK, "YouTube", "%s: itag %d (%dp%d) + itag %d%s%s via %s", id.c_str(), best_v->itag,
                    best_v->height, best_v->fps, best_a->itag, best_a->audio_label.empty() ? "" : ", ",
                    best_a->audio_label.c_str(), c.name);
        return true;
    }
    // Progressive (video+audio in one file) fallback: usually 360p.
    std::vector<Format> muxed = formats(json_object_get(sd, "formats"));
    const Format* best_m = nullptr;
    for (const Format& f : muxed)
        if (f.mime.find("avc1") != std::string::npos && (!best_m || f.height > best_m->height)) best_m = &f;
    if (best_m) {
        src.url = best_m->url;
        log_message(LOG_OK, "YouTube", "%s: progressive itag %d via %s", id.c_str(), best_m->itag, c.name);
        return true;
    }
    if (!hls_url.empty()) return from_hls(hls_url, max_height, c, src, error);
    error = "No playable H.264 streams";
    return false;
}

}  // namespace

const std::vector<Topic>& topics() {
    static const std::vector<Topic> t = {
        {"Music", "music video", ic::MUSIC},
        {"Gaming", "gaming", ic::SPORTS_ESPORTS},
        {"News", "news today", ic::NEWSPAPER},
        {"Sports", "sports highlights", ic::SPORTS_SOCCER},
        {"Science", "science explained", ic::AUTO_AWESOME},
        {"Trailers", "official trailer", ic::MOVIE},
        {"Comedy", "comedy", ic::PEOPLE},
        {"Cooking", "recipe", ic::LOCAL_CAFE},
    };
    return t;
}

Results search(const std::string& query, const std::string& params, const std::string& continuation) {
    Results res;
    json_t* body = json_object();
    if (!continuation.empty()) {
        json_object_set_new(body, "continuation", json_string(continuation.c_str()));
    } else {
        json_object_set_new(body, "query", json_string(query.c_str()));
        if (!params.empty()) json_object_set_new(body, "params", json_string(util::url_decode(params).c_str()));
    }
    std::string err;
    json::Doc doc = call("search", WEB, body, err);
    if (!doc) {
        res.error = err;
        return res;
    }
    return parse_results(doc.get());
}

Results trending() {
    // Six searches: the YouTube page, Home and For you all want this at the start, so it is
    // fetched once (the others wait) and kept for 10 minutes per region.
    static std::mutex m;
    static Results cached;
    static std::string cached_region;
    static double cached_at = -1e9;
    std::lock_guard<std::mutex> lk(m);
    std::string region = store::get_str("yt_region", "US");
    if (region == cached_region && util::now_seconds() - cached_at < 10 * 60 && !cached.items.empty()) return cached;

    // YouTube retired the Trending page (FEtrending now answers 400). Searching for "trending"
    // mostly finds spam tagged #trending, so mix the most-viewed videos of the week from a few
    // broad topics instead.
    static const char* const QUERIES[] = {"music video", "gaming", "news today", "official trailer", "comedy",
                                          "sports highlights"};
    std::vector<std::future<Results>> parts;
    for (const char* q : QUERIES)
        parts.push_back(std::async(std::launch::async, [q] { return search(q, PARAMS_POPULAR_WEEK); }));
    std::vector<Results> got;
    Results out;
    for (auto& p : parts) {
        got.push_back(p.get());
        if (!got.back().ok && out.error.empty()) out.error = got.back().error;
    }
    std::set<std::string> seen;
    for (size_t k = 0; k < 8; k++)
        for (auto& r : got)
            if (k < r.items.size() && seen.insert(r.items[k].id).second) out.items.push_back(r.items[k]);
    out.ok = !out.items.empty() || out.error.empty();
    if (!out.items.empty()) {
        cached = out;
        cached_region = region;
        cached_at = util::now_seconds();
    }
    return out;
}

Results channel_videos(const std::string& channel_id, const std::string& continuation) {
    Channel header;  // for the channel's name, which the videos themselves no longer carry
    return channel_tab(channel_id, Tab::VIDEOS, continuation, continuation.empty() ? &header : nullptr);
}

Results channel_tab(const std::string& channel_id, Tab tab, const std::string& continuation, Channel* header) {
    const char* params = tab == Tab::SHORTS ? TAB_SHORTS : tab == Tab::LIVE ? TAB_LIVE : TAB_VIDEOS;
    std::string err;
    json::Doc doc = browse(channel_id, params, continuation, err);
    if (!doc) return fail(err);
    if (header && continuation.empty()) {
        parse_channel_header(doc.get(), *header);
        if (header->id.empty()) header->id = channel_id;
    }
    Results r = parse_results(doc.get(), tab == Tab::SHORTS);
    // Every video on a channel page is that channel's, but the new layout leaves the name out.
    for (Video& v : r.items) {
        v.channel_id = channel_id;
        if (v.channel.empty() && header) v.channel = header->name;
    }
    return r;
}

PlaylistResults channel_playlists(const std::string& channel_id, const std::string& continuation) {
    PlaylistResults out;
    std::string err;
    json::Doc doc = browse(channel_id, TAB_PLAYLISTS, continuation, err);
    if (!doc) {
        out.error = err;
        return out;
    }
    std::set<std::string> seen;
    json::for_each_key(doc.get(), "lockupViewModel", [&](json_t* l) {
        Playlist p;
        if (parse_playlist_lockup(l, p) && seen.insert(p.id).second) out.items.push_back(p);
    });
    json::for_each_key(doc.get(), "gridPlaylistRenderer", [&](json_t* r) {
        Playlist p;
        if (parse_playlist_renderer(r, p) && seen.insert(p.id).second) out.items.push_back(p);
    });
    out.continuation = main_continuation(doc.get());
    out.ok = true;
    return out;
}

Results playlist_videos(const std::string& playlist_id, const std::string& continuation, Playlist* info) {
    std::string err;
    json::Doc doc = browse("VL" + playlist_id, nullptr, continuation, err);
    if (!doc) return fail(err);
    Results r = parse_results(doc.get());
    if (info && continuation.empty()) {
        info->id = playlist_id;
        json_t* h = json::at(doc.get(), {"header", "pageHeaderRenderer", "content", "pageHeaderViewModel"});
        info->title = json::str(h, {"title", "dynamicTextViewModel", "text", "content"});
        if (info->title.empty()) info->title = json::str(doc.get(), {"metadata", "playlistMetadataRenderer", "title"});
        std::vector<std::string> parts = metadata_parts(json::at(h, {"metadata", "contentMetadataViewModel", "metadataRows"}));
        for (const std::string& p : parts) {
            if (info->channel.empty() && p != "Playlist" && p.find(" view") == std::string::npos &&
                p.find("video") == std::string::npos && p.find("Updated") == std::string::npos)
                info->channel = p;
            if (p.find("video") != std::string::npos) info->count = p;
        }
        if (util::starts_with(info->channel, "by ")) info->channel.erase(0, 3);
        if (!r.items.empty()) info->thumbnail = thumbnail(r.items[0].id);
    }
    return r;
}

ChannelResults search_channels(const std::string& query) {
    ChannelResults out;
    json_t* body = json_object();
    json_object_set_new(body, "query", json_string(query.c_str()));
    json_object_set_new(body, "params", json_string(PARAMS_CHANNELS));
    std::string err;
    json::Doc doc = call("search", WEB, body, err);
    if (!doc) {
        out.error = err;
        return out;
    }
    json::for_each_key(doc.get(), "channelRenderer", [&](json_t* r) {
        Channel c;
        c.id = json::str(r, {"channelId"});
        c.name = first_text(r, {"title"});
        c.avatar = best_image(json::at(r, {"thumbnail"}));
        c.description = json::yt_text(json_object_get(r, "descriptionSnippet"));
        // YouTube puts the handle in "subscriberCountText" and the subscribers in "videoCountText"
        // since handles arrived; sort them out by what they say.
        for (const char* k : {"subscriberCountText", "videoCountText"}) {
            std::string t = json::yt_text(json_object_get(r, k));
            if (t.empty()) continue;
            if (t[0] == '@') c.handle = t;
            else if (t.find("subscriber") != std::string::npos) c.subscribers = t;
            else c.videos = t;
        }
        if (!c.id.empty() && !c.name.empty()) out.items.push_back(c);
    });
    out.ok = true;
    return out;
}

int64_t age_seconds(const std::string& published) {
    static const std::pair<const char*, int64_t> UNITS[] = {
        {"second", 1}, {"minute", 60}, {"hour", 3600}, {"day", 86400},
        {"week", 7 * 86400}, {"month", 30 * 86400}, {"year", 365 * 86400},
    };
    size_t i = published.find_first_of("0123456789");
    if (i == std::string::npos) return INT64_MAX / 2;
    int64_t n = 0;
    while (i < published.size() && isdigit((unsigned char)published[i])) n = n * 10 + (published[i++] - '0');
    for (auto& u : UNITS)
        if (published.find(u.first, i) != std::string::npos) return n * u.second;
    return INT64_MAX / 2;
}

Results subscription_feed(const std::vector<std::string>& channel_ids, size_t per_channel,
                          std::vector<Channel>* channels) {
    // A handful of channels at a time: parallel enough to be quick, gentle enough on the Wii U.
    const size_t BATCH = 6;
    std::vector<Results> per(channel_ids.size());
    std::vector<Channel> headers(channel_ids.size());
    for (size_t b = 0; b < channel_ids.size(); b += BATCH) {
        std::vector<std::future<Results>> jobs;
        for (size_t i = b; i < std::min(b + BATCH, channel_ids.size()); i++)
            jobs.push_back(std::async(std::launch::async, [id = channel_ids[i], h = &headers[i]] {
                return channel_tab(id, Tab::VIDEOS, "", h);
            }));
        for (size_t k = 0; k < jobs.size(); k++) per[b + k] = jobs[k].get();
    }
    if (channels) *channels = std::move(headers);
    struct Dated {
        int64_t age;
        size_t order;
        Video v;
    };
    std::vector<Dated> all;
    Results out;
    for (size_t c = 0; c < per.size(); c++) {
        if (!per[c].ok && out.error.empty()) out.error = per[c].error;
        for (size_t k = 0; k < per[c].items.size() && k < per_channel; k++) {
            Video& v = per[c].items[k];
            if (v.channel_id.empty()) v.channel_id = channel_ids[c];
            // Upcoming premieres and streams have no age yet; keep them after today's uploads.
            all.push_back({v.live ? 0 : age_seconds(v.published), k * per.size() + c, v});
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Dated& a, const Dated& b) {
        return a.age != b.age ? a.age < b.age : a.order < b.order;
    });
    for (auto& d : all) out.items.push_back(std::move(d.v));
    out.ok = !out.items.empty() || out.error.empty();
    return out;
}

std::vector<Segment> sponsor_segments(const std::string& video_id) {
    static const std::string api = util::env_or("COFFEEFLIX_SPONSORBLOCK_API", "https://sponsor.ajay.app/api/");
    std::string url = api + "skipSegments?videoID=" + util::url_encode(video_id) +
                      "&categories=" + util::url_encode("[\"sponsor\",\"selfpromo\",\"interaction\"]");
    std::vector<Segment> out;
    http::Response r = http::get(url, {}, 6);
    if (!r.ok()) return out;  // 404: nobody has submitted segments for this video
    json::Doc doc = json::Doc::parse(r.body);
    for (size_t i = 0; i < json::size(doc.get()); i++) {
        json_t* s = json_array_get(doc.get(), i);
        if (json::str(s, {"actionType"}, "skip") != "skip") continue;
        Segment seg;
        seg.start = json::real(s, {"segment", 0});
        seg.end = json::real(s, {"segment", 1});
        seg.category = json::str(s, {"category"});
        if (seg.end - seg.start >= 1) out.push_back(seg);
    }
    std::sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
    return out;
}

Results related(const std::string& video_id) {
    std::string err;
    json_t* body = json_object();
    json_object_set_new(body, "videoId", json_string(video_id.c_str()));
    json::Doc doc = call("next", WEB, body, err);
    if (!doc) {
        Results r;
        r.error = err;
        return r;
    }
    // The sidebar ("up next"); the rest of the watch page describes the video itself.
    json_t* side = json::at(doc.get(), {"contents", "twoColumnWatchNextResults", "secondaryResults"});
    Results r = parse_results(side ? side : doc.get());
    r.items.erase(std::remove_if(r.items.begin(), r.items.end(), [&](const Video& v) { return v.id == video_id; }),
                  r.items.end());
    return r;
}

std::string thumbnail(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"; }
std::string thumbnail_hq(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg"; }

bool resolve(const std::string& id, int max_height, player::Source& src, std::string& error) {
    std::future<std::vector<Segment>> segments;
    if (store::get_bool("yt_sponsorblock", true))
        segments = std::async(std::launch::async, [id] { return sponsor_segments(id); });
    auto take_segments = [&] {
        if (!segments.valid()) return;
        static const std::pair<const char*, const char*> LABELS[] = {
            {"sponsor", "Skipped sponsor"}, {"selfpromo", "Skipped self-promotion"},
            {"interaction", "Skipped subscribe reminder"}};
        src.skip_segments.clear();
        for (const Segment& seg : segments.get()) {
            const char* label = "Skipped segment";
            for (auto& l : LABELS)
                if (seg.category == l.first) label = l.second;
            src.skip_segments.push_back({seg.start, seg.end, label});
        }
        if (!src.skip_segments.empty())
            log_message(LOG_OK, "YouTube", "%s: %d SponsorBlock segments", id.c_str(), (int)src.skip_segments.size());
    };
    std::string first_error;
    for (const Client* c : {&VISIONOS, &ANDROID_VR}) {
        std::string err;
        if (try_client(*c, id, max_height, src, err)) {
            if (!src.live) take_segments();
            return true;
        }
        log_message(LOG_WARNING, "YouTube", "%s client failed for %s: %s", c->name, id.c_str(), err.c_str());
        if (first_error.empty()) first_error = err;
    }
    error = first_error.empty() ? "YouTube playback failed" : first_error;
    return false;
}

player::Source make_source(const Video& v) {
    player::Source s;
    s.title = v.title;
    s.subtitle = v.channel;
    s.artwork = thumbnail_hq(v.id);
    s.service = "youtube";
    s.id = v.id;
    s.live = v.live;
    s.extra = v.channel;
    s.start = v.live ? 0 : store::resume_position("youtube", v.id);
    s.channel_id = v.channel_id;
    s.qualities = {360, 480, 720, 1080};
    s.quality = (int)store::get_int("yt_quality", 720);
    s.quality_setting = "yt_quality";
    std::string id = v.id;
    s.resolve = [id](player::Source& src, std::string& err) { return resolve(id, src.quality, src, err); };
    s.on_stop = [v](double position, bool finished) { yt_recs::on_watch(v, position, finished); };
    // Nearly every video has auto-generated captions; don't turn them on unless asked to.
    s.subs_auto = store::get_bool("yt_captions", false);
    return s;
}

}  // namespace youtube
