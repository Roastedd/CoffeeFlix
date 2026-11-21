#include "services/spotify/spotify.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/spotify/spotify_ap.hpp"
#include "services/spotify/spotify_crypto.hpp"
#include "services/spotify/spotify_index.hpp"
#include "services/spotify/spotify_internal.hpp"
#include "services/spotify/spotify_proto.hpp"

namespace spotify {

namespace detail {

// The client id Spotify's own desktop app signs in as (librespot's KEYMASTER_CLIENT_ID): the device login and
// login5 accept it, and its tokens open the access point and the web services.
constexpr char CLIENT_ID[] = "65b708073fc0480ea92a077233ca87bd";
constexpr uint8_t AUTH_STORED = 1, AUTH_TOKEN = 3;

std::string hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) s += d[c >> 4], s += d[c & 15];
    return s;
}

std::vector<uint8_t> unhex(const std::string& s) {
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::vector<uint8_t> b;
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((uint8_t)(v(s[i]) << 4 | v(s[i + 1])));
    return b;
}

// A Spotify id (22 characters, base 62) as the 16 bytes the services want.
std::vector<uint8_t> id_to_gid(const std::string& id) {
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::vector<uint8_t> n(16, 0);
    for (char c : id) {
        const char* at = strchr(digits, c);
        if (!c || !at) return {};
        unsigned carry = (unsigned)(at - digits);
        for (int i = 15; i >= 0; i--) {
            unsigned v = n[i] * 62u + carry;
            n[i] = (uint8_t)v;
            carry = v >> 8;
        }
        if (carry) return {};
    }
    return n;
}

std::string gid_to_id(const std::vector<uint8_t>& gid) {
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::vector<uint8_t> n(gid);
    std::string out(22, '0');
    for (int pos = 21; pos >= 0; pos--) {  // divide by 62, the remainder is the next digit from the end
        unsigned rem = 0;
        for (uint8_t& b : n) {
            unsigned v = rem << 8 | b;
            b = (uint8_t)(v / 62);
            rem = v % 62;
        }
        out[pos] = digits[rem];
    }
    return out;
}

std::string device_id() {
    std::string id = store::get_str("sp_device_id");
    if (id.size() != 40) {
        id = util::secure_random_hex(20);
        store::set_str("sp_device_id", id);
    }
    return id;
}

Credentials stored_credentials() {
    Credentials c;
    c.username = store::get_str("sp_user");
    c.auth_data = unhex(store::get_str("sp_auth"));
    c.auth_type = (uint8_t)store::get_int("sp_auth_type", AUTH_STORED);
    c.device_id = device_id();
    return c;
}

// --- the access point: one connection, opened when something needs it ---------------------------

std::mutex g_ap_mu;  // opening and closing it
std::unique_ptr<AP> g_ap;

struct KeyWait {
    bool done = false;
    std::vector<uint8_t> key;
    int error = 0;
};
std::mutex g_key_mu;
std::condition_variable g_key_cv;
std::map<uint32_t, KeyWait> g_keys;
uint32_t g_key_seq = 0;

void on_packet(uint8_t cmd, std::vector<uint8_t> payload) {
    if ((cmd != Cmd::AesKey && cmd != Cmd::AesKeyError) || payload.size() < 4) return;
    uint32_t seq = (uint32_t)payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
    std::lock_guard<std::mutex> lk(g_key_mu);
    auto it = g_keys.find(seq);
    if (it == g_keys.end()) return;
    if (cmd == Cmd::AesKey && payload.size() >= 20)
        it->second.key.assign(payload.begin() + 4, payload.begin() + 20);
    else
        it->second.error = payload.size() >= 6 ? payload[4] << 8 | payload[5] : -1;
    it->second.done = true;
    g_key_cv.notify_all();
}

