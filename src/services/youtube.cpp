#include "services/youtube.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"
#include "services/hls.hpp"
#include "services/yt_account.hpp"
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
// without a JS runtime as of 2026.08, and Flow's primary client). ANDROID_VR's file URLs
// stopped after about a minute for guests, so for them it's only used for live HLS; signed in,
// its files downloaded whole (2026.09) and play what guests can't, like age-restricted videos.
// IOS returns SABR-only or 403ing URLs and is gone.
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

// `token`: an access token of the signed-in account (services/yt_account).
// `fields` (an X-Goog-FieldMask) trims the answer to what's needed: a whole player response
// is a lot of JSON for the Wii U to parse.
json::Doc call(const char* endpoint, const Client& c, json_t* body, std::string& error, const std::string& token = "",
               long* status = nullptr, const char* fields = nullptr) {
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
    if (!token.empty()) headers.emplace_back("Authorization", "Bearer " + token);
    if (fields) headers.emplace_back("X-Goog-FieldMask", fields);
    http::Response r = http::post_json(api_base() + endpoint + "?prettyPrint=false", payload, headers, 20);
    if (status) *status = r.status;
    if (!r.ok()) {
        error = !r.error.empty() ? r.error : r.status ? util::fmt(tr("YouTube answered %ld"), r.status) : tr("YouTube request failed");
        return json::Doc();
    }
    double t0 = util::now_seconds();
    json::Doc doc = json::Doc::parse(r.body);
    double took = util::now_seconds() - t0;
    if (took > 1.0 || getenv("COFFEEFLIX_HTTP_TRACE"))
        log_message(took > 1.0 ? LOG_WARNING : LOG_DEBUG, "YouTube", "%s: %zu KB of JSON parsed in %.2f s", endpoint,
                    r.body.size() / 1024, took);
    if (!doc) error = tr("Unexpected response from YouTube");
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
    v.duration = "Short";  // kept in English, as saved lists have it: see duration_label()
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

// A request as the signed-in account. Its token works with the VR client, not WEB; its pages
// are the older "compactVideoRenderer" lists. `make_body` runs once per attempt (call() takes it).
json::Doc account_call(const char* endpoint, const std::function<json_t*()>& make_body, std::string& err,
                       const char* fields = nullptr) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string token = yt_account::access_token(attempt > 0, err);  // again once when turned down
        if (token.empty()) return json::Doc();
        long status = 0;
        json::Doc doc = call(endpoint, ANDROID_VR, make_body(), err, token, &status, fields);
        if (doc || status != 401) return doc;
    }
    return json::Doc();
}

json::Doc account_browse(const std::string& browse_id, const std::string& continuation, std::string& err) {
    return account_call("browse", [&] {
        json_t* body = json_object();
        if (!continuation.empty()) json_object_set_new(body, "continuation", json_string(continuation.c_str()));
        else json_object_set_new(body, "browseId", json_string(browse_id.c_str()));
        return body;
    }, err);
}

// The next page of a VR list, whichever way it's given.
std::string next_page(json_t* root) {
    std::string c = json::str(json::find_key(root, "nextContinuationData", 16), {"continuation"});
    if (c.empty()) c = json::str(json::find_key(root, "continuationCommand", 16), {"token"});
    return c;
}

Results account_feed(const char* browse_id, const std::string& continuation) {
    std::string err;
    json::Doc doc = account_browse(browse_id, continuation, err);
    if (!doc) return fail(err);
    Results r = parse_results(doc.get());
    // The VR pages continue with "nextContinuationData" rather than a continuation item.
    if (r.continuation.empty()) r.continuation = next_page(doc.get());
    if (r.items.empty()) r.continuation.clear();
    return r;
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
    int average_bitrate = 0;  // over the whole file (`bitrate` is its peak): what Auto budgets with
    // Audio: videos with dubs have one set of formats per language.
    std::string audio_id, audio_label;  // "en-US.4", "English (US) original"
    bool audio_original = false, audio_dubbed = false, drc = false;
};

// Auto: up to 1080p for a video (within player::auto_budget above 720p), 720p for HLS (live
// streams), which has no bitrates to budget with.
constexpr int AUTO_MAX_HEIGHT = 1080, AUTO_HLS_HEIGHT = 720;

// A format's quality as YouTube names it, by the shorter side: a vertical video's 480p is 480×854.
int lines(const Format& f) { return f.width > 0 ? std::min(f.width, f.height) : f.height; }

