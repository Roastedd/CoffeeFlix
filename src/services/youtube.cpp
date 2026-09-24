#include "services/youtube.hpp"

#include <algorithm>
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

namespace youtube {

const char* PARAMS_VIDEOS = "EgIQAQ%3D%3D";          // type: video
const char* PARAMS_POPULAR_WEEK = "CAMSBAgDEAE%3D";  // sort: views, upload: this week, type: video

namespace {

const char* API = "https://www.youtube.com/youtubei/v1/";

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
const Client ANDROID_VR{"ANDROID_VR", 28, "1.62.27",
                        "com.google.android.apps.youtube.vr.oculus/1.62.27 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
                        "Oculus", "Quest 3", "Android", "12L", 32};
const Client IOS{"IOS", 5, "20.10.4", "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
                 "Apple", "iPhone16,2", "iPhone", "18.3.2.22D82", 0};

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
    http::Response r = http::post_json(std::string(API) + endpoint + "?prettyPrint=false", payload, headers, 20);
    if (!r.ok()) {
        error = r.error.empty() ? "YouTube request failed" : r.error;
        return json::Doc();
    }
    json::Doc doc = json::Doc::parse(r.body);
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
    if (parts.size() > 0) v.channel = parts[0];
    if (parts.size() > 1) v.views = parts[1];
    if (parts.size() > 2) v.published = parts[2];
    v.channel_id = json::str(json::find_key(meta, "browseEndpoint", 12), {"browseId"});
    json::for_each_key(json::at(l, {"contentImage"}), "thumbnailBadgeViewModel", [&](json_t* b) {
        std::string t = json::str(b, {"text"});
        if (t == "LIVE" || json::str(b, {"badgeStyle"}) == "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE") v.live = true;
        else if (v.duration.empty() && !t.empty()) v.duration = t;
    });
    return !v.title.empty();
}

Results parse_results(json_t* root) {
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
    json::for_each_key(root, "continuationItemRenderer", [&](json_t* c) {
        std::string t = json::str(c, {"continuationEndpoint", "continuationCommand", "token"});
        if (!t.empty()) res.continuation = t;
    });
    res.ok = true;
    return res;
}

// --- playback ------------------------------------------------------------------

struct Format {
    std::string url, mime;
    int itag = 0, width = 0, height = 0, bitrate = 0, fps = 0;
};

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
        out.push_back(fm);
    }
    return out;
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
    bool live = json::boolean(details, {"isLive"}) ||
                (json::boolean(details, {"isLiveContent"}) && json::num(details, {"lengthSeconds"}) == 0);
    src.live = live;
    src.user_agent = c.user_agent;

    json_t* sd = json_object_get(root, "streamingData");
    std::string hls_url = json::str(sd, {"hlsManifestUrl"});
    if (live) {
        if (hls_url.empty()) {
            error = "Live stream isn't available";
            return false;
        }
        return from_hls(hls_url, max_height, c, src, error);
    }

    std::vector<Format> adaptive = formats(json_object_get(sd, "adaptiveFormats"));
    const Format* best_v = nullptr;
    const Format* best_a = nullptr;
    for (const Format& f : adaptive) {
        if (util::starts_with(f.mime, "video/mp4") && f.mime.find("avc1") != std::string::npos && f.height > 0 &&
            f.height <= max_height) {
            bool better = !best_v || f.height > best_v->height ||
                          (f.height == best_v->height && f.fps <= 30 && best_v->fps > 30) ||
                          (f.height == best_v->height && f.fps == best_v->fps && f.bitrate > best_v->bitrate);
            if (better) best_v = &f;
        }
        if (util::starts_with(f.mime, "audio/mp4") && (!best_a || f.bitrate > best_a->bitrate)) best_a = &f;
    }
    if (best_v && best_a) {
        src.url = best_v->url;
        src.audio_url = best_a->url;
        log_message(LOG_OK, "YouTube", "%s: itag %d (%dp) + itag %d via %s", id.c_str(), best_v->itag, best_v->height,
                    best_a->itag, c.name);
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
    std::string err;
    json_t* body = json_object();
    json_object_set_new(body, "browseId", json_string("FEtrending"));
    json::Doc doc = call("browse", WEB, body, err);
    if (doc) {
        Results r = parse_results(doc.get());
        if (r.items.size() >= 6) return r;
    }
    // YouTube retired the Trending page in 2025: popular-this-week instead.
    return search("trending", PARAMS_POPULAR_WEEK);
}

Results channel_videos(const std::string& channel_id, const std::string& continuation) {
    std::string err;
    json_t* body = json_object();
    if (!continuation.empty()) {
        json_object_set_new(body, "continuation", json_string(continuation.c_str()));
    } else {
        json_object_set_new(body, "browseId", json_string(channel_id.c_str()));
        json_object_set_new(body, "params", json_string("EgZ2aWRlb3PyBgQKAjoA"));  // "Videos" tab
    }
    json::Doc doc = call("browse", WEB, body, err);
    if (!doc) {
        Results r;
        r.error = err;
        return r;
    }
    return parse_results(doc.get());
}

std::string thumbnail(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"; }
std::string thumbnail_hq(const std::string& id) { return "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg"; }

bool resolve(const std::string& id, int max_height, player::Source& src, std::string& error) {
    std::string first_error;
    for (const Client* c : {&ANDROID_VR, &IOS}) {
        std::string err;
        if (try_client(*c, id, max_height, src, err)) return true;
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
    int q = (int)store::get_int("yt_quality", 720);
    std::string id = v.id;
    s.resolve = [id, q](player::Source& src, std::string& err) { return resolve(id, q, src, err); };
    return s;
}

}  // namespace youtube