// Signs in on the access point with `creds` (under g_ap_mu).
bool open_ap(const Credentials& creds, std::string& error) {
    if (g_ap && g_ap->is_connected()) return true;
    g_ap.reset();
    auto ap = std::make_unique<AP>();
    AP::Callbacks cb;
    cb.on_packet = on_packet;
    cb.on_disconnect = [] { log_message(LOG_WARNING, "Spotify", "The access point closed the connection"); };
    if (!ap->connect(creds, std::move(cb))) {
        error = tr("Couldn't sign in to Spotify");
        return false;
    }
    log_message(LOG_OK, "Spotify", "Signed in on the access point");
    g_ap = std::move(ap);
    return true;
}

bool ensure_ap(std::string& error) {
    if (!signed_in()) {
        error = tr("Not signed in");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_ap_mu);
    return open_ap(stored_credentials(), error);
}

// --- tokens for the web services (login5) ---------------------------------------------------------

std::mutex g_token_mu;
std::string g_token;
double g_token_until = 0;

http::Response post_proto(const std::string& url, const std::vector<uint8_t>& body) {
    http::Request r;
    r.method = "POST";
    r.url = url;
    r.body.assign(body.begin(), body.end());
    r.headers = {{"Content-Type", "application/x-protobuf"}, {"Accept", "application/x-protobuf"}};
    r.timeout = 15;
    return http::perform(r);
}

// login5's proof of work: a suffix whose SHA-1 together with `prefix` ends in `bits` zero bits.
std::vector<uint8_t> solve_hashcash(const std::vector<uint8_t>& ctx, const std::vector<uint8_t>& prefix, int bits) {
    uint8_t md[20];
    crypto::sha1({{ctx.data(), ctx.size()}}, md);
    uint64_t target = 0;
    for (int i = 12; i < 20; i++) target = target << 8 | md[i];
    uint8_t suffix[16];
    for (uint64_t ctr = 0; ctr < 10'000'000; ctr++) {
        uint64_t v1 = target + ctr, v2 = ctr;
        for (int i = 7; i >= 0; i--) {
            suffix[i] = (uint8_t)v1, v1 >>= 8;
            suffix[8 + i] = (uint8_t)v2, v2 >>= 8;
        }
        uint8_t h[20];
        crypto::sha1({{prefix.data(), prefix.size()}, {suffix, 16}}, h);
        uint64_t hv = 0;
        for (int i = 12; i < 20; i++) hv = hv << 8 | h[i];
        if ((hv == 0 ? 64 : __builtin_ctzll(hv)) >= bits) return {suffix, suffix + 16};
    }
    return {};
}