// The qualities a video has, as player::Source::qualities, from its H.264 streams' {height, fps}
// (up to 1080p: the Wii U's decoder): 720p and 1080p at 60 and 30 fps when it's 60 fps (on the Wii U
// as far as player::max_fps allows). Sets src.qualities and src.hfr.
void set_qualities(const std::vector<std::pair<int, int>>& streams, player::Source& src) {
    src.hfr = false;
    for (auto [h, fps] : streams)
        if (h >= 720 && h <= 1080 && fps > 31) src.hfr = true;
    std::vector<int> q;
    for (auto [h, fps] : streams) {
        if (h <= 0 || h > 1080) continue;
        if (h == 720) {
            q.push_back(72060);
            if (src.hfr) q.push_back(72030);
        } else if (h == 1080) {
            if (fps > 31 && player::max_fps(1080, 108060) > 31) q.push_back(108060);
            if (fps <= 31 || player::max_fps(1080, 108030) > 31) q.push_back(108030);
        } else {
            q.push_back(h);
        }
    }
    auto key = [](int v) { return std::make_pair(player::quality_height(v), v); };
    std::sort(q.begin(), q.end(), [&](int a, int b) { return key(a) < key(b); });
    q.erase(std::unique(q.begin(), q.end()), q.end());
    if (!q.empty()) src.qualities = std::move(q);
}

// Which of set_qualities()'s a stream of `height` and `fps` is, asked for as `quality`.
int quality_of(int height, int fps, int quality, bool hfr) {
    if (height == 1080) return hfr && fps > 31 && quality != 108030 ? 108060 : 108030;
    if (height == 720) return hfr && fps > 31 && quality == 72030 ? 72030 : 72060;
    return height;
}

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
        fm.average_bitrate = (int)json::num(f, {"averageBitrate"});
        if (fm.average_bitrate <= 0) fm.average_bitrate = fm.bitrate;
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
        if (f->audio_dubbed) label = util::fmt(tr("%s (auto-dubbed)"), label.c_str());
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

