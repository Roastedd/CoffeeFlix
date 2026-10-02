#include "core/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace http {

namespace {

std::string g_ca_bundle;
std::atomic<bool> g_verify{true};
void (*g_socket_setup)(int fd) = nullptr;

struct Sink {
    std::string* body;
    size_t max;
    const std::atomic<bool>* cancel;
    bool overflow = false;
    // Streaming (Request::on_data)
    const Request* req = nullptr;
    Response* resp = nullptr;
    CURL* curl = nullptr;
    bool stopped = false;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    Sink* s = (Sink*)userdata;
    size_t n = size * nmemb;
    if (s->req && s->req->on_data) {
        curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &s->resp->status);
        if (s->req->on_data(*s->resp, ptr, n)) return n;
        s->stopped = true;
        return 0;
    }
    if (s->body->size() + n > s->max) {
        s->overflow = true;
        return 0;
    }
    s->body->append(ptr, n);
    return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = (std::map<std::string, std::string>*)userdata;
    size_t n = size * nitems;
    std::string line(buffer, n);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = util::lower(util::trim(line.substr(0, colon)));
        (*headers)[key] = util::trim(line.substr(colon + 1));
    }
    return n;
}

int sockopt_cb(void* big_buffers, curl_socket_t fd, curlsocktype) {
    if (g_socket_setup && big_buffers) g_socket_setup((int)fd);
    return CURL_SOCKOPT_OK;
}

// Counts cancel_running() calls: a transfer that began in an earlier round ends.
std::atomic<uint32_t> g_cancel_round{0};

struct Progress {
    const Request* req;
    CURL* curl;
    double started;
    uint32_t round;        // g_cancel_round when the transfer began
    bool gave_up = false;  // keep_going said no, or cancel_running()
};

int progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* p = (Progress*)clientp;
    if (g_cancel_round.load() != p->round) {
        p->gave_up = true;
        return 1;
    }
    if (p->req->cancel && p->req->cancel->load()) return 1;
    if (p->req->keep_going) {
        curl_off_t sent = 0;  // microseconds from the start until the request went out
        curl_easy_getinfo(p->curl, CURLINFO_PRETRANSFER_TIME_T, &sent);
        if (!p->req->keep_going(sent > 0 ? p->started + sent / 1e6 : 0)) {
            p->gave_up = true;
            return 1;
        }
    }
    return 0;
}

// Looked-up addresses and TLS sessions, shared by every handle: a new connection to a server
// another handle talked to lately skips the DNS lookup (which blocks, without a time limit, in
// the Wii U's libcurl) and resumes the TLS session instead of a full handshake, which costs a
// lot on Espresso.
CURLSH* g_share = nullptr;
std::mutex g_share_m[CURL_LOCK_DATA_LAST];

void share_lock(CURL*, curl_lock_data data, curl_lock_access, void*) { g_share_m[data].lock(); }
void share_unlock(CURL*, curl_lock_data data, void*) { g_share_m[data].unlock(); }

// Pool of easy handles so keep-alive connections (and their TLS sessions) are
// reused across requests: a TLS handshake costs a lot on Espresso. (No
// thread_local: the Wii U's RPX format doesn't support TLS relocations.)
std::mutex g_pool_m;
std::vector<CURL*> g_pool;

CURL* acquire_handle() {
    std::lock_guard<std::mutex> lk(g_pool_m);
    if (!g_pool.empty()) {
        CURL* h = g_pool.back();
        g_pool.pop_back();
        return h;
    }
    return curl_easy_init();
}

void release_handle(CURL* h) {
    std::lock_guard<std::mutex> lk(g_pool_m);
    if (g_pool.size() < 8) g_pool.push_back(h);  // images, tasks and the log fetch at once
    else curl_easy_cleanup(h);
}