bool login5(std::string& token, int& expires_in, std::string& error) {
    const Credentials creds = stored_credentials();
    std::vector<uint8_t> client;
    pb_str(client, 1, CLIENT_ID);
    pb_str(client, 2, creds.device_id);
    std::vector<uint8_t> stored;
    pb_str(stored, 1, creds.username);
    pb_bytes(stored, 2, creds.auth_data.data(), creds.auth_data.size());

    std::vector<uint8_t> login_ctx, solution;
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<uint8_t> req;
        pb_msg(req, 1, client);
        if (!login_ctx.empty()) pb_bytes(req, 2, login_ctx.data(), login_ctx.size());
        if (!solution.empty()) {
            std::vector<uint8_t> dur, hc, ch, chs;
            pb_u32(dur, 1, 0);
            pb_u32(dur, 2, 0);
            pb_bytes(hc, 1, solution.data(), solution.size());
            pb_msg(hc, 2, dur);
            pb_msg(ch, 1, hc);
            pb_msg(chs, 1, ch);
            pb_msg(req, 3, chs);
        }
        pb_msg(req, 100, stored);
        http::Response r = post_proto("https://login5.spotify.com/v3/login", req);
        if (!r.ok()) {
            error = r.error.empty() ? util::fmt("HTTP %ld", r.status) : r.error;
            return false;
        }
        PRd rd = reader(r.body);
        int bits = 0, login_error = 0;
        std::vector<uint8_t> prefix;
        bool challenged = false;
        uint32_t f;
        uint8_t w;
        while (rd.next(f, w)) {
            if (f == 1 && w == 2) {  // ok
                PRd ok;
                rd.enter(ok);
                uint32_t of;
                uint8_t ow;
                uint64_t v;
                while (ok.next(of, ow)) {
                    if (of == 2 && ow == 2) ok.read_str(token);
                    else if (of == 4 && ow == 0 && ok.vi(v)) expires_in = (int)v;
                    else ok.skip(ow);
                }
            } else if (f == 2 && w == 0) {
                uint64_t v;
                rd.vi(v);
                login_error = (int)v;
            } else if (f == 3 && w == 2) {  // challenges > challenge > hashcash
                PRd chs;
                rd.enter(chs);
                uint32_t cf;
                uint8_t cw;
                while (chs.next(cf, cw)) {
                    if (cf != 1 || cw != 2) { chs.skip(cw); continue; }
                    PRd c;
                    chs.enter(c);
                    while (c.next(cf, cw)) {
                        if (cf != 1 || cw != 2) { c.skip(cw); continue; }
                        PRd hc;
                        c.enter(hc);
                        uint64_t v;
                        while (hc.next(cf, cw)) {
                            if (cf == 1 && cw == 0 && hc.vi(v)) bits = (int)v;
                            else if (cf == 2 && cw == 2) hc.read_bytes(prefix);
                            else hc.skip(cw);
                        }
                        challenged = true;
                    }
                }
            } else if (f == 5 && w == 2) {
                rd.read_bytes(login_ctx);
            } else {
                rd.skip(w);
            }
        }
        if (!token.empty()) return true;
        if (login_error || !challenged) {
            error = util::fmt(tr("Spotify refused the sign-in (%d)"), login_error);
            return false;
        }
        solution = solve_hashcash(login_ctx, prefix, bits);
        if (solution.empty()) break;
    }
    error = tr("Couldn't sign in to Spotify");
    return false;
}

// --- spclient: metadata and file locations --------------------------------------------------------

std::mutex g_host_mu;
std::string g_spclient;

std::string spclient() {
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        if (!g_spclient.empty()) return g_spclient;
    }
    std::string host = "gew4-spclient.spotify.com";
    http::Response r = http::get("https://apresolve.spotify.com/?type=spclient", {}, 8);
    if (r.ok()) {
        json::Doc d = json::Doc::parse(r.body);
        std::string first = json::str(json_array_get(json::at(d.get(), {"spclient"}), 0));
        if (!first.empty()) host = first.substr(0, first.rfind(':'));
    }
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_spclient = host;
}

// A client token (clienttoken.spotify.com), which many of spclient's services want besides the login's: what the
// apps get for the install they are. librespot's way, as a Linux desktop app.
std::mutex g_ct_mu;
std::string g_client_token;
double g_client_token_until = 0;