// `quality`: as in player::Source (a height, perhaps with a frame rate).
bool from_hls(const std::string& manifest, int quality, const Client& c, player::Source& src, std::string& error) {
    http::Response r = http::get(manifest, {{"User-Agent", c.user_agent}}, 15);
    if (!r.ok()) {
        error = tr("Couldn't load the stream playlist");
        return false;
    }
    hls::Master m = hls::parse(r.body, manifest);
    if (!m.is_master) {
        src.url = manifest;
        return true;
    }
    const hls::Variant* v = hls::pick(m, player::quality_height(quality), true,
                                      [quality](int h) { return player::max_fps(h, quality); });
    if (!v) {
        error = tr("No compatible stream (H.264) found");
        return false;
    }
    std::vector<std::pair<int, int>> streams;
    for (const hls::Variant& o : m.variants)
        if (o.height > 0 && (o.codecs.empty() || o.codecs.find("avc1") != std::string::npos))
            streams.emplace_back(o.height, (int)std::lround(o.fps));
    set_qualities(streams, src);
    if (src.quality != 0) src.quality = quality_of(v->height, (int)std::lround(v->fps), quality, src.hfr);
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

// A "player" response's views, likes and the day it was posted.
Details details_of(json_t* root) {
    Details d;
    json_t* info = json_object_get(root, "videoDetails");
    json_t* micro = json::at(root, {"microformat", "playerMicroformatRenderer"});
    const int64_t views = json::num(info, {"viewCount"}, json::num(micro, {"viewCount"}, -1));
    if (views >= 0) d.views = views == 1 ? tr("1 view") : util::fmt(tr("%s views"), util::format_count(views).c_str());
    const int64_t likes = json::num(micro, {"likeCount"}, -1);
    if (likes > 0) d.likes = likes == 1 ? tr("1 like") : util::fmt(tr("%s likes"), util::format_count(likes).c_str());
    d.posted = i18n::long_date(json::str(micro, {"publishDate"}, json::str(micro, {"uploadDate"})));
    return d;
}

// `token`: as the signed-in account (services/yt_account), for what guests don't get to see.
bool try_client(const Client& c, const std::string& id, int quality, player::Source& src, std::string& error,
                const std::string& token = "") {
    const int max_height = player::quality_height(quality);
    json_t* body = json_object();
    json_object_set_new(body, "videoId", json_string(id.c_str()));
    json_object_set_new(body, "contentCheckOk", json_true());
    json_object_set_new(body, "racyCheckOk", json_true());
    json::Doc doc = call("player", c, body, error, token);
    if (!doc) return false;
    json_t* root = doc.get();

    std::string status = json::str(root, {"playabilityStatus", "status"});
    if (status != "OK") {
        error = json::str(root, {"playabilityStatus", "reason"});
        if (error.empty()) error = json::yt_text(json::at(root, {"playabilityStatus", "errorScreen", "playerErrorMessageRenderer", "reason"}));
        if (error.empty()) error = util::fmt(tr("This video can't be played (%s)"), status.c_str());
        return false;
    }

    json_t* details = json_object_get(root, "videoDetails");
    if (src.title.empty()) src.title = json::str(details, {"title"});
    if (src.subtitle.empty()) src.subtitle = json::str(details, {"author"});
    if (src.channel_id.empty()) src.channel_id = json::str(details, {"channelId"});
    bool live = json::boolean(details, {"isLive"}) ||
                (json::boolean(details, {"isLiveContent"}) && json::num(details, {"lengthSeconds"}) == 0);
    src.live = live;
    if (!live) {  // the feed's text stays for a stream: its count is of the people watching now
        Details d = details_of(root);
        if (!d.views.empty()) src.views = d.views;
        if (!d.posted.empty()) src.posted = d.posted;
    }
    src.user_agent = c.user_agent;
    // Files (not HLS) through libcurl in ranged chunks: FFmpeg's single whole-file request to
    // googlevideo.com never gets going on the Wii U.
    src.chunked_http = false;
    add_captions(json::at(root, {"captions", "playerCaptionsTracklistRenderer", "captionTracks"}), src);

    json_t* sd = json_object_get(root, "streamingData");
    std::string hls_url = json::str(sd, {"hlsManifestUrl"});
    // Auto (max_height 0): the best that fits the budget.
    const bool auto_q = max_height <= 0;
    if (live) {
        if (hls_url.empty()) {
            error = tr("Live stream isn't available");
            return false;
        }
        return from_hls(hls_url, auto_q ? AUTO_HLS_HEIGHT : quality, c, src, error);
    }
    if (&c == &ANDROID_VR && token.empty()) {
        error = tr("This video can't be played right now");
        return false;
    }

    std::vector<Format> adaptive = formats(json_object_get(sd, "adaptiveFormats"));
    const Format* best_a = pick_audio(adaptive, src);
    const int audio_rate = best_a ? best_a->average_bitrate : 0;
    const int budget = !auto_q ? 0 : src.bitrate_cap > 0 ? src.bitrate_cap : player::auto_cap();
    const int hd_budget = auto_q ? player::auto_budget() : 0;
    // Pass 0: within the budget (Auto after a slow connection). Pass 1: the lightest there is (Auto;
    // the budget is too small for all of them) or the same as pass 0. 60 fps doubles the decoding
    // work: only as far as player::max_fps allows, unless there is nothing else (pass 2).
    // Up to 1080 pixels high whatever the shape: the Wii U's decoder.
    auto h264 = [](const Format& f) {
        return util::starts_with(f.mime, "video/mp4") && f.mime.find("avc1") != std::string::npos && f.height > 0 &&
               f.height <= 1080;
    };
    auto usable = [&](const Format& f, int pass) {
        return h264(f) && lines(f) <= (auto_q ? AUTO_MAX_HEIGHT : max_height) &&
               (lines(f) <= 720 || !hd_budget || f.average_bitrate + audio_rate <= hd_budget) &&
               (pass == 2 || f.fps <= player::max_fps(lines(f), quality)) &&
               (pass != 0 || !budget || f.average_bitrate + audio_rate <= budget);
    };
    const Format* best_v = nullptr;
    for (int pass = 0; pass < 3 && !best_v; pass++) {
        for (const Format& f : adaptive) {
            if (!usable(f, pass)) continue;
            bool better = pass == 1 && budget
                              ? !best_v || f.average_bitrate < best_v->average_bitrate
                              : !best_v || lines(f) > lines(*best_v) ||
                                    (lines(f) == lines(*best_v) && f.fps <= 30 && best_v->fps > 30) ||
                                    (lines(f) == lines(*best_v) && f.fps == best_v->fps && f.bitrate > best_v->bitrate);
            if (better) best_v = &f;
        }
    }
    if (best_v && best_a) {
        std::vector<std::pair<int, int>> streams;
        for (const Format& f : adaptive)
            if (h264(f)) streams.emplace_back(lines(f), f.fps);
        set_qualities(streams, src);
        if (!auto_q) src.quality = quality_of(lines(*best_v), best_v->fps, quality, src.hfr);
        src.url = best_v->url;
        src.audio_url = best_a->url;
        src.chunked_http = true;
        src.bitrate = best_v->average_bitrate + audio_rate;
        src.min_bitrate = src.bitrate;
        for (const Format& f : adaptive)
            if (usable(f, 1)) src.min_bitrate = std::min(src.min_bitrate, f.average_bitrate + audio_rate);
        log_message(LOG_OK, "YouTube", "%s: itag %d (%dp%d, %d kbps) + itag %d%s%s via %s%s", id.c_str(), best_v->itag,
                    lines(*best_v), best_v->fps, best_v->average_bitrate / 1000, best_a->itag,
                    best_a->audio_label.empty() ? "" : ", ", best_a->audio_label.c_str(), c.name,
                    budget      ? util::fmt(" (Auto: up to %d kbps)", budget / 1000).c_str()
                    : hd_budget ? util::fmt(" (Auto: above 720p up to %d kbps)", hd_budget / 1000).c_str()
                                : "");
        return true;
    }
    // Progressive (video+audio in one file) fallback: usually 360p.
    std::vector<Format> muxed = formats(json_object_get(sd, "formats"));
    const Format* best_m = nullptr;
    for (const Format& f : muxed)
        if (f.mime.find("avc1") != std::string::npos && (!best_m || f.height > best_m->height)) best_m = &f;
    if (best_m) {
        src.url = best_m->url;
        src.chunked_http = true;
        log_message(LOG_OK, "YouTube", "%s: progressive itag %d via %s", id.c_str(), best_m->itag, c.name);
        return true;
    }
    if (!hls_url.empty()) return from_hls(hls_url, auto_q ? AUTO_HLS_HEIGHT : quality, c, src, error);
    error = tr("No playable H.264 streams");
    return false;
}

}  // namespace

