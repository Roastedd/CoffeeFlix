#include "services/jellyfin.hpp"

#include <mutex>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"

namespace jellyfin {

namespace {

const char* CLIENT = "CoffeeFlix";
const char* VERSION = "2.0.0";

std::mutex g_m;
Account g_acc;
bool g_loaded = false;

void load_account() {
    if (g_loaded) return;
    g_acc.server = store::get_str("jf_server");
    g_acc.server_name = store::get_str("jf_server_name");
    g_acc.user_id = store::get_str("jf_user_id");
    g_acc.user_name = store::get_str("jf_user_name");
    g_acc.token = store::get_str("jf_token");
    g_loaded = true;
}

Account snapshot() {
    std::lock_guard<std::mutex> lk(g_m);
    load_account();
    return g_acc;
}

std::string device_id() {
    std::string id = store::get_str("jf_device_id");
    if (id.empty()) {
        id = "coffeeflix-" + util::random_hex(8);
        store::set_str("jf_device_id", id);
    }
    return id;
}

std::string auth_value(const std::string& token) {
    std::string v = util::fmt("MediaBrowser Client=\"%s\", Device=\"Wii U\", DeviceId=\"%s\", Version=\"%s\"", CLIENT,
                              device_id().c_str(), VERSION);
    if (!token.empty()) v += ", Token=\"" + token + "\"";
    return v;
}

http::Response request(const std::string& server, const std::string& token, const std::string& method,
                       const std::string& path, const std::string& body = "") {
    http::Request r;
    r.method = method;
    r.url = server + path;
    std::string auth = auth_value(token);
    r.headers = {{"Authorization", auth}, {"X-Emby-Authorization", auth}, {"Accept", "application/json"}};
    if (!body.empty() || method == "POST") {
        r.headers.emplace_back("Content-Type", "application/json");
        r.body = body.empty() ? "{}" : body;
    }
    r.timeout = 25;
    return http::perform(r);
}

http::Response api(const std::string& method, const std::string& path, const std::string& body = "") {
    Account a = snapshot();
    return request(a.server, a.token, method, path, body);
}

// Newer servers moved several /Users/{id}/... routes; try the new one first.
http::Response get_with_fallback(const std::string& path, const std::string& legacy) {
    http::Response r = api("GET", path);
    if (r.status == 404 || r.status == 405) r = api("GET", legacy);
    return r;
}

std::string first(json_t* arr) { return json::str(json_array_get(arr, 0)); }

Item parse_item(json_t* j) {
    Item it;
    it.id = json::str(j, {"Id"});
    it.name = json::str(j, {"Name"});
    it.type = json::str(j, {"Type"});
    it.collection_type = json::str(j, {"CollectionType"});
    it.overview = json::str(j, {"Overview"});
    int64_t year = json::num(j, {"ProductionYear"});
    if (year > 0) it.year = std::to_string(year);
    it.rating = json::str(j, {"OfficialRating"});
    it.series_name = json::str(j, {"SeriesName"});
    it.series_id = json::str(j, {"SeriesId"});
    it.season_id = json::str(j, {"SeasonId"});
    it.album_artist = json::str(j, {"AlbumArtist"});
    it.primary_tag = json::str(j, {"ImageTags", "Primary"});
    it.thumb_tag = json::str(j, {"ImageTags", "Thumb"});
    it.backdrop_tag = first(json_object_get(j, "BackdropImageTags"));
    it.series_primary_tag = json::str(j, {"SeriesPrimaryImageTag"});
    it.parent_backdrop_id = json::str(j, {"ParentBackdropItemId"});
    it.parent_backdrop_tag = first(json_object_get(j, "ParentBackdropImageTags"));
    it.parent_thumb_id = json::str(j, {"ParentThumbItemId"});
    it.parent_thumb_tag = json::str(j, {"ParentThumbImageTag"});
    it.album_id = json::str(j, {"AlbumId"});
    it.album_primary_tag = json::str(j, {"AlbumPrimaryImageTag"});
    it.runtime = json::num(j, {"RunTimeTicks"}) / 1e7;
    it.position = json::num(j, {"UserData", "PlaybackPositionTicks"}) / 1e7;
    it.played = json::boolean(j, {"UserData", "Played"});
    it.favorite = json::boolean(j, {"UserData", "IsFavorite"});
    it.unplayed = (int)json::num(j, {"UserData", "UnplayedItemCount"});
    it.community_rating = json::real(j, {"CommunityRating"});
    it.index = (int)json::num(j, {"IndexNumber"});
    it.parent_index = (int)json::num(j, {"ParentIndexNumber"});
    it.child_count = (int)json::num(j, {"ChildCount"});
    it.aspect = (float)json::real(j, {"PrimaryImageAspectRatio"});
    return it;
}

List parse_list(const http::Response& r) {
    List l;
    if (!r.ok()) {
        l.error = r.error;
        return l;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* root = doc.get();
    json_t* arr = json_is_array(root) ? root : json_object_get(root, "Items");
    for (size_t i = 0; i < json::size(arr); i++) l.items.push_back(parse_item(json_array_get(arr, i)));
    l.total = json_is_array(root) ? (int)l.items.size() : (int)json::num(root, {"TotalRecordCount"}, (int64_t)l.items.size());
    l.ok = doc.get() != nullptr;
    if (!l.ok) l.error = tr("Unexpected response from the server");
    return l;
}

const char* FIELDS = "fields=PrimaryImageAspectRatio,Overview,ProductionYear,OfficialRating,ChildCount&enableImageTypes=Primary,Backdrop,Thumb&imageTypeLimit=1";

std::string img(const std::string& item, const char* type, const std::string& tag, int max_w) {
    Account a = snapshot();
    return util::fmt("%s/Items/%s/Images/%s?maxWidth=%d&quality=88&tag=%s", a.server.c_str(), item.c_str(), type, max_w,
                     tag.c_str());
}

AuthResult finish_auth(const std::string& url, const http::Response& r) {
    AuthResult res;
    if (!r.ok()) {
        res.error = r.status == 401 ? tr("Wrong username or password") : r.error;
        return res;
    }
    json::Doc doc = json::Doc::parse(r.body);
    std::string token = json::str(doc.get(), {"AccessToken"});
    std::string uid = json::str(doc.get(), {"User", "Id"});
    if (token.empty() || uid.empty()) {
        res.error = tr("Unexpected response from the server");
        return res;
    }
    ServerInfo info = server_info(url);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_acc.server = url;
        g_acc.server_name = info.name;
        g_acc.token = token;
        g_acc.user_id = uid;
        g_acc.user_name = json::str(doc.get(), {"User", "Name"});
        g_loaded = true;
        store::set_str("jf_server", g_acc.server);
        store::set_str("jf_server_name", g_acc.server_name);
        store::set_str("jf_token", g_acc.token);
        store::set_str("jf_user_id", g_acc.user_id);
        store::set_str("jf_user_name", g_acc.user_name);
    }
    store::save_now();
    res.ok = true;
    return res;
}

// Above 720p the Wii U's hardware decoder manages about 50 pictures a second: faster video is
// sent at 30 fps (see player::max_fps).
std::string profile_json(int max_height) {
    int max_width = max_height >= 1080 ? 1920 : max_height >= 720 ? 1280 : 854;
    int bitrate = max_height >= 1080 ? 10000000 : max_height >= 720 ? 5000000 : 2500000;
    return util::fmt(R"({"DeviceProfile":{
"Name":"CoffeeFlix Wii U","MaxStreamingBitrate":%d,"MaxStaticBitrate":40000000,"MusicStreamingTranscodingBitrate":256000,
"DirectPlayProfiles":[
 {"Container":"mp4,m4v,mkv,mov","Type":"Video","VideoCodec":"h264","AudioCodec":"aac,mp3,ac3,eac3,flac,opus,vorbis,alac"},
 {"Container":"mp3,flac,m4a,aac,ogg,oga,opus,wav,webma","Type":"Audio"}],
"TranscodingProfiles":[
 {"Container":"ts","Type":"Video","VideoCodec":"h264","AudioCodec":"aac,mp3","Protocol":"hls","Context":"Streaming","MaxAudioChannels":"2","MinSegments":"1","BreakOnNonKeyFrames":true},
 {"Container":"mp3","Type":"Audio","AudioCodec":"mp3","Protocol":"http","Context":"Streaming","MaxAudioChannels":"2"}],
"CodecProfiles":[
 {"Type":"Video","Codec":"h264","Conditions":[
  {"Condition":"LessThanEqual","Property":"Width","Value":"%d"},
  {"Condition":"LessThanEqual","Property":"VideoLevel","Value":"42"},
  {"Condition":"LessThanEqual","Property":"VideoBitDepth","Value":"8"},
  {"Condition":"EqualsAny","Property":"VideoProfile","Value":"high|main|baseline|constrained baseline"}]},
 {"Type":"Video","Codec":"h264","ApplyConditions":[{"Condition":"GreaterThanEqual","Property":"Width","Value":"1281"}],
  "Conditions":[{"Condition":"LessThanEqual","Property":"VideoFramerate","Value":"30"}]}],
"SubtitleProfiles":[
 {"Format":"srt","Method":"External"},{"Format":"subrip","Method":"External"},{"Format":"ass","Method":"External"},
 {"Format":"ssa","Method":"External"},{"Format":"vtt","Method":"External"},{"Format":"webvtt","Method":"External"}],
"ContainerProfiles":[],"ResponseProfiles":[]}})",
                     bitrate, max_width);
}