std::string client_token() {
    std::lock_guard<std::mutex> lk(g_ct_mu);
    if (!g_client_token.empty() && util::now_seconds() < g_client_token_until) return g_client_token;
    std::vector<uint8_t> linux_data, platform, sdk, data, req;
    pb_str(linux_data, 1, "Linux");
    pb_str(linux_data, 2, "6.1.0");
    pb_str(linux_data, 3, "#1 SMP");
    pb_str(linux_data, 4, "x86_64");
    pb_msg(platform, 5, linux_data);  // desktop_linux
    pb_msg(sdk, 1, platform);
    pb_str(sdk, 2, device_id());
    pb_str(data, 1, "1.2.52.442.g01893f42");
    pb_str(data, 2, CLIENT_ID);
    pb_msg(data, 3, sdk);
    pb_u32(req, 1, 1);  // REQUEST_CLIENT_DATA_REQUEST
    pb_msg(req, 2, data);
    for (int attempt = 0; attempt < 3; attempt++) {
        http::Response r = post_proto("https://clienttoken.spotify.com/v1/clienttoken", req);
        if (!r.ok()) {
            log_message(LOG_WARNING, "Spotify", "Client token: HTTP %ld %s", r.status, r.error.c_str());
            return "";
        }
        // ClientTokenResponse: granted_token (2) { token 1, refresh_after_seconds 3 } or challenges (3)
        std::string token, state, prefix_hex;
        uint64_t refresh = 1200, length = 0;
        PRd rd = reader(r.body);
        uint32_t f;
        uint8_t w;
        while (rd.next(f, w)) {
            if (f == 2 && w == 2) {
                PRd g;
                rd.enter(g);
                while (g.next(f, w)) {
                    if (f == 1 && w == 2) g.read_str(token);
                    else if (f == 3 && w == 0) g.vi(refresh);
                    else g.skip(w);
                }
            } else if (f == 3 && w == 2) {
                PRd c;
                rd.enter(c);
                while (c.next(f, w)) {
                    if (f == 1 && w == 2) {
                        c.read_str(state);
                    } else if (f == 2 && w == 2) {  // a challenge: hashcash parameters (3) { length 1, prefix 2 }
                        PRd ch;
                        c.enter(ch);
                        while (ch.next(f, w)) {
                            if (f != 3 || w != 2) { ch.skip(w); continue; }
                            PRd hc;
                            ch.enter(hc);
                            while (hc.next(f, w)) {
                                if (f == 1 && w == 0) hc.vi(length);
                                else if (f == 2 && w == 2) hc.read_str(prefix_hex);
                                else hc.skip(w);
                            }
                        }
                    } else {
                        c.skip(w);
                    }
                }
            } else {
                rd.skip(w);
            }
        }
        if (!token.empty()) {
            g_client_token = token;
            g_client_token_until = util::now_seconds() + std::max<uint64_t>(60, refresh);
            log_message(LOG_OK, "Spotify", "Got a client token%s", attempt ? " (after a challenge)" : "");
            return token;
        }
        if (state.empty() || prefix_hex.empty()) break;
        const std::vector<uint8_t> suffix = solve_hashcash({}, unhex(prefix_hex), (int)length);
        if (suffix.empty()) break;
        std::string suffix_hex = hex(suffix);
        for (char& c : suffix_hex) c = (char)toupper((unsigned char)c);
        std::vector<uint8_t> hc, answer, answers;
        pb_str(hc, 1, suffix_hex);
        pb_u32(answer, 1, 1);  // CHALLENGE_HASH_CASH
        pb_msg(answer, 3, hc);
        pb_str(answers, 1, state);
        pb_msg(answers, 2, answer);
        req.clear();
        pb_u32(req, 1, 2);  // REQUEST_CHALLENGE_ANSWERS_REQUEST
        pb_msg(req, 3, answers);
    }
    log_message(LOG_WARNING, "Spotify", "No client token");
    return "";
}

http::Response sp_get(const std::string& path, const std::atomic<bool>* cancel, std::string& error,
                      const std::vector<uint8_t>* post, const char* content_type) {
    std::string token = access_token(error);
    if (token.empty()) return {};
    http::Request r;
    r.url = "https://" + spclient() + path;
    r.headers = {{"Authorization", "Bearer " + token}, {"Accept", "application/x-protobuf"}};
    const std::string ct = client_token();
    if (!ct.empty()) r.headers.emplace_back("client-token", ct);
    if (post) {
        r.method = "POST";
        r.body.assign(post->begin(), post->end());
        r.headers.emplace_back("Content-Type", content_type);
        r.headers[1].second = content_type;  // Accept: the same (collection wants its own type both ways)
    }
    r.timeout = 15;
    r.cancel = cancel;
    http::Response res = http::perform(r);
    if (!res.ok()) error = res.error.empty() ? util::fmt("HTTP %ld", res.status) : res.error;
    return res;
}

struct TrackMeta {
    std::string title, artist, album, artwork;
    std::vector<uint8_t> file_id, gid;  // the file to play, and the track (or alternative) it belongs to
    int64_t duration_ms = 0;
};