const std::vector<Topic>& topics() {
    static const std::vector<Topic> t = {
        {N_("Music"), "music video", ic::MUSIC},
        {N_("Gaming"), "gaming", ic::SPORTS_ESPORTS},
        {N_("News"), "news today", ic::NEWSPAPER},
        {N_("Sports"), "sports highlights", ic::SPORTS_SOCCER},
        {N_("Science"), "science explained", ic::AUTO_AWESOME},
        {N_("Trailers"), "official trailer", ic::MOVIE},
        {N_("Comedy"), "comedy", ic::PEOPLE},
        {N_("Cooking"), "recipe", ic::LOCAL_CAFE},
    };
    return t;
}

// X-Goog-FieldMask for searches: only what parse_renderer and parse_lockup read, in the places
// YouTube puts results. A whole answer is 300 KB to 1.5 MB of JSON; this keeps about 5% of it.
// Every path has to exist in YouTube's schema (gridVideoRenderer has no ownerText, say), or the
// whole request fails with 400.
const std::string& search_fields() {
    static const std::string mask = [] {
        static const char* const VIDEO[] = {"videoId", "title", "ownerText", "shortBylineText", "lengthText",
                                            "shortViewCountText", "viewCountText", "publishedTimeText",
                                            "thumbnailOverlays.thumbnailOverlayTimeStatusRenderer",
                                            "badges.metadataBadgeRenderer.style"};
        static const char* const LOCKUP[] = {"contentType", "contentId", "metadata.lockupMetadataViewModel.title",
                                             "metadata.lockupMetadataViewModel.metadata",
                                             "contentImage.thumbnailViewModel.overlays"};
        enum Kind { V, G, L };
        struct Place {
            const char* path;
            Kind kind;
        };
        static const Place FIRST[] = {
            {"videoRenderer", V},
            {"shelfRenderer.content.verticalListRenderer.items.videoRenderer", V},
            {"shelfRenderer.content.horizontalListRenderer.items.gridVideoRenderer", G},
            {"lockupViewModel", L},
            {"shelfRenderer.content.verticalListRenderer.items.lockupViewModel", L},
            {"officialCardViewModel.contents.horizontalShelfViewModel.items.lockupViewModel", L},
        };
        static const Place MORE[] = {{"videoRenderer", V}, {"lockupViewModel", L}};
        std::string out;
        auto add = [&](const std::string& path) {
            if (!out.empty()) out += ',';
            out += path;
        };
        auto section = [&](const std::string& root, const Place* places, size_t n) {
            for (size_t i = 0; i < n; i++) {
                std::string base = root + ".itemSectionRenderer.contents." + places[i].path + ".";
                if (places[i].kind == L) {
                    for (const char* f : LOCKUP) add(base + f);
                } else {
                    for (const char* f : VIDEO)
                        if (places[i].kind == V || strcmp(f, "ownerText") != 0) add(base + f);
                }
            }
            add(root + ".continuationItemRenderer.continuationEndpoint.continuationCommand.token");
        };
        section("contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents", FIRST,
                sizeof(FIRST) / sizeof(FIRST[0]));
        section("onResponseReceivedCommands.appendContinuationItemsAction.continuationItems", MORE,
                sizeof(MORE) / sizeof(MORE[0]));
        return out;
    }();
    return mask;
}