void post_async(const std::string& path, const std::string& body) {
    Account a = snapshot();
    tasks::submit(tasks::API, [a, path, body]() -> std::function<void()> {
        request(a.server, a.token, "POST", path, body);
        return nullptr;
    });
}

bool resolve_video(const Item& it, double start, player::Source& src, std::string& error) {
    Account a = snapshot();
    int max_h = (int)store::get_int("jf_quality", 1080);
    std::string path = util::fmt("/Items/%s/PlaybackInfo?UserId=%s&StartTimeTicks=%lld&IsPlayback=true&AutoOpenLiveStream=true",
                                 it.id.c_str(), a.user_id.c_str(), (long long)(start * 1e7));
    http::Response r = request(a.server, a.token, "POST", path, profile_json(max_h));
    if (!r.ok()) {
        error = util::fmt(tr("Server refused playback: %s"), r.error.c_str());
        return false;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* ms = json::at(doc.get(), {"MediaSources", 0});
    if (!ms) {
        error = json::str(doc.get(), {"ErrorCode"}, tr("No playable media found"));
        return false;
    }
    std::string psid = json::str(doc.get(), {"PlaySessionId"});
    std::string msid = json::str(ms, {"Id"});
    std::string transcode = json::str(ms, {"TranscodingUrl"});
    bool direct = json::boolean(ms, {"SupportsDirectPlay"}) || json::boolean(ms, {"SupportsDirectStream"});
    std::string method;
    if (direct && transcode.empty()) {
        src.url = util::fmt("%s/Videos/%s/stream?static=true&mediaSourceId=%s&deviceId=%s&api_key=%s&PlaySessionId=%s",
                            a.server.c_str(), it.id.c_str(), msid.c_str(), device_id().c_str(), a.token.c_str(),
                            psid.c_str());
        method = "DirectPlay";
    } else if (!transcode.empty()) {
        src.url = a.server + transcode;
        method = "Transcode";
    } else {
        error = tr("The server can't stream this file");
        return false;
    }
    log_message(LOG_OK, "Jellyfin", "%s via %s", it.name.c_str(), method.c_str());

    // Text subtitles are fetched as SRT (the server converts).
    json_t* streams = json_object_get(ms, "MediaStreams");
    int def_sub = (int)json::num(ms, {"DefaultSubtitleStreamIndex"}, -1);
    for (size_t i = 0; i < json::size(streams); i++) {
        json_t* st = json_array_get(streams, i);
        if (json::str(st, {"Type"}) != "Subtitle" || !json::boolean(st, {"IsTextSubtitleStream"})) continue;
        int idx = (int)json::num(st, {"Index"});
        std::string label = json::str(st, {"DisplayTitle"}, json::str(st, {"Language"}, tr("Subtitles")));
        std::string url = util::fmt("%s/Videos/%s/%s/Subtitles/%d/0/Stream.srt?api_key=%s", a.server.c_str(),
                                    it.id.c_str(), msid.c_str(), idx, a.token.c_str());
        auto entry = std::make_pair(label, url);
        if (idx == def_sub) src.external_subs.insert(src.external_subs.begin(), entry);
        else src.external_subs.push_back(entry);
    }

    std::string report = util::fmt(R"({"ItemId":"%s","MediaSourceId":"%s","PlaySessionId":"%s","PlayMethod":"%s","CanSeek":true,"PositionTicks":%lld})",
                                   it.id.c_str(), msid.c_str(), psid.c_str(), method.c_str(), (long long)(start * 1e7));
    request(a.server, a.token, "POST", "/Sessions/Playing", report);

    std::string id = it.id;
    src.on_progress = [id, msid, psid, method](double pos, bool paused) {
        post_async("/Sessions/Playing/Progress",
                   util::fmt(R"({"ItemId":"%s","MediaSourceId":"%s","PlaySessionId":"%s","PlayMethod":"%s","PositionTicks":%lld,"IsPaused":%s,"CanSeek":true})",
                             id.c_str(), msid.c_str(), psid.c_str(), method.c_str(), (long long)(pos * 1e7),
                             paused ? "true" : "false"));
    };
    src.on_stop = [id, msid, psid, method](double pos, bool) {
        post_async("/Sessions/Playing/Stopped",
                   util::fmt(R"({"ItemId":"%s","MediaSourceId":"%s","PlaySessionId":"%s","PositionTicks":%lld})",
                             id.c_str(), msid.c_str(), psid.c_str(), (long long)(pos * 1e7)));
        if (method == "Transcode") {
            Account acc = snapshot();
            std::string path = "/Videos/ActiveEncodings?deviceId=" + device_id() + "&playSessionId=" + psid;
            tasks::submit(tasks::API, [acc, path]() -> std::function<void()> {
                request(acc.server, acc.token, "DELETE", path);
                return nullptr;
            });
        }
    };
    return true;
}

}  // namespace