// The best file of a Track (or one of its alternatives): Ogg Vorbis, 320 kbit/s first.
void pick_file(PRd file, int& best, std::vector<uint8_t>& out) {
    std::vector<uint8_t> fid;
    uint64_t fmt = 255;
    uint32_t f;
    uint8_t w;
    while (file.next(f, w)) {
        if (f == 1 && w == 2) file.read_bytes(fid);
        else if (f == 2 && w == 0) file.vi(fmt);
        else file.skip(w);
    }
    // OGG_VORBIS_96 = 0, _160 = 1, _320 = 2; then MP3_256 = 3, _320 = 4, _160 = 5, _96 = 6 (podcasts)
    static const int RANK[] = {4, 5, 6, 3, 3, 2, 1};
    const int rank = fmt < 7 ? RANK[fmt] : 0;
    if (rank > best && fid.size() == 20) best = rank, out = fid;
}

bool parse_track(const std::string& body, const std::vector<uint8_t>& gid, TrackMeta& m) {
    PRd rd = reader(body);
    int best = 0;
    uint32_t f;
    uint8_t w;
    m.gid = gid;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(m.title);
        } else if (f == 7 && w == 0) {
            uint64_t v;
            rd.vi(v);
            m.duration_ms = (int64_t)(int32_t)((v >> 1) ^ (~(v & 1) + 1));
        } else if (f == 3 && w == 2) {  // album: name, cover group
            PRd alb;
            rd.enter(alb);
            uint64_t best_size = 0;
            while (alb.next(f, w)) {
                if (f == 2 && w == 2) {
                    alb.read_str(m.album);
                } else if (f == 17 && w == 2) {
                    PRd group;
                    alb.enter(group);
                    while (group.next(f, w)) {
                        if (f != 1 || w != 2) { group.skip(w); continue; }
                        PRd img;
                        group.enter(img);
                        std::vector<uint8_t> id;
                        uint64_t size = 0;
                        while (img.next(f, w)) {
                            if (f == 1 && w == 2) img.read_bytes(id);
                            else if (f == 2 && w == 0) img.vi(size);
                            else img.skip(w);
                        }
                        // DEFAULT 0 (300 px), SMALL 1 (64), LARGE 2 (640), XLARGE 3: 640 is plenty
                        const uint64_t rank = size == 2 ? 4 : size == 0 ? 3 : size == 3 ? 2 : 1;
                        if (!id.empty() && rank > best_size) best_size = rank, m.artwork = "https://i.scdn.co/image/" + hex(id);
                    }
                } else {
                    alb.skip(w);
                }
            }
        } else if (f == 4 && w == 2) {
            PRd art;
            rd.enter(art);
            std::string name;
            while (art.next(f, w)) {
                if (f == 2 && w == 2) art.read_str(name);
                else art.skip(w);
            }
            if (!name.empty()) m.artist += (m.artist.empty() ? "" : ", ") + name;
        } else if (f == 12 && w == 2) {
            PRd file;
            rd.enter(file);
            pick_file(file, best, m.file_id);
        } else if (f == 13 && w == 2 && m.file_id.empty()) {  // alternatives, for a track not playable as it is
            PRd alt;
            rd.enter(alt);
            std::vector<uint8_t> alt_gid, alt_file;
            int alt_best = 0;
            while (alt.next(f, w)) {
                if (f == 1 && w == 2) {
                    alt.read_bytes(alt_gid);
                } else if (f == 12 && w == 2) {
                    PRd file;
                    alt.enter(file);
                    pick_file(file, alt_best, alt_file);
                } else {
                    alt.skip(w);
                }
            }
            if (alt_best > best && alt_gid.size() == 16) best = alt_best, m.file_id = alt_file, m.gid = alt_gid;
        } else {
            rd.skip(w);
        }
    }
    return rd.ok && !m.file_id.empty();
}