Results search(const std::string& query, const std::string& params, const std::string& continuation) {
    auto run = [&](bool mask, long& status) {
        json_t* body = json_object();
        if (!continuation.empty()) {
            json_object_set_new(body, "continuation", json_string(continuation.c_str()));
        } else {
            json_object_set_new(body, "query", json_string(query.c_str()));
            if (!params.empty()) json_object_set_new(body, "params", json_string(util::url_decode(params).c_str()));
        }
        std::string err;
        json::Doc doc = call("search", WEB, body, err, "", &status, mask ? search_fields().c_str() : nullptr);
        if (!doc) {
            Results res;
            res.error = err;
            return res;
        }
        return parse_results(doc.get());
    };
    // Should YouTube turn the mask down, or move results out of it, the rest of the session asks
    // for whole answers.
    static std::atomic<bool> masked{true};
    long status = 0;
    if (!masked) return run(false, status);
    Results res = run(true, status);
    if (status != 400 && (!res.ok || !res.items.empty())) return res;
    long whole_status = 0;
    Results whole = run(false, whole_status);
    if (status == 400 || !whole.items.empty()) {
        log_message(LOG_WARNING, "YouTube", "Trimmed search came back %s, asking for whole answers from now on",
                    status == 400 ? "refused" : "empty");
        masked = false;
    }
    return whole;
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

Results account_home(const std::string& continuation) { return account_feed("FEwhat_to_watch", continuation); }

Results account_subscriptions(const std::string& continuation) { return account_feed("FEsubscriptions", continuation); }

ChannelResults account_channels() {
    ChannelResults out;
    std::string err, continuation;
    std::set<std::string> seen;
    // A few hundred channels come in pages; the cap only stops a list that never ends.
    for (int page = 0; page < 50; page++) {
        json::Doc doc = account_browse("FEchannels", continuation, err);
        if (!doc) {
            if (page == 0) {
                out.error = err;
                return out;
            }
            log_message(LOG_WARNING, "YouTube", "Only part of the account's channels loaded (%s)", err.c_str());
            break;
        }
        size_t before = out.items.size();
        json::for_each_key(doc.get(), "compactChannelRenderer", [&](json_t* r) {
            Channel c;
            c.id = json::str(r, {"channelId"});
            c.name = first_text(r, {"displayName", "title"});
            c.avatar = best_image(json::at(r, {"thumbnail"}));
            c.subscribers = first_text(r, {"subscriberCountText"});
            if (!c.id.empty() && !c.name.empty() && seen.insert(c.id).second) out.items.push_back(c);
        });
        std::string next = next_page(doc.get());
        if (next.empty() || next == continuation || out.items.size() == before) break;
        continuation = next;
    }
    out.ok = true;
    return out;
}

bool account_subscribe(const std::string& channel_id, bool on, std::string& error) {
    // The "params" the apps send with the button.
    json::Doc doc = account_call(on ? "subscription/subscribe" : "subscription/unsubscribe", [&] {
        json_t* body = json_object();
        json_t* ids = json_array();
        json_array_append_new(ids, json_string(channel_id.c_str()));
        json_object_set_new(body, "channelIds", ids);
        json_object_set_new(body, "params", json_string(on ? "EgIIAhgA" : "CgIIAhgA"));
        return body;
    }, error);
    if (!doc) return false;
    // It answers with the new state of the button when it has one.
    json_t* done = json::find_key(doc.get(), "updateSubscribeButtonAction", 8);
    if (done && json::boolean(done, {"subscribed"}, on) != on) {
        error = tr("YouTube didn't change it");
        return false;
    }
    return true;
}

bool account_info(AccountInfo& out, std::string& error) {
    json::Doc doc = account_browse("FElibrary", "", error);
    if (!doc) return false;
    json_t* a = json::find_key(doc.get(), "activeAccountHeaderRenderer", 16);
    out.name = json::yt_text(json_object_get(a, "accountName"));
    out.photo = best_image(json::at(a, {"accountPhoto"}));
    if (out.name.empty()) error = tr("No account in the answer");
    return !out.name.empty();
}

// --- watch history and Watch later (signed in) --------------------------------------------------

namespace {

// YouTube's apps report playback as they go: a "playback" ping puts the video in the history,
// "watchtime" pings say how much of it was watched. The addresses come with the player response;
// asked for as the account, they count for it.
struct Tracking {
    std::string playback, watchtime, cpn;
    double reported = 0;
};
std::mutex g_track_m, g_report_m;
std::map<std::string, Tracking> g_tracking;  // by video id, while it plays

// `url` with `params` set, replacing any it had.
std::string with_params(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params) {
    size_t q = url.find('?');
    std::string query;
    if (q != std::string::npos) {
        std::string old = url.substr(q + 1);
        for (size_t i = 0; i <= old.size();) {
            size_t amp = std::min(old.find('&', i), old.size());
            std::string kv = old.substr(i, amp - i);
            std::string key = kv.substr(0, kv.find('='));
            bool replaced = std::any_of(params.begin(), params.end(), [&](const auto& p) { return p.first == key; });
            if (!kv.empty() && !replaced) query += (query.empty() ? "" : "&") + kv;
            i = amp + 1;
        }
    }
    for (const auto& [k, v] : params) query += (query.empty() ? "" : "&") + k + "=" + v;
    return url.substr(0, q) + "?" + query;
}

// A client playback nonce: 16 random characters naming this viewing.
std::string new_cpn() {
    static const char* A = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    static std::mt19937 rng((unsigned)(util::now_seconds() * 1000));
    std::string out;
    for (int i = 0; i < 16; i++) out += A[rng() & 63];
    return out;
}

bool ping(const std::string& url, std::string& error) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string token = yt_account::access_token(attempt > 0, error);  // again once when turned down
        if (token.empty()) return false;
        http::Response r = http::get(url, {{"User-Agent", ANDROID_VR.user_agent}, {"Authorization", "Bearer " + token}}, 15);
        if (r.ok()) return true;
        error = !r.error.empty() ? r.error : util::fmt(tr("YouTube answered %ld"), r.status);
        if (r.status != 401) return false;
    }
    return false;
}