const Account& account() {
    std::lock_guard<std::mutex> lk(g_m);
    load_account();
    return g_acc;
}

void sign_out() {
    Account a = snapshot();
    if (a.valid()) request(a.server, a.token, "POST", "/Sessions/Logout");
    std::lock_guard<std::mutex> lk(g_m);
    g_acc = Account();
    g_acc.server = a.server;  // keep the address for convenience
    for (const char* k : {"jf_token", "jf_user_id", "jf_user_name"}) store::set_str(k, "");
}

std::string normalize_url(const std::string& input) {
    std::string u = util::trim(input);
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.empty()) return u;
    if (!util::starts_with(u, "http://") && !util::starts_with(u, "https://")) {
        // Bare host: assume the default Jellyfin port on the local network.
        u = "http://" + u;
        size_t host_start = 7;
        if (u.find(':', host_start) == std::string::npos) {
            size_t slash = u.find('/', host_start);
            u.insert(slash == std::string::npos ? u.size() : slash, ":8096");
        }
    }
    return u;
}

ServerInfo server_info(const std::string& url) {
    ServerInfo info;
    http::Response r = request(url, "", "GET", "/System/Info/Public");
    if (!r.ok()) {
        info.error = r.error;
        return info;
    }
    json::Doc doc = json::Doc::parse(r.body);
    info.name = json::str(doc.get(), {"ServerName"});
    info.version = json::str(doc.get(), {"Version"});
    info.ok = !json::str(doc.get(), {"Id"}).empty();
    if (!info.ok) info.error = tr("That doesn't look like a Jellyfin server");
    return info;
}

