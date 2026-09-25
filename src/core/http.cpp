#include "core/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <vector>

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

int progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = (const std::atomic<bool>*)clientp;
    return (cancel && cancel->load()) ? 1 : 0;
}

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
    if (g_pool.size() < 8) g_pool.push_back(h);  // the player downloads over 3 at once
    else curl_easy_cleanup(h);
}

std::string friendly_error(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST: return "No internet connection (can't find the server)";
        case CURLE_COULDNT_CONNECT: return "Can't connect to the server";
        case CURLE_OPERATION_TIMEDOUT: return "The connection timed out";
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING: return "The connection was interrupted";
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CONNECT_ERROR:
            return "Secure connection failed (check the console's date and time)";
        default: return curl_easy_strerror(rc);
    }
}

}  // namespace

const char* user_agent() { return "CoffeeFlix/2.0 (Nintendo Wii U)"; }

void init(const std::string& ca_bundle_path, void (*socket_setup)(int fd)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
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
    curl_global_cleanup();
}
void set_verify_tls(bool verify) { g_verify = verify; }
bool verify_tls() { return g_verify; }
const std::string& ca_bundle() { return g_ca_bundle; }

Response perform(const Request& req) {
    Response resp;
    CURL* curl = acquire_handle();
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
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(req.timeout, 12L));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate/brotli as built
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent());
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
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
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, req.require_tls ? "https" : "http,https");

    if (req.cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)req.cancel);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

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
    CURLcode rc = curl_easy_perform(curl);
    double took = util::now_seconds() - t0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    char* eff = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) resp.effective_url = eff;
    if (hdrs) curl_slist_free_all(hdrs);
    // Don't keep a dangling pointer to the stack-allocated sink/headers.
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);

    release_handle(curl);

    if (rc != CURLE_OK) {
        bool cancelled = sink.stopped || (req.cancel && req.cancel->load());
        if (sink.overflow) resp.error = "Response too large";
        else if (cancelled) resp.error = "Cancelled";
        else resp.error = friendly_error(rc);
        if (!cancelled)  // asked for: not worth a line
            log_message(LOG_WARNING, "HTTP", "%s %s -> %s", req.method.c_str(), req.url.substr(0, 96).c_str(),
                        resp.error.c_str());
    } else if (resp.status >= 400) {
        resp.error = resp.status == 401 || resp.status == 403 ? util::fmt("Access denied (HTTP %ld)", resp.status)
                     : resp.status == 404                     ? "Not found (HTTP 404)"
                     : resp.status == 429                     ? "Too many requests, try again later"
                     : resp.status >= 500                     ? util::fmt("Server error (HTTP %ld)", resp.status)
                                                              : util::fmt("HTTP error %ld", resp.status);
        // 404 is routine (a station without an icon, a video without SponsorBlock segments).
        if (resp.status != 404)
            log_message(LOG_WARNING, "HTTP", "%s %s -> %ld", req.method.c_str(), req.url.substr(0, 96).c_str(),
                        resp.status);
    }
    // Slow requests, for the log (the first few; COFFEEFLIX_HTTP_TRACE logs all of them). Streamed
    // media takes long by design and reports its own speed.
    static std::atomic<int> slow_logged{0};
    if (getenv("COFFEEFLIX_HTTP_TRACE") || (took > 5.0 && !req.on_data && slow_logged++ < 20))
        log_message(took > 5.0 ? LOG_WARNING : LOG_DEBUG, "HTTP", "%s %s: %ld, %zu KB in %.2f s", req.method.c_str(),
                    req.url.substr(0, 90).c_str(), resp.status, resp.body.size() / 1024, took);
    return resp;
}

Response get(const std::string& url, std::vector<std::pair<std::string, std::string>> headers, long timeout) {
    Request r;
    r.url = url;
    r.headers = std::move(headers);
    r.timeout = timeout;
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