std::string seconds(double s) { return util::fmt("%.3f", std::max(0.0, s)); }

}  // namespace

bool account_report_watched(const std::string& id, double position, bool done, std::string& error) {
    // One report at a time: a slow first one (it loads the player response) mustn't be overtaken.
    std::lock_guard<std::mutex> busy(g_report_m);
    Tracking t;
    bool started;
    {
        std::lock_guard<std::mutex> lk(g_track_m);
        auto it = g_tracking.find(id);
        started = it != g_tracking.end();
        if (started) t = it->second;
    }
    if (!started) {
        json::Doc doc = account_call("player", [&] {
            json_t* body = json_object();
            json_object_set_new(body, "videoId", json_string(id.c_str()));
            json_object_set_new(body, "contentCheckOk", json_true());
            json_object_set_new(body, "racyCheckOk", json_true());
            return body;
        }, error, "playbackTracking.videostatsPlaybackUrl.baseUrl,playbackTracking.videostatsWatchtimeUrl.baseUrl");
        if (!doc) return false;
        t.playback = json::str(doc.get(), {"playbackTracking", "videostatsPlaybackUrl", "baseUrl"});
        t.watchtime = json::str(doc.get(), {"playbackTracking", "videostatsWatchtimeUrl", "baseUrl"});
        if (t.playback.empty()) {
            error = tr("Unexpected response from YouTube");
            return false;
        }
        t.cpn = new_cpn();
        if (!ping(with_params(t.playback, {{"ver", "2"}, {"cpn", t.cpn}, {"cmt", seconds(position)}, {"el", "detailpage"}}),
                  error))
            return false;
        log_message(LOG_OK, "YouTube", "%s: added to the account's history", id.c_str());
    }
    if (!t.watchtime.empty() && position > t.reported + 1) {
        std::string st = seconds(t.reported), et = seconds(position);
        if (!ping(with_params(t.watchtime, {{"ver", "2"}, {"cpn", t.cpn}, {"cmt", et}, {"st", st}, {"et", et},
                                            {"el", "detailpage"}}),
                  error))
            return false;
    }
    t.reported = std::max(t.reported, position);
    std::lock_guard<std::mutex> lk(g_track_m);
    if (done) g_tracking.erase(id);
    else g_tracking[id] = t;
    return true;
}

bool account_watch_later(const std::string& id, bool on, std::string& error) {
    json::Doc doc = account_call("browse/edit_playlist", [&] {
        json_t* action = json_object();
        json_object_set_new(action, "action", json_string(on ? "ACTION_ADD_VIDEO" : "ACTION_REMOVE_VIDEO_BY_VIDEO_ID"));
        json_object_set_new(action, on ? "addedVideoId" : "removedVideoId", json_string(id.c_str()));
        json_t* actions = json_array();
        json_array_append_new(actions, action);
        json_t* body = json_object();
        json_object_set_new(body, "playlistId", json_string("WL"));
        json_object_set_new(body, "actions", actions);
        return body;
    }, error);
    if (!doc) return false;
    std::string status = json::str(doc.get(), {"status"});
    if (!status.empty() && status != "STATUS_SUCCEEDED") {
        error = status;
        return false;
    }
    return true;
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

namespace {

void put_varint(std::string& out, uint64_t n) {
    while (n >= 0x80) {
        out += (char)((n & 0x7f) | 0x80);
        n >>= 7;
    }
    out += (char)n;
}

void put_bytes(std::string& out, int field, const std::string& data) {
    put_varint(out, (uint64_t)field << 3 | 2);
    put_varint(out, data.size());
    out += data;
}

void put_number(std::string& out, int field, uint64_t n) {
    put_varint(out, (uint64_t)field << 3);
    put_varint(out, n);
}

std::string base64url_encode(const std::string& in) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += A[n >> 18 & 63], out += A[n >> 12 & 63], out += A[n >> 6 & 63], out += A[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += A[n >> 18 & 63], out += A[n >> 12 & 63], out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += A[n >> 18 & 63], out += A[n >> 12 & 63], out += A[n >> 6 & 63], out += "=";
    }
    return out;
}

// The continuation the watch page's comments section starts from, built the same way the page
// builds it (a small protobuf), so the whole 400 KB watch page needn't be loaded first.
std::string comments_token(const std::string& video_id, bool newest) {
    std::string inner, section, video, token;
    put_bytes(inner, 4, video_id);
    put_number(inner, 6, newest ? 1 : 0);
    put_number(inner, 15, 2);
    put_bytes(section, 4, inner);
    put_bytes(section, 8, "comments-section");
    put_bytes(video, 2, video_id);
    put_bytes(token, 2, video);
    put_number(token, 3, 6);
    put_bytes(token, 6, section);
    return base64url_encode(token);
}

std::string continuation_token(json_t* item) {
    return json::str(json::find_key(item, "continuationCommand", 8), {"token"});
}

}  // namespace