AuthResult sign_in(const std::string& url, const std::string& user, const std::string& password) {
    json_t* body = json_object();
    json_object_set_new(body, "Username", json_string(user.c_str()));
    json_object_set_new(body, "Pw", json_string(password.c_str()));
    std::string payload = json::dump(body);
    json_decref(body);
    return finish_auth(url, request(url, "", "POST", "/Users/AuthenticateByName", payload));
}

QuickConnect quick_connect_start(const std::string& url) {
    QuickConnect q;
    http::Response r = request(url, "", "POST", "/QuickConnect/Initiate");
    if (r.status == 404 || r.status == 405) r = request(url, "", "GET", "/QuickConnect/Initiate");
    if (!r.ok()) {
        q.error = r.status == 401 || r.status == 403 ? tr("Quick Connect is turned off on this server") : r.error;
        return q;
    }
    json::Doc doc = json::Doc::parse(r.body);
    q.code = json::str(doc.get(), {"Code"});
    q.secret = json::str(doc.get(), {"Secret"});
    q.ok = !q.code.empty() && !q.secret.empty();
    if (!q.ok) q.error = tr("Quick Connect isn't available");
    return q;
}

int quick_connect_poll(const std::string& url, const std::string& secret) {
    http::Response r = request(url, "", "GET", "/QuickConnect/Connect?Secret=" + util::url_encode(secret));
    if (r.status == 404) return -1;
    if (!r.ok()) return 0;
    json::Doc doc = json::Doc::parse(r.body);
    return json::boolean(doc.get(), {"Authenticated"}) ? 1 : 0;
}