// An Episode: name, show, cover, its files, or a link to the file elsewhere (a podcast's own feed).
bool parse_episode(const std::string& body, const std::vector<uint8_t>& gid, TrackMeta& m, std::string& external) {
    PRd rd = reader(body);
    int best = 0;
    uint32_t f;
    uint8_t w;
    m.gid = gid;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) {
            rd.read_str(m.title);
        } else if (f == 7 && w == 0) {
            uint64_t v;
            rd.vi(v);
            m.duration_ms = (int64_t)(int32_t)((v >> 1) ^ (~(v & 1) + 1));
        } else if (f == 12 && w == 2) {
            PRd file;
            rd.enter(file);
            pick_file(file, best, m.file_id);
        } else if (f == 71 && w == 2) {  // the show: its name
            PRd show;
            rd.enter(show);
            while (show.next(f, w))
                if (f == 2 && w == 2) show.read_str(m.artist);
                else show.skip(w);
        } else if (f == 68 && w == 2) {  // cover image group
            PRd group;
            rd.enter(group);
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
                if (!id.empty() && (m.artwork.empty() || size == 2)) m.artwork = "https://i.scdn.co/image/" + hex(id);
            }
        } else if (f == 83 && w == 2) {
            rd.read_str(external);
        } else {
            rd.skip(w);
        }
    }
    return rd.ok && (!m.file_id.empty() || !external.empty());
}

std::string cdn_url(const std::vector<uint8_t>& file_id, const std::atomic<bool>* cancel, std::string& error) {
    http::Response r = sp_get("/storage-resolve/files/audio/interactive/" + hex(file_id), cancel, error);
    if (!r.ok()) return "";
    PRd rd = reader(r.body);
    std::string url;
    uint32_t f;
    uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2 && url.empty()) rd.read_str(url);  // the first of the CDN's addresses
        else rd.skip(w);
    }
    if (url.empty()) error = tr("Spotify has no file for this song");
    return url;
}

// What the services know of an entity ("spotify:track:..."), one kind of it (ExtensionKind: TRACK_V4 10,
// EPISODE_V4 11): its message (a Track, an Episode), from extended-metadata (librespot's way since 2025).
bool extended_metadata(const std::string& uri, int kind, const std::atomic<bool>* cancel, std::string& body,
                       std::string& error) {
    std::vector<uint8_t> query, entity, req;  // no request header: country and catalogue come from the token
    pb_u32(query, 1, (uint32_t)kind);
    pb_str(entity, 1, uri);
    pb_msg(entity, 2, query);
    pb_msg(req, 2, entity);
    http::Response r = sp_get("/extended-metadata/v0/extended-metadata", cancel, error, &req);
    if (!r.ok()) {
        log_message(LOG_WARNING, "Spotify", "extended-metadata: %s", error.c_str());
        return false;
    }
    // BatchedExtensionResponse > extended_metadata (2) > extension_data (3) > extension_data (3, an Any) > value (2)
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
            while (data.next(f, w)) {
                if (f != 3 || w != 2) { data.skip(w); continue; }
                PRd any;
                data.enter(any);
                while (any.next(f, w)) {
                    if (f == 2 && w == 2) return any.read_str(body) && !body.empty();
                    any.skip(w);
                }
            }
        }
    }
    log_message(LOG_WARNING, "Spotify", "No metadata for %s (kind %d)", uri.c_str(), kind);
    error = tr("Spotify has no file for this song");
    return false;
}