std::string friendly_error(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST: return tr("No internet connection (can't find the server)");
        case CURLE_COULDNT_CONNECT: return tr("Can't connect to the server");
        case CURLE_OPERATION_TIMEDOUT: return tr("The connection timed out");
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING: return tr("The connection was interrupted");
        case CURLE_PEER_FAILED_VERIFICATION:
            return tr("The server's certificate could not be verified. Check the console's date and time.");
        case CURLE_SSL_CACERT_BADFILE:
            return tr("The app's certificate bundle could not be loaded. Reinstall or update the app.");
        case CURLE_SSL_CONNECT_ERROR:
            return tr("The secure handshake failed. The server may be unavailable or incompatible.");
        default: return curl_easy_strerror(rc);
    }
}

}  // namespace

const char* user_agent() { return "CoffeeFlix/2.0 (Nintendo Wii U)"; }

void init(const std::string& ca_bundle_path, void (*socket_setup)(int fd)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    g_ca_bundle = ca_bundle_path;
    g_socket_setup = socket_setup;
    if (!util::file_exists(g_ca_bundle)) {
        log_message(LOG_WARNING, "HTTP", "CA bundle missing at %s", g_ca_bundle.c_str());
    }
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_pool_m);
        for (CURL* h : g_pool) curl_easy_cleanup(h);
        g_pool.clear();
    }
    // Still in use by a download that didn't stop in time: left to the process's end.
    if (g_share && curl_share_cleanup(g_share) == CURLSHE_OK) g_share = nullptr;
    curl_global_cleanup();
}

Connection::~Connection() {
    if (curl_) curl_easy_cleanup((CURL*)curl_);
}
void cancel_running() { g_cancel_round++; }
void set_verify_tls(bool verify) { g_verify = verify; }
bool verify_tls() { return g_verify; }
const std::string& ca_bundle() { return g_ca_bundle; }

double retry_after(const Response& response, int64_t unix_now) {
    auto it = response.headers.find("retry-after");
    if (it == response.headers.end()) return -1;
    const std::string value = util::trim(it->second);
    if (value.empty()) return -1;
    if (value.find_first_not_of("0123456789") == std::string::npos) {
        double seconds = 0;
        for (char digit : value) { seconds = seconds * 10 + digit - '0'; if (seconds >= 86400) return 86400; }
        return seconds;
    }
    const time_t date = curl_getdate(value.c_str(), nullptr);
    return date < 0 ? -1 : std::clamp((double)date - unix_now, 0.0, 86400.0);
}