AuthResult quick_connect_finish(const std::string& url, const std::string& secret) {
    std::string body = "{\"Secret\":\"" + secret + "\"}";
    return finish_auth(url, request(url, "", "POST", "/Users/AuthenticateWithQuickConnect", body));
}

List views() {
    Account a = snapshot();
    return parse_list(get_with_fallback("/UserViews?userId=" + a.user_id, "/Users/" + a.user_id + "/Views"));
}

List resume() {
    Account a = snapshot();
    std::string q = std::string("?mediaTypes=Video&limit=20&") + FIELDS;
    return parse_list(get_with_fallback("/UserItems/Resume" + q + "&userId=" + a.user_id,
                                        "/Users/" + a.user_id + "/Items/Resume" + q));
}

List next_up(const std::string& series_id) {
    Account a = snapshot();
    std::string path = "/Shows/NextUp?userId=" + a.user_id + "&limit=20&" + FIELDS;
    if (!series_id.empty()) path += "&seriesId=" + series_id;
    return parse_list(api("GET", path));
}

List latest(const std::string& parent_id) {
    Account a = snapshot();
    std::string q = "?parentId=" + parent_id + "&limit=24&" + FIELDS;
    return parse_list(get_with_fallback("/Items/Latest" + q + "&userId=" + a.user_id,
                                        "/Users/" + a.user_id + "/Items/Latest" + q));
}

List items(const std::string& parent_id, const std::string& types, const std::string& sort, int start, int limit,
           bool recursive) {
    Account a = snapshot();
    std::string q = util::fmt("?parentId=%s&startIndex=%d&limit=%d&recursive=%s&sortBy=%s&sortOrder=%s&%s",
                              parent_id.c_str(), start, limit, recursive ? "true" : "false",
                              sort.empty() ? "SortName" : sort.c_str(),
                              sort == "DateCreated" || sort == "PremiereDate" ? "Descending" : "Ascending", FIELDS);
    if (!types.empty()) q += "&includeItemTypes=" + types;
    return parse_list(get_with_fallback("/Items" + q + "&userId=" + a.user_id, "/Users/" + a.user_id + "/Items" + q));
}

List seasons(const std::string& series_id) {
    Account a = snapshot();
    return parse_list(api("GET", "/Shows/" + series_id + "/Seasons?userId=" + a.user_id + "&" + FIELDS));
}

List episodes(const std::string& series_id, const std::string& season_id) {
    Account a = snapshot();
    return parse_list(api("GET", "/Shows/" + series_id + "/Episodes?userId=" + a.user_id + "&seasonId=" + season_id +
                                     "&" + FIELDS));
}

List children(const std::string& parent_id) { return items(parent_id, "", "SortName", 0, 500, false); }

List similar(const std::string& id) {
    Account a = snapshot();
    return parse_list(api("GET", "/Items/" + id + "/Similar?userId=" + a.user_id + "&limit=16&" + FIELDS));
}

List search(const std::string& term) {
    Account a = snapshot();
    std::string q = "?searchTerm=" + util::url_encode(term) +
                    "&recursive=true&limit=40&includeItemTypes=Movie,Series,Episode,MusicAlbum,Audio&" + FIELDS;
    return parse_list(get_with_fallback("/Items" + q + "&userId=" + a.user_id, "/Users/" + a.user_id + "/Items" + q));
}

bool item(const std::string& id, Item& out, std::string& error) {
    Account a = snapshot();
    http::Response r = get_with_fallback("/Items/" + id + "?userId=" + a.user_id, "/Users/" + a.user_id + "/Items/" + id);
    if (!r.ok()) {
        error = r.error;
        return false;
    }
    json::Doc doc = json::Doc::parse(r.body);
    if (!doc) {
        error = tr("Unexpected response from the server");
        return false;
    }
    out = parse_item(doc.get());
    return true;
}