bool audio_key(const std::vector<uint8_t>& file_id, const std::vector<uint8_t>& gid, const std::atomic<bool>* cancel,
               std::vector<uint8_t>& key, std::string& error) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_ap(error)) return false;
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lk(g_key_mu);
            seq = g_key_seq++;
            g_keys[seq] = {};
        }
        std::vector<uint8_t> pkt(file_id);
        pkt.insert(pkt.end(), gid.begin(), gid.end());
        for (int s = 24; s >= 0; s -= 8) pkt.push_back((uint8_t)(seq >> s));
        pkt.push_back(0), pkt.push_back(0);
        bool sent;
        {
            std::lock_guard<std::mutex> lk(g_ap_mu);
            sent = g_ap && g_ap->send_packet(Cmd::RequestKey, pkt);
        }
        std::unique_lock<std::mutex> lk(g_key_mu);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (sent && !g_keys[seq].done && !(cancel && *cancel) && std::chrono::steady_clock::now() < until)
            g_key_cv.wait_for(lk, std::chrono::milliseconds(100));
        KeyWait got = g_keys[seq];
        g_keys.erase(seq);
        lk.unlock();
        if (got.key.size() == 16) {
            key = got.key;
            return true;
        }
        if (cancel && *cancel) {
            error = "Cancelled";
            return false;
        }
        if (got.done) {
            log_message(LOG_WARNING, "Spotify", "No key for the file (error %04x)", got.error);
            error = tr("Spotify won't play this song here");
            return false;
        }
        // No answer: the connection may have died quietly. Once more on a new one.
        log_message(LOG_WARNING, "Spotify", "No answer for a key: reconnecting");
        std::lock_guard<std::mutex> ap(g_ap_mu);
        g_ap.reset();
    }
    error = tr("Spotify didn't answer");
    return false;
}

}  // namespace detail
using namespace detail;

// --- sign-in ----------------------------------------------------------------------------------------

bool signed_in() { return !store::get_str("sp_auth").empty(); }
std::string user_name() { return store::get_str("sp_user"); }

void sign_out() {
    forget_library();
    index::forget();
    {
        std::lock_guard<std::mutex> lk(g_ap_mu);
        g_ap.reset();
    }
    {
        std::lock_guard<std::mutex> lk(g_token_mu);
        g_token.clear();
        g_token_until = 0;
    }
    store::set_str("sp_user", "");
    store::set_str("sp_auth", "");
    store::set_int("sp_auth_type", AUTH_STORED);
}

namespace {
http::Response post_form(const std::string& url, const std::string& body) {
    http::Request r;
    r.method = "POST";
    r.url = url;
    r.body = body;
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    r.timeout = 15;
    return http::perform(r);
}
}  // namespace

bool begin_login(DeviceLogin& out, std::string& error) {
    http::Response r = post_form("https://accounts.spotify.com/oauth2/device/authorize",
                                 std::string("client_id=") + CLIENT_ID + "&scope=streaming");
    json::Doc d = json::Doc::parse(r.body);
    out.device_code = json::str(d.get(), {"device_code"});
    out.user_code = json::str(d.get(), {"user_code"});
    out.url = json::str(d.get(), {"verification_uri_complete"});
    out.interval = (int)json::num(d.get(), {"interval"}, 5);
    out.expires_at = util::now_seconds() + json::num(d.get(), {"expires_in"}, 300);
    if (out.device_code.empty() || out.url.empty()) {
        const std::string why = json::str(d.get(), {"error_description"}, json::str(d.get(), {"error"}, r.error));
        log_message(LOG_ERROR, "Spotify", "Device login: HTTP %ld %s", r.status, why.c_str());
        error = why.empty() ? tr("Couldn't reach Spotify") : why;
        return false;
    }
    return true;
}

LoginState check_login(DeviceLogin& login, std::string& error) {
    if (util::now_seconds() > login.expires_at) {
        error = tr("The code ran out. Try again.");
        return LoginState::FAILED;
    }
    http::Response r = post_form("https://accounts.spotify.com/api/token",
                                 std::string("client_id=") + CLIENT_ID +
                                     "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=" +
                                     util::url_encode(login.device_code));
    json::Doc d = json::Doc::parse(r.body);
    const std::string token = json::str(d.get(), {"access_token"});
    if (token.empty()) {
        const std::string e = json::str(d.get(), {"error"});
        if (e == "authorization_pending" || (!r.ok() && e.empty() && r.status == 0)) return LoginState::WAITING;
        if (e == "slow_down") {
            login.interval += 5;
            return LoginState::WAITING;
        }
        log_message(LOG_ERROR, "Spotify", "Device login: %s", e.c_str());
        error = e == "access_denied" ? tr("The sign-in was declined") : tr("Couldn't sign in to Spotify");
        return LoginState::FAILED;
    }
    // Into the access point with the token: it answers with a login to keep.
    Credentials creds;
    creds.auth_type = AUTH_TOKEN;
    creds.auth_data.assign(token.begin(), token.end());
    creds.device_id = device_id();
    std::lock_guard<std::mutex> lk(g_ap_mu);
    g_ap.reset();
    if (!open_ap(creds, error)) return LoginState::FAILED;
    if (g_ap->reusable_creds().empty()) {
        error = tr("Couldn't sign in to Spotify");
        g_ap.reset();
        return LoginState::FAILED;
    }
    store::set_str("sp_user", g_ap->canonical_username());
    store::set_str("sp_auth", hex(g_ap->reusable_creds()));
    store::set_int("sp_auth_type", g_ap->reusable_type());
    log_message(LOG_OK, "Spotify", "Signed in (login type %u)", g_ap->reusable_type());
    return LoginState::SIGNED_IN;
}