Comments comments(const std::string& video_id, bool newest, const std::string& continuation) {
    Comments out;
    json_t* body = json_object();
    std::string token = continuation.empty() ? comments_token(video_id, newest) : continuation;
    json_object_set_new(body, "continuation", json_string(token.c_str()));
    json::Doc doc = call("next", WEB, body, out.error);
    if (!doc) return out;
    json_t* root = doc.get();

    // The text, author and counts are "entities" stored beside the list, by key.
    std::map<std::string, json_t*> entities;
    std::set<std::string> hearted;
    json_t* muts = json::at(root, {"frameworkUpdates", "entityBatchUpdate", "mutations"});
    for (size_t i = 0; i < json::size(muts); i++) {
        json_t* p = json::at(json_array_get(muts, i), {"payload"});
        if (json_t* c = json_object_get(p, "commentEntityPayload")) entities[json::str(c, {"key"})] = c;
        if (json_t* t = json_object_get(p, "engagementToolbarStateEntityPayload"))
            if (json::str(t, {"heartState"}) == "TOOLBAR_HEART_STATE_HEARTED") hearted.insert(json::str(t, {"key"}));
    }
    auto add = [&](json_t* view, json_t* thread) {
        json_t* e = entities[json::str(view, {"commentKey"})];
        if (!e) return;
        Comment c;
        c.id = json::str(view, {"commentId"});
        c.text = json::str(e, {"properties", "content", "content"});
        c.published = json::str(e, {"properties", "publishedTime"});
        c.reply = json::num(e, {"properties", "replyLevel"}) > 0;
        c.author = json::str(e, {"author", "displayName"});
        c.author_id = json::str(e, {"author", "channelId"});
        c.avatar = json::str(e, {"author", "avatarThumbnailUrl"});
        c.verified = json::boolean(e, {"author", "isVerified"});
        c.creator = json::boolean(e, {"author", "isCreator"});
        c.likes = json::str(e, {"toolbar", "likeCountNotliked"});
        c.replies = json::str(e, {"toolbar", "replyCount"});
        c.pinned = json::str(view, {"pinnedText"});
        c.hearted = hearted.count(json::str(view, {"toolbarStateKey"})) > 0;
        if (thread) c.replies_token = continuation_token(json::at(thread, {"replies"}));
        if (c.likes == "0") c.likes.clear();
        if (c.replies == "0") c.replies.clear();
        out.items.push_back(std::move(c));
    };

    json_t* eps = json_object_get(root, "onResponseReceivedEndpoints");
    for (size_t i = 0; i < json::size(eps); i++) {
        json_t* ep = json_array_get(eps, i);
        json_t* list = json::at(ep, {"reloadContinuationItemsCommand", "continuationItems"});
        if (!list) list = json::at(ep, {"appendContinuationItemsAction", "continuationItems"});
        for (size_t k = 0; k < json::size(list); k++) {
            json_t* item = json_array_get(list, k);
            if (json_t* h = json_object_get(item, "commentsHeaderRenderer")) {
                out.count = json::yt_text(json_object_get(h, "countText"));
            } else if (json_t* t = json_object_get(item, "commentThreadRenderer")) {
                add(json::at(t, {"commentViewModel", "commentViewModel"}), t);
            } else if (json_t* v = json_object_get(item, "commentViewModel")) {
                add(v, nullptr);  // a reply
            } else if (json_t* more = json_object_get(item, "continuationItemRenderer")) {
                out.continuation = continuation_token(more);
            }
        }
    }
    // A video with comments turned off answers with nothing at all.
    out.off = continuation.empty() && out.items.empty() && out.count.empty();
    out.ok = true;
    return out;
}

std::string duration_label(const Video& v) { return v.duration == "Short" ? tr("Short") : v.duration; }