void set_favorite(const std::string& id, bool fav) {
    Account a = snapshot();
    std::string path = "/UserFavoriteItems/" + id + "?userId=" + a.user_id;
    std::string legacy = "/Users/" + a.user_id + "/FavoriteItems/" + id;
    tasks::submit(tasks::API, [a, path, legacy, fav]() -> std::function<void()> {
        http::Response r = request(a.server, a.token, fav ? "POST" : "DELETE", path);
        if (r.status == 404 || r.status == 405) request(a.server, a.token, fav ? "POST" : "DELETE", legacy);
        return nullptr;
    });
}

void set_played(const std::string& id, bool played) {
    Account a = snapshot();
    std::string path = "/UserPlayedItems/" + id + "?userId=" + a.user_id;
    std::string legacy = "/Users/" + a.user_id + "/PlayedItems/" + id;
    tasks::submit(tasks::API, [a, path, legacy, played]() -> std::function<void()> {
        http::Response r = request(a.server, a.token, played ? "POST" : "DELETE", path);
        if (r.status == 404 || r.status == 405) request(a.server, a.token, played ? "POST" : "DELETE", legacy);
        return nullptr;
    });
}

std::string poster(const Item& it, int max_w) {
    if (!it.primary_tag.empty() && it.type != "Episode") return img(it.id, "Primary", it.primary_tag, max_w);
    if (!it.series_primary_tag.empty() && !it.series_id.empty()) return img(it.series_id, "Primary", it.series_primary_tag, max_w);
    if (!it.album_primary_tag.empty() && !it.album_id.empty()) return img(it.album_id, "Primary", it.album_primary_tag, max_w);
    if (!it.primary_tag.empty()) return img(it.id, "Primary", it.primary_tag, max_w);
    return "";
}

std::string backdrop(const Item& it, int max_w) {
    if (!it.backdrop_tag.empty()) return img(it.id, "Backdrop/0", it.backdrop_tag, max_w);
    if (!it.parent_backdrop_id.empty() && !it.parent_backdrop_tag.empty())
        return img(it.parent_backdrop_id, "Backdrop/0", it.parent_backdrop_tag, max_w);
    return "";
}

std::string thumb(const Item& it, int max_w) {
    if (it.type == "Episode" && !it.primary_tag.empty()) return img(it.id, "Primary", it.primary_tag, max_w);
    if (!it.thumb_tag.empty()) return img(it.id, "Thumb", it.thumb_tag, max_w);
    if (!it.parent_thumb_id.empty() && !it.parent_thumb_tag.empty())
        return img(it.parent_thumb_id, "Thumb", it.parent_thumb_tag, max_w);
    std::string b = backdrop(it, max_w);
    return b.empty() ? poster(it, max_w) : b;
}

player::Source make_source(const Item& it, bool from_start) {
    player::Source s;
    s.title = it.type == "Episode" ? it.name : it.name;
    if (it.type == "Episode")
        s.subtitle = util::fmt(tr("%s \xC2\xB7 S%d E%d"), it.series_name.c_str(), it.parent_index, it.index);
    else if (it.is_audio())
        s.subtitle = it.album_artist;
    else
        s.subtitle = it.year;
    s.artwork = it.is_audio() ? poster(it, 600) : backdrop(it, 1280);
    if (s.artwork.empty()) s.artwork = poster(it, 600);
    s.service = "jellyfin";
    s.id = it.id;
    s.extra = it.type;
    s.remember_position = false;  // the server remembers
    s.start = from_start ? 0 : it.position;
    if (it.is_audio()) {
        Account a = snapshot();
        s.url = util::fmt("%s/Audio/%s/universal?UserId=%s&DeviceId=%s&MaxStreamingBitrate=320000&"
                          "Container=mp3,aac,m4a,flac,ogg,opus,wav,webma&TranscodingContainer=mp3&TranscodingProtocol=http&"
                          "AudioCodec=mp3&api_key=%s",
                          a.server.c_str(), it.id.c_str(), a.user_id.c_str(), device_id().c_str(), a.token.c_str());
        return s;
    }
    Item copy = it;
    double start = s.start;
    s.resolve = [copy, start](player::Source& src, std::string& err) { return resolve_video(copy, start, src, err); };
    return s;
}

}  // namespace jellyfin