std::string access_token(std::string& error) {
    std::lock_guard<std::mutex> lk(g_token_mu);
    if (!g_token.empty() && util::now_seconds() < g_token_until) return g_token;
    if (!signed_in()) {
        error = tr("Not signed in");
        return "";
    }
    std::string token;
    int expires = 3600;
    if (!login5(token, expires, error)) {
        log_message(LOG_ERROR, "Spotify", "login5: %s", error.c_str());
        return "";
    }
    g_token = token;
    g_token_until = util::now_seconds() + std::max(60, expires - 120);
    return g_token;
}

player::Source track_source(const std::string& uri, const std::string& title, const std::string& artist,
                            const std::string& artwork) {
    player::Source s;
    s.service = "spotify";
    s.id = uri;
    s.title = title;
    s.subtitle = artist;
    s.artwork = artwork;
    const bool episode = util::starts_with(uri, "spotify:episode:");
    s.remember_position = episode;  // an episode goes on where it was left, here on the console
    s.resolve = [](player::Source& src, std::string& error) {
        const std::vector<uint8_t> gid = id_to_gid(src.id.substr(src.id.rfind(':') + 1));
        if (gid.size() != 16) {
            error = tr("Not a Spotify song");
            return false;
        }
        if (util::starts_with(src.id, "spotify:episode:")) {
            std::string body, external;
            TrackMeta m;
            if (!extended_metadata(src.id, 12, src.resolve_cancel, body, error)) return false;
            if (!parse_episode(body, gid, m, external)) {
                error = tr("Spotify has no file for this episode");
                return false;
            }
            if (src.title.empty()) src.title = m.title;
            if (src.subtitle.empty()) src.subtitle = m.artist;
            if (src.artwork.empty()) src.artwork = m.artwork;
            if (m.file_id.empty()) {  // the podcast's own file
                src.url = external;
                return true;
            }
            src.url = cdn_url(m.file_id, src.resolve_cancel, error);
            return !src.url.empty() && audio_key(m.file_id, m.gid, src.resolve_cancel, src.audio_key, error);
        }
        // From extended metadata: spclient's metadata/4 leaves the files out, and the access point's metadata/3
        // (Mercury) answers 400 (November 2026).
        std::string body;
        if (!extended_metadata(src.id, 10, src.resolve_cancel, body, error)) return false;
        TrackMeta m;
        if (!parse_track(body, gid, m)) {
            error = tr("Spotify has no file for this song");
            return false;
        }
        if (src.title.empty()) src.title = m.title;
        if (src.subtitle.empty()) src.subtitle = m.artist;
        if (src.artwork.empty()) src.artwork = m.artwork;
        src.url = cdn_url(m.file_id, src.resolve_cancel, error);
        if (src.url.empty()) return false;
        if (!audio_key(m.file_id, m.gid, src.resolve_cancel, src.audio_key, error)) return false;
        log_message(LOG_OK, "Spotify", "Playing %s (%lld s)", src.id.c_str(), (long long)(m.duration_ms / 1000));
        return true;
    };
    return s;
}

player::Source source(const Item& item) { return track_source(item.uri, item.name, item.subtitle, item.image); }

}  // namespace spotify