std::string thumbnail(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"; }
std::string thumbnail_hq(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg"; }

bool resolve(const std::string& id, int quality, player::Source& src, std::string& error) {
    std::future<std::vector<Segment>> segments;
    if (store::get_bool("yt_sponsorblock", true))
        segments = std::async(std::launch::async, [id] { return sponsor_segments(id); });
    auto take_segments = [&] {
        if (!segments.valid()) return;
        static const std::pair<const char*, const char*> LABELS[] = {
            {"sponsor", N_("Skipped sponsor")}, {"selfpromo", N_("Skipped self-promotion")},
            {"interaction", N_("Skipped subscribe reminder")}};
        src.skip_segments.clear();
        for (const Segment& seg : segments.get()) {
            const char* label = tr("Skipped segment");
            for (auto& l : LABELS)
                if (seg.category == l.first) label = tr(l.second);
            src.skip_segments.push_back({seg.start, seg.end, label});
        }
        if (!src.skip_segments.empty())
            log_message(LOG_OK, "YouTube", "%s: %d SponsorBlock segments", id.c_str(), (int)src.skip_segments.size());
    };
    std::string first_error;
    for (const Client* c : {&VISIONOS, &ANDROID_VR}) {
        std::string err;
        if (try_client(*c, id, quality, src, err)) {
            if (!src.live) take_segments();
            return true;
        }
        log_message(LOG_WARNING, "YouTube", "%s client failed for %s: %s", c->name, id.c_str(), err.c_str());
        if (first_error.empty()) first_error = err;
    }
    // Signed in, again as the account: it may see what guests don't (age-restricted videos,
    // "confirm you're not a bot"). Only ANDROID_VR takes the token; VISIONOS answers 400.
    std::string token_error;
    std::string token = yt_account::signed_in() ? yt_account::access_token(false, token_error) : "";
    if (!token.empty()) {
        std::string err;
        if (try_client(ANDROID_VR, id, quality, src, err, token)) {
            log_message(LOG_OK, "YouTube", "%s: played signed in", id.c_str());
            if (!src.live) take_segments();
            return true;
        }
        log_message(LOG_WARNING, "YouTube", "ANDROID_VR client failed for %s signed in: %s", id.c_str(), err.c_str());
    }
    error = first_error.empty() ? tr("YouTube playback failed") : first_error;
    return false;
}

namespace {

void report_watched(const std::string& id, double position, bool done) {
    if (!yt_account::signed_in() || !store::get_bool("yt_history_sync", true)) return;
    tasks::submit(tasks::API, [id, position, done]() -> std::function<void()> {
        std::string err;
        if (!account_report_watched(id, position, done, err))
            log_message(LOG_WARNING, "YouTube", "Watch history for %s: %s", id.c_str(), err.c_str());
        return nullptr;
    });
}

}  // namespace

Details parse_details(const std::string& player_json) {
    json::Doc doc = json::Doc::parse(player_json);
    return doc ? details_of(doc.get()) : Details{};
}

Details details(const std::string& video_id, std::string& error) {
    json_t* body = json_object();
    json_object_set_new(body, "videoId", json_string(video_id.c_str()));
    json::Doc doc = call("player", WEB, body, error, "", nullptr,
                         "videoDetails.viewCount,microformat.playerMicroformatRenderer(viewCount,likeCount,publishDate,uploadDate)");
    return doc ? details_of(doc.get()) : Details{};
}

player::Source make_source(const Video& v) {
    player::Source s;
    s.title = v.title;
    s.subtitle = v.channel;
    s.artwork = thumbnail_hq(v.id);
    s.service = "youtube";
    s.id = v.id;
    s.live = v.live;
    s.views = v.views;
    s.posted = v.published;
    s.extra = v.channel;
    s.start = v.live ? 0 : store::resume_position("youtube", v.id);
    s.channel_id = v.channel_id;
    s.qualities = {360, 480, 72030, 72060, 108030, 108060};  // until resolve() has the video's own
    s.quality = (int)store::get_int("yt_quality", 0);
    s.quality_setting = "yt_quality";
    s.auto_quality = true;
    std::string id = v.id;
    s.resolve = [id](player::Source& src, std::string& err) { return resolve(id, src.quality, src, err); };
    // Signed in, the account's watch history follows along: once 10 s in, then only every couple
    // of minutes (each report is work for the CPU that also feeds the video), on pausing, and at
    // the end.
    auto reported = std::make_shared<double>(-1);
    s.on_progress = [id, live = v.live, reported](double position, bool paused) {
        if (live || position < 10) return;
        double moved = *reported < 0 ? 1e9 : std::fabs(position - *reported);
        if (moved < (paused ? 1 : 120)) return;
        *reported = position;
        report_watched(id, position, false);
    };
    s.on_stop = [v, reported](double position, bool finished) {
        yt_recs::on_watch(v, position, finished);
        if (!v.live && (*reported >= 0 || position >= 10 || finished)) report_watched(v.id, position, true);
    };
    // Nearly every video has auto-generated captions; don't turn them on unless asked to.
    s.subs_auto = store::get_bool("yt_captions", false);
    return s;
}

}  // namespace youtube