Response perform(const Request& req) {
    Response resp;
    const std::string log_url = req.private_url ? "[private URL]" : req.url.substr(0, 96);
    if (req.connection && !req.connection->curl_) req.connection->curl_ = curl_easy_init();
    CURL* curl = req.connection ? (CURL*)req.connection->curl_ : acquire_handle();
    if (!curl) {
        resp.error = "curl init failed";
        return resp;
    }
    curl_easy_reset(curl);

    Sink sink{&resp.body, req.max_bytes, req.cancel};
    if (req.on_data) {
        sink.req = &req;
        sink.resp = &resp;
        sink.curl = curl;
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024);  // fewer, larger reads for media
    }
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 12L);  // podcasts' trackers: chains of 8 seen
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(req.timeout, 12L));
    if (req.stall_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, req.stall_seconds);
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate/brotli as built
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent());
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (req.fresh_connection) curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
    // Each pooled handle keeps its last connection only: the Wii U runs out of sockets after a
    // few dozen, and handles that talked to many hosts (logos, thumbnails) would hoard them.
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    if (g_socket_setup) {
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, req.big_buffers ? (void*)1 : nullptr);
    }

    if (g_verify || req.require_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (util::file_exists(g_ca_bundle)) curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    // Nothing here speaks anything else, but this libcurl would (gopher, ftp, smb, file, ...), and
    // a playlist's guide link or a logo address is somebody else's text.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, req.require_tls ? "https" : "http,https");

    // For every transfer, so that cancel_running() reaches it (curl calls this about once a second when
    // nothing arrives, and with what does).
    Progress progress{&req, curl, 0, g_cancel_round.load()};
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)&progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    struct curl_slist* hdrs = nullptr;
    for (auto& [k, v] : req.headers) {
        if (util::lower(k) == "user-agent") {
            curl_easy_setopt(curl, CURLOPT_USERAGENT, v.c_str());
            continue;
        }
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }

    double t0 = util::now_seconds();
    progress.started = t0;
    CURLcode rc = curl_easy_perform(curl);
    double took = util::now_seconds() - t0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    long tls_verify_result = 0;
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &tls_verify_result);
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &resp.connects);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &resp.dns_seconds);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &resp.connect_seconds);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &resp.tls_seconds);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &resp.first_byte_seconds);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &resp.total_seconds);
    char* eff = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) resp.effective_url = eff;
    if (hdrs) curl_slist_free_all(hdrs);
    // Don't keep a dangling pointer to the stack-allocated sink/headers.
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);

    if (!req.connection) release_handle(curl);

    if (rc != CURLE_OK) {
        bool cancelled = sink.stopped || progress.gave_up || (req.cancel && req.cancel->load());
        if (sink.overflow) resp.error = tr("Response too large");
        else if (cancelled) resp.error = "Cancelled";
        else resp.error = friendly_error(rc);
        if (!cancelled) {  // asked for: not worth a line
            log_message(LOG_WARNING, "HTTP", "%s %s -> %s", req.method.c_str(), log_url.c_str(),
                        resp.error.c_str());
            if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_SSL_CONNECT_ERROR) {
                const auto* version = curl_version_info(CURLVERSION_NOW);
                // Numeric diagnostics avoid printing backend error buffers, which may
                // include a configured URL, query token, or redirect target.
                log_message(LOG_WARNING, "HTTP", "TLS: curl %d, verification %ld, UTC epoch %lld, backend %s",
                            (int)rc, tls_verify_result, (long long)util::unix_time(),
                            version && version->ssl_version ? version->ssl_version : "unknown");
            }
        }
    } else if (resp.status >= 400) {
        resp.error = resp.status == 401 || resp.status == 403 ? util::fmt(tr("Access denied (HTTP %ld)"), resp.status)
                     : resp.status == 404                     ? tr("Not found (HTTP 404)")
                     : resp.status == 429                     ? tr("Too many requests, try again later")
                     : resp.status >= 500                     ? util::fmt(tr("Server error (HTTP %ld)"), resp.status)
                                                              : util::fmt(tr("HTTP error %ld"), resp.status);
        // 404 is routine (a station without an icon, a video without SponsorBlock segments).
        if (resp.status != 404)
            log_message(LOG_WARNING, "HTTP", "%s %s -> %ld", req.method.c_str(), log_url.c_str(),
                        resp.status);
    }
    // Slow requests, for the log (the first few; COFFEEFLIX_HTTP_TRACE logs all of them). Streamed
    // media takes long by design and reports its own speed.
    static std::atomic<int> slow_logged{0};
    if (getenv("COFFEEFLIX_HTTP_TRACE") || (took > 5.0 && !req.on_data && slow_logged++ < 20))
        log_message(took > 5.0 ? LOG_WARNING : LOG_DEBUG, "HTTP", "%s %s: %ld, %zu KB in %.2f s", req.method.c_str(),
                    log_url.c_str(), resp.status, resp.body.size() / 1024, took);
    return resp;
}

Response get(const std::string& url, std::vector<std::pair<std::string, std::string>> headers, long timeout, bool private_url) {
    Request r;
    r.url = url;
    r.headers = std::move(headers);
    r.timeout = timeout;
    r.private_url = private_url;
    return perform(r);
}

Response post_json(const std::string& url, const std::string& body,
                   std::vector<std::pair<std::string, std::string>> headers, long timeout) {
    Request r;
    r.method = "POST";
    r.url = url;
    r.body = body;
    r.headers = std::move(headers);
    r.headers.emplace_back("Content-Type", "application/json");
    r.timeout = timeout;
    return perform(r);
}

}  // namespace http
