#include "services/smb.hpp"

#include <fcntl.h>
#include <poll.h>
#include <smb2/smb2.h>  // before libsmb2.h, which uses its types
#include <smb2/libsmb2.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace smb {

// A connected libsmb2 context. A context is not thread-safe, so each one is
// used by a single caller at a time: taken from the idle pool (or connected)
// for an operation or an open file, and handed back afterwards.
struct Conn {
    smb2_context* ctx = nullptr;
    std::string key;        // server, share and login it is connected with
    double idle_since = 0;
    bool reused = false;    // came from the pool, so it may have gone stale
};

namespace {

constexpr const char* STORE_SERVICE = "smb_share";  // saved shares live with the favorites
constexpr int CONNECT_TIMEOUT = 10;                 // seconds, unreachable hosts
constexpr int IO_TIMEOUT = 20;                      // seconds, per request
constexpr size_t MAX_IDLE = 4;
constexpr double IDLE_TTL = 240;                    // servers drop idle sessions eventually

std::mutex g_m;
std::vector<Conn*> g_idle;

std::string pool_key(const Share& s) {
    return s.host + '\x1f' + s.share + '\x1f' + s.domain + '\x1f' + s.user + '\x1f' + s.password;
}

// Failures caused by the request itself; anything else is treated as a broken
// connection (sync calls report those as -1 or a socket errno).
bool request_error(int rc) {
    return rc == -ENOENT || rc == -ENOTDIR || rc == -EISDIR || rc == -EACCES || rc == -EFBIG;
}

// errno for a call that returned NULL (opendir, open).
int last_error(smb2_context* ctx) {
    int nt = smb2_get_nterror(ctx);
    return nt ? -nterror_to_errno((uint32_t)nt) : -EIO;
}

// libsmb2 messages are technical ("Session setup failed with (0xc000006d)
// STATUS_LOGON_FAILURE"): turn the common ones into something friendlier.
std::string describe(const std::string& raw, int rc, const Share& s) {
    std::string e = util::trim(raw);
    auto has = [&](const char* k) { return e.find(k) != std::string::npos; };
    if (has("LOGON_FAILURE") || has("STATUS_ACCOUNT_") || has("STATUS_PASSWORD_") || has("WRONG_PASSWORD"))
        return tr("Wrong username or password");
    if (has("BAD_NETWORK_NAME"))
        return util::fmt(tr("There is no share called \"%s\" on %s"), s.share.c_str(), s.host.c_str());
    if (has("ACCESS_DENIED") || rc == -EACCES) return tr("Access denied");
    if (rc == -ETIMEDOUT) return util::fmt(tr("Timed out connecting to %s"), s.host.c_str());
    if (has("Invalid address")) return util::fmt(tr("Can't find %s on the network"), s.host.c_str());
    if (has("Socket connect failed") || has("Connect failed"))
        return util::fmt(tr("Can't connect to %s"), s.host.c_str());
    if (has("Negotiate failed")) return util::fmt(tr("%s doesn't support SMB 2 or newer"), s.host.c_str());
    if (rc == -ENOENT) return tr("Not found");
    if (rc == -EFBIG) return tr("The file is too large");
    if (!e.empty()) return e;
    return rc < 0 ? std::string(strerror(-rc)) : tr("Unknown error");
}

std::string describe(smb2_context* ctx, int rc, const Share& s) { return describe(smb2_get_error(ctx), rc, s); }

struct ConnectWait {
    bool done = false;
    int status = 0;
    std::string error;  // captured now: tearing down the failed session overwrites it
};

void on_connected(smb2_context* ctx, int status, void*, void* opaque) {
    auto* w = (ConnectWait*)opaque;
    w->done = true;
    w->status = status;
    if (status < 0) w->error = smb2_get_error(ctx);
}

Conn* open_conn(const Share& s, std::string& error) {
#ifndef __WIIU__
    // libsmb2 sends with writev(), which can't pass MSG_NOSIGNAL: a server
    // dropping the connection must not kill the desktop build.
    static std::once_flag ignore_sigpipe;
    std::call_once(ignore_sigpipe, [] { signal(SIGPIPE, SIG_IGN); });
#endif
    smb2_context* ctx = smb2_init_context();
    if (!ctx) {
        error = tr("Out of memory");
        return nullptr;
    }
    // No username and no password: anonymous (guest) login.
    if (!s.user.empty() || !s.password.empty()) smb2_set_password(ctx, s.password.c_str());
    if (!s.domain.empty()) smb2_set_domain(ctx, s.domain.c_str());
    smb2_set_timeout(ctx, IO_TIMEOUT);

    // Asynchronous connect so an unreachable host fails after CONNECT_TIMEOUT
    // instead of the system's TCP timeout (minutes).
    ConnectWait w;
    std::string user = s.user.empty() ? "Guest" : s.user;
    int rc = smb2_connect_share_async(ctx, s.host.c_str(), s.share.c_str(), user.c_str(), on_connected, &w);
    double deadline = util::now_seconds() + CONNECT_TIMEOUT;
    while (rc == 0 && !w.done) {
        if (util::now_seconds() > deadline) {
            rc = -ETIMEDOUT;
            break;
        }
        pollfd pfd{};
        pfd.fd = smb2_get_fd(ctx);
        pfd.events = (short)smb2_which_events(ctx);
        int n = poll(&pfd, 1, 100);
        if (n < 0 && errno != EINTR) rc = -errno;
        else if (n > 0 && smb2_service(ctx, pfd.revents) < 0) rc = -ECONNRESET;
    }
    if (w.done) rc = w.status;
    if (rc < 0) {
        std::string raw = !w.error.empty() ? w.error : rc == -ETIMEDOUT ? "timeout" : smb2_get_error(ctx);
        error = describe(raw, rc, s);
        log_message(LOG_WARNING, "SMB", "Connecting to //%s/%s failed: %s", s.host.c_str(), s.share.c_str(),
                    raw.c_str());
        smb2_destroy_context(ctx);  // before `w` goes out of scope: it may still call back
        return nullptr;
    }
    Conn* c = new Conn();
    c->ctx = ctx;
    c->key = pool_key(s);
    return c;
}

void destroy(Conn* c) {
    smb2_destroy_context(c->ctx);
    delete c;
}

// An idle connection to the same share and login, or a new one.
Conn* acquire(const Share& s, bool fresh, std::string& error) {
    if (!fresh) {
        std::vector<Conn*> expired;
        Conn* found = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_m);
            double now = util::now_seconds();
            std::string key = pool_key(s);
            for (auto it = g_idle.begin(); it != g_idle.end();) {
                if (now - (*it)->idle_since > IDLE_TTL) {
                    expired.push_back(*it);
                    it = g_idle.erase(it);
                } else if (!found && (*it)->key == key) {
                    found = *it;
                    it = g_idle.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (Conn* c : expired) destroy(c);
        if (found) {
            found->reused = true;
            return found;
        }
    }
    return open_conn(s, error);
}

// Returns a connection to the pool, or closes it if it's broken.
void release(Conn* c, bool healthy) {
    if (!c) return;
    if (healthy) {
        std::lock_guard<std::mutex> lk(g_m);
        c->idle_since = util::now_seconds();
        g_idle.push_back(c);
        if (g_idle.size() <= MAX_IDLE) return;
        c = g_idle.front();  // oldest
        g_idle.erase(g_idle.begin());
    }
    destroy(c);
}

// Runs `op(ctx) -> 0 or -errno` on a pooled connection. A pooled connection
// that went stale while idle (server timeout, Wi-Fi reconnect) is replaced once.
template <typename Op>
bool run(const Share& s, std::string& error, Op op) {
    for (int attempt = 0; attempt < 2; attempt++) {
        Conn* c = acquire(s, attempt > 0, error);
        if (!c) return false;
        smb2_set_error(c->ctx, "");  // also clears the NT status last_error() reads
        int rc = op(c->ctx);
        if (rc >= 0) {
            release(c, true);
            return true;
        }
        error = describe(c->ctx, rc, s);
        bool retry = c->reused && !request_error(rc);
        release(c, request_error(rc));
        if (!retry) return false;
    }
    return false;
}

std::string clean_path(std::string p) {
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

// Splits a URL into the share to connect to and the path inside it.
bool resolve(const std::string& url, Share& s, std::string& path, std::string& error) {
    if (!is_url(url)) {
        error = tr("Not a network share address");
        return false;
    }
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    std::string head = rest.substr(0, slash);
    path = clean_path(slash == std::string::npos ? "" : rest.substr(slash + 1));
    for (Share& saved : saved_shares()) {
        if (saved.name == head) {
            s = std::move(saved);
            return true;
        }
    }
    // smb://[[domain;]user[:password]@]host/share/path
    s = Share{};
    size_t at = head.rfind('@');
    if (at != std::string::npos) {
        std::string login = head.substr(0, at);
        size_t colon = login.find(':');
        s.user = util::url_decode(login.substr(0, colon));
        if (colon != std::string::npos) s.password = util::url_decode(login.substr(colon + 1));
        size_t semi = s.user.find(';');
        if (semi != std::string::npos) {
            s.domain = s.user.substr(0, semi);
            s.user.erase(0, semi + 1);
        }
        head.erase(0, at + 1);
    }
    s.host = head;
    size_t ps = path.find('/');
    s.share = path.substr(0, ps);
    path = ps == std::string::npos ? "" : path.substr(ps + 1);
    if (s.host.empty() || s.share.empty()) {
        error = tr("Unknown network share");
        return false;
    }
    return true;
}

}  // namespace

// --- saved shares ------------------------------------------------------------------

std::vector<Share> saved_shares() {
    std::vector<Share> out;
    for (const store::Fav& f : store::favs(STORE_SERVICE)) {
        json::Doc d = json::Doc::parse(f.extra);
        Share s;
        s.name = f.id;
        s.host = json::str(d.get(), {"host"});
        s.share = json::str(d.get(), {"share"});
        s.user = json::str(d.get(), {"user"});
        s.password = json::str(d.get(), {"password"});
        s.domain = json::str(d.get(), {"domain"});
        if (!s.host.empty() && !s.share.empty()) out.push_back(std::move(s));
    }
    return out;
}

std::string save_share(const Share& s) {
    // The name is the first segment of smb:// URLs, so it can't contain slashes.
    std::string name = util::trim(util::replace_all(util::replace_all(s.name, "/", "-"), "\\", "-"));
    if (name.empty()) name = default_name(s);
    json_t* o = json_object();
    json_object_set_new(o, "host", json_string(s.host.c_str()));
    json_object_set_new(o, "share", json_string(s.share.c_str()));
    json_object_set_new(o, "user", json_string(s.user.c_str()));
    json_object_set_new(o, "password", json_string(s.password.c_str()));
    json_object_set_new(o, "domain", json_string(s.domain.c_str()));
    store::fav_set(STORE_SERVICE, store::Fav{name, name, s.host + "/" + s.share, "", json::dump(o)}, true);
    json_decref(o);
    return name;
}

void remove_share(const std::string& name) { store::fav_set(STORE_SERVICE, store::Fav{name, "", "", "", ""}, false); }

std::string default_name(const Share& s) {
    std::string name = s.host.empty() || s.share.empty() ? s.share + s.host
                                                         : util::fmt(tr("%s on %s"), s.share.c_str(), s.host.c_str());
    return util::replace_all(name, "/", "-");
}

bool is_url(const std::string& s) { return util::starts_with(s, "smb://"); }

std::string share_url(const Share& s, const std::string& path) {
    std::string p = clean_path(path);
    return "smb://" + s.name + (p.empty() ? "" : "/" + p);
}

std::string display_path(const std::string& url) {
    Share s;
    std::string path, error;
    if (!resolve(url, s, path, error)) return url;
    return util::replace_all("\\\\" + s.host + "\\" + s.share + (path.empty() ? "" : "\\" + path), "/", "\\");
}

// --- files and folders ----------------------------------------------------------------

bool list_dir(const std::string& url, std::vector<DirEntry>& out, std::string& error) {
    Share s;
    std::string path;
    if (!resolve(url, s, path, error)) return false;
    bool ok = run(s, error, [&](smb2_context* ctx) -> int {
        out.clear();
        smb2dir* dir = smb2_opendir(ctx, path.c_str());
        if (!dir) return last_error(ctx);
        while (smb2dirent* de = smb2_readdir(ctx, dir)) {
            if (!de->name || de->name[0] == '.' || de->name[0] == 0) continue;
            if (de->st.smb2_attributes & (SMB2_FILE_ATTRIBUTE_HIDDEN | SMB2_FILE_ATTRIBUTE_SYSTEM)) continue;
            DirEntry e;
            e.name = de->name;
            e.is_dir = de->st.smb2_type == SMB2_TYPE_DIRECTORY;
            e.size = de->st.smb2_size;
            out.push_back(std::move(e));
        }
        smb2_closedir(ctx, dir);
        return 0;
    });
    if (!ok) log_message(LOG_WARNING, "SMB", "Can't list %s: %s", url.c_str(), error.c_str());
    return ok;
}

bool test_share(const Share& s, std::string& error) {
    Conn* c = acquire(s, true, error);
    release(c, true);  // keep it: the user is about to browse this share
    return c != nullptr;
}

bool read_file(const std::string& url, std::string& out, size_t max_bytes) {
    Share s;
    std::string path, error;
    if (!resolve(url, s, path, error)) return false;
    bool ok = run(s, error, [&](smb2_context* ctx) -> int {
        smb2fh* fh = smb2_open(ctx, path.c_str(), O_RDONLY);
        if (!fh) return last_error(ctx);
        smb2_stat_64 st;
        int rc = smb2_fstat(ctx, fh, &st);
        if (rc == 0 && st.smb2_size > max_bytes) rc = -EFBIG;
        if (rc == 0) {
            out.resize((size_t)st.smb2_size);
            uint32_t chunk = std::max<uint32_t>(smb2_get_max_read_size(ctx), 64u << 10);
            size_t done = 0;
            while (done < out.size()) {
                uint32_t n = (uint32_t)std::min<size_t>(chunk, out.size() - done);
                rc = smb2_pread(ctx, fh, (uint8_t*)out.data() + done, n, done);
                if (rc <= 0) break;
                done += (size_t)rc;
            }
            out.resize(done);
            if (rc > 0) rc = 0;
        }
        smb2_close(ctx, fh);
        return rc;
    });
    if (!ok) log_message(LOG_WARNING, "SMB", "Can't read %s: %s", url.c_str(), error.c_str());
    return ok;
}

// --- streaming ------------------------------------------------------------------------

std::unique_ptr<File> File::open(const std::string& url, std::string& error) {
    std::unique_ptr<File> f(new File());
    if (!resolve(url, f->share_, f->path_, error) || !f->reopen(error)) {
        log_message(LOG_WARNING, "SMB", "Can't open %s: %s", url.c_str(), error.c_str());
        return nullptr;
    }
    return f;
}

File::~File() { close_handle(true); }

void File::close_handle(bool healthy) {
    if (fh_ && smb2_close(conn_->ctx, fh_) < 0) healthy = false;
    fh_ = nullptr;
    release(conn_, healthy);
    conn_ = nullptr;
}

bool File::reopen(std::string& error) {
    close_handle(false);
    for (int attempt = 0; attempt < 2; attempt++) {
        conn_ = acquire(share_, attempt > 0, error);
        if (!conn_) return false;
        smb2_context* ctx = conn_->ctx;
        smb2_set_error(ctx, "");
        smb2_stat_64 st;
        int rc = (fh_ = smb2_open(ctx, path_.c_str(), O_RDONLY)) ? smb2_fstat(ctx, fh_, &st) : last_error(ctx);
        if (rc == 0) {
            size_ = st.smb2_size;
            return true;
        }
        error = describe(ctx, rc, share_);
        bool retry = conn_->reused && !request_error(rc);
        close_handle(request_error(rc));
        if (!retry) return false;
    }
    return false;
}

int File::read(uint8_t* buf, int size) {
    if (size <= 0 || pos_ >= size_) return 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!conn_) {
            // The connection dropped (Wi-Fi hiccup, server restart): reconnect.
            std::string error;
            if (!reopen(error)) return -EIO;
        }
        uint32_t n = (uint32_t)std::min<uint64_t>({(uint64_t)size, size_ - pos_, smb2_get_max_read_size(conn_->ctx)});
        int rc = smb2_pread(conn_->ctx, fh_, buf, n, pos_);
        if (rc >= 0) {
            pos_ += (uint64_t)rc;
            return rc;
        }
        log_message(LOG_WARNING, "SMB", "Read failed at %llu: %s", (unsigned long long)pos_, smb2_get_error(conn_->ctx));
        if (request_error(rc)) return rc;
        close_handle(false);
    }
    return -EIO;
}

int64_t File::seek(int64_t offset, int whence) {
    // Reads are positioned (pread), so seeking is just bookkeeping.
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)pos_ : whence == SEEK_END ? (int64_t)size_ : -1;
    if (base < 0 || base + offset < 0) return -EINVAL;
    pos_ = (uint64_t)(base + offset);
    return (int64_t)pos_;
}

}  // namespace smb
