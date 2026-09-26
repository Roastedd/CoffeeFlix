#include "app/updater.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "core/version.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/player.hpp"
#include "ui/ui.hpp"

namespace updater {

namespace {

const char* LATEST = "https://api.github.com/repos/Roastedd/CoffeeFlix/releases/latest";
const char* DOWNLOADS = "https://github.com/Roastedd/CoffeeFlix/releases/download/";
const char* RELEASES_PAGE = "github.com/Roastedd/CoffeeFlix/releases";
const int64_t DAY = 24 * 3600;

// Developer updates: a computer running tools/dev-update.sh answers this broadcast with the port
// of its web server, which offers dev.json (the build's version, size, SHA-256 and signature,
// and what changed) and coffeeflix.wuhb.
const uint16_t DEV_PORT = 47291;
const char* DEV_ASK = "COFFEEFLIX-DEV?";
const char* DEV_ANSWER = "COFFEEFLIX-DEV ";
// The public half of the key tools/dev-update.sh signs builds with. The private half stays on
// the developer's computer (~/.coffeeflix/dev-key.pem), so no one else can make a build that
// this accepts.
const char* DEV_KEY =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEJlGyOyIgKjsQEXBBTNiCldx8+ugC\n"
    "AXkHAW5FifDHEA134ZtRyWSnD2E+eImm7FWXNeWC0N19164SB1qAyKNJ+g==\n"
    "-----END PUBLIC KEY-----\n";

struct Job {
    std::atomic<bool> cancel{false};
    std::atomic<int32_t> done{0};  // bytes; 32 bits, which the Wii U's CPU can update atomically
};

struct CheckResult {
    std::string error;
    Release rel;
    std::string server;  // developer builds: where it came from
    bool running = false;  // developer builds: it's the one running now
};

State g_state = IDLE;
Release g_release;
std::string g_error, g_reason, g_bundle, g_dev_server;
bool g_supported = false, g_has_previous = false, g_switch_back = false;
bool g_declined = false;  // this session: the user said no to this update
bool g_dev_checked = false;  // this session
int g_check = 0;  // which check is the current one
int64_t g_last_checked = 0;
double g_started = 0, g_next_check = 0, g_next_download = 0;
std::shared_ptr<Job> g_job;
tasks::Scope g_scope;

// Desktop tests point this at a local server (never set on the Wii U).
std::string latest_url() { return util::env_or("COFFEEFLIX_UPDATE_URL", LATEST); }

// Where the update comes from, for messages.
const char* source(const Release& rel) { return rel.dev ? "your computer" : "GitHub"; }
const char* Source(const Release& rel) { return rel.dev ? "Your computer" : "GitHub"; }

// What the user's answers about an update (skipped, told about) refer to.
std::string key(const Release& rel) { return rel.dev ? rel.sha256 : rel.version; }

// "2.1.2", "v2.1.2", "2.1.2-11-gf25ba75" -> {2, 1, 2}; empty without a leading number.
std::vector<int> numbers(const std::string& v) {
    std::vector<int> out;
    size_t i = !v.empty() && (v[0] == 'v' || v[0] == 'V') ? 1 : 0;
    while (i < v.size() && isdigit((unsigned char)v[i])) {
        int n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) n = std::min(n * 10 + (v[i++] - '0'), 1000000);
        out.push_back(n);
        if (i + 1 < v.size() && v[i] == '.' && isdigit((unsigned char)v[i + 1])) i++;
        else break;
    }
    return out;
}

bool newer(const std::string& a, const std::string& b) {
    std::vector<int> x = numbers(a), y = numbers(b);
    size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x > y;
}

// A release's version ("2.2.1"), not one of a build in between ("2.2.1-3-gabc1234").
bool release_build(const std::string& v) {
    return !numbers(v).empty() && v.find_first_not_of("v0123456789.") == std::string::npos;
}

bool is_sha256(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'); });
}

// Lower-case hex to bytes; empty if it isn't hex.
std::vector<uint8_t> from_hex(const std::string& s) {
    auto digit = [](char c) { return isdigit((unsigned char)c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = digit(s[i]), lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((uint8_t)(hi << 4 | lo));
    }
    return s.size() % 2 ? std::vector<uint8_t>() : out;
}

struct FileHash {
    std::string sha256;
    int64_t size = 0;
    char magic[4] = {};
};

// Worker: a file's SHA-256, size and first four bytes. False if it couldn't be read (or cancel).
bool hash_file(const std::string& path, const std::atomic<bool>* cancel, FileHash& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    sha256::Hasher h;
    std::vector<char> buf(256 * 1024);
    size_t n;
    while (!(cancel && *cancel) && (n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (out.size == 0 && n >= 4) memcpy(out.magic, buf.data(), 4);
        h.update(buf.data(), n);
        out.size += n;
    }
    bool ok = !ferror(f) && !(cancel && *cancel);
    fclose(f);
    out.sha256 = h.hex();
    return ok;
}

int64_t file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

bool is_bundle(const std::string& path) {
    char magic[4] = {};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "WUHB", 4);
    fclose(f);
    return ok;
}

bool player_busy() {
    player::State s = player::state();
    return s == player::OPENING || s == player::BUFFERING || s == player::PLAYING;
}

// Drops what the app's fonts can't draw (symbols and emoji from U+2600 on).
std::string drawable(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) break;
        uint32_t cp = len == 3 ? (c & 15u) << 12 | (s[i + 1] & 63u) << 6 | (s[i + 2] & 63u) : 0;
        if (len <= 2 || (len == 3 && cp < 0x2600)) out.append(s, i, len);
        i += len;
    }
    return util::trim(out);
}

// Release notes from GitHub's Markdown to plain text: no markup, links as their text, and
// without the install steps every release repeats.
std::string plain_notes(const std::string& md) {
    std::string out;
    bool skipping = false, blank = true;
    for (std::string line : util::split(md, '\n')) {
        line = util::trim(line);
        if (util::starts_with(line, "<!--")) continue;
        for (const char* mark : {"**", "__", "`"}) line = util::replace_all(line, mark, "");
        while (util::starts_with(line, "#")) line.erase(0, 1);
        line = util::trim(line);
        if (util::starts_with(line, "* ") || util::starts_with(line, "- ")) line = "\xE2\x80\xA2 " + line.substr(2);
        for (size_t open; (open = line.find('[')) != std::string::npos;) {
            size_t mid = line.find("](", open);
            size_t close = mid == std::string::npos ? mid : line.find(')', mid);
            if (close == std::string::npos) break;
            line = line.substr(0, open) + line.substr(open + 1, mid - open - 1) + line.substr(close + 1);
        }
        // GitHub's generated notes end each change with "by @someone in https://...".
        size_t in = line.find(" in https://");
        if (in != std::string::npos) {
            line.erase(in);
            size_t by = line.rfind(" by @");
            if (by != std::string::npos) line.erase(by);
        }
        if (line.empty()) {
            skipping = false;
            if (!blank) out += "\n";
            blank = true;
            continue;
        }
        if (util::starts_with(line, "Install:") || util::starts_with(line, "Tiramisu:")) skipping = true;
        if (skipping || util::starts_with(line, "Full Changelog")) continue;
        line = drawable(line);
        if (line.empty()) continue;
        out += line + "\n";
        blank = false;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// Worker: the latest release (GitHub leaves out drafts and pre-releases) and its bundle.
CheckResult fetch_latest() {
    CheckResult out;
    http::Request req;
    req.url = latest_url();
    req.headers = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    req.require_tls = true;
    req.max_bytes = 1 << 20;
    http::Response r = http::perform(req);
    if (!r.ok()) {
        out.error = r.status == 403 || r.status == 429 ? "GitHub is busy right now. Try again in an hour."
                    : r.status == 404                  ? "CoffeeFlix has no releases yet"
                    : !r.error.empty()                 ? r.error
                                                       : util::fmt("GitHub answered %ld", r.status);
        return out;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* root = doc.get();
    std::string tag = json::str(root, {"tag_name"});
    out.rel.version = util::starts_with(tag, "v") ? tag.substr(1) : tag;
    out.rel.notes = plain_notes(json::str(root, {"body"}));
    json_t* assets = json::at(root, {"assets"});
    for (size_t i = 0; i < json::size(assets); i++) {
        json_t* a = json_array_get(assets, i);
        if (json::str(a, {"name"}) != "coffeeflix.wuhb") continue;
        out.rel.url = json::str(a, {"browser_download_url"});
        out.rel.size = json::num(a, {"size"});
        std::string digest = json::str(a, {"digest"});
        if (util::starts_with(digest, "sha256:")) out.rel.sha256 = util::lower(digest.substr(7));
    }
    bool test = latest_url() != LATEST;
    if (!doc) out.error = "GitHub's answer didn't make sense";
    else if (numbers(out.rel.version).empty()) out.error = "The latest release has no version number";
    else if (out.rel.url.empty()) out.error = "The latest release has no coffeeflix.wuhb";
    else if (!is_sha256(out.rel.sha256)) out.error = "GitHub gave no checksum for the download, so it can't be checked";
    else if (!test && !util::starts_with(out.rel.url, DOWNLOADS)) out.error = "The download isn't on CoffeeFlix's GitHub page";
    else if (out.rel.size < (1 << 20) || out.rel.size > (256 << 20)) out.error = "The download's size looks wrong";
    return out;
}

// Worker: the address ("ip:port") of a computer running tools/dev-update.sh, asked for across
// the network, else the one found last time.
std::string find_dev_server() {
    std::string fixed = util::env_or("COFFEEFLIX_DEV_SERVER", "");  // desktop tests
    if (!fixed.empty()) return fixed;
    std::string found;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fd, (sockaddr*)&local, sizeof(local));
        // Everyone, and this network's own broadcast address in case a router drops the first.
        std::vector<std::string> targets = {"255.255.255.255"};
        std::string ip = platform::ip_address();
        if (ip.rfind('.') != std::string::npos) targets.push_back(ip.substr(0, ip.rfind('.')) + ".255");
        // UDP can drop packets: ask three times.
        for (int attempt = 0; attempt < 3 && found.empty(); attempt++) {
            for (const std::string& t : targets) {
                sockaddr_in dst;
                memset(&dst, 0, sizeof(dst));
                dst.sin_family = AF_INET;
                dst.sin_port = htons(DEV_PORT);
                inet_aton(t.c_str(), &dst.sin_addr);
                sendto(fd, DEV_ASK, strlen(DEV_ASK), 0, (sockaddr*)&dst, sizeof(dst));
            }
            pollfd p{fd, POLLIN, 0};
            while (found.empty() && poll(&p, 1, 500) > 0) {
                char buf[64];
                sockaddr_in from;
                socklen_t len = sizeof(from);
                int n = (int)recvfrom(fd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
                if (n <= 0) break;
                buf[n] = 0;
                int port = strncmp(buf, DEV_ANSWER, strlen(DEV_ANSWER)) == 0 ? atoi(buf + strlen(DEV_ANSWER)) : 0;
                uint32_t a = ntohl(from.sin_addr.s_addr);
                if (port > 0 && port < 65536)
                    found = util::fmt("%u.%u.%u.%u:%d", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255, port);
            }
        }
        close(fd);
    }
    return found.empty() ? store::get_str("update_dev_server") : found;
}

// Worker: the running bundle's SHA-256, kept with its size and time so it's read only once.
std::string running_sha256(const std::string& bundle) {
    struct stat st;
    if (stat(bundle.c_str(), &st) != 0) return "";
    std::string stamp = util::fmt("%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
    if (store::get_str("update_running_stamp") == stamp) return store::get_str("update_running_sha256");
    FileHash fh;
    if (!hash_file(bundle, nullptr, fh)) return "";
    store::set_str("update_running_stamp", stamp);
    store::set_str("update_running_sha256", fh.sha256);
    return fh.sha256;
}

// Worker: the build tools/dev-update.sh offers, if the developer key signed it.
CheckResult fetch_dev(const std::string& bundle) {
    CheckResult out;
    out.rel.dev = true;
    out.server = find_dev_server();
    if (out.server.empty()) {
        out.error = "Couldn't find your computer. Run tools/dev-update.sh on it, on the same network as this Wii U.";
        return out;
    }
    http::Request req;
    req.url = "http://" + out.server + "/dev.json";
    req.timeout = 8;
    req.max_bytes = 1 << 20;
    http::Response r = http::perform(req);
    if (!r.ok()) {
        out.error = r.status ? util::fmt("Your computer (%s) answered %ld", out.server.c_str(), r.status)
                             : util::fmt("Couldn't reach your computer at %s. Is tools/dev-update.sh running?", out.server.c_str());
        return out;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* root = doc.get();
    out.rel.version = json::str(root, {"version"});
    out.rel.notes = plain_notes(json::str(root, {"notes"}));
    out.rel.url = "http://" + out.server + "/coffeeflix.wuhb";
    out.rel.size = json::num(root, {"size"});
    out.rel.sha256 = util::lower(json::str(root, {"sha256"}));
    std::vector<uint8_t> digest = from_hex(out.rel.sha256), sig = from_hex(util::lower(json::str(root, {"signature"})));
    if (!doc) out.error = "Your computer's answer didn't make sense";
    else if (out.rel.version.empty()) out.error = "The build has no version";
    else if (!is_sha256(out.rel.sha256)) out.error = "Your computer gave no checksum for the build";
    else if (out.rel.size < (1 << 20) || out.rel.size > (256 << 20)) out.error = "The build's size looks wrong";
    else if (sig.empty() || !platform::verify_signature(DEV_KEY, digest.data(), sig))
        out.error = "That build isn't signed with your developer key, so it can't be installed";
    else out.running = running_sha256(bundle) == out.rel.sha256;
    return out;
}

// Worker: reads the saved file back from the SD card and compares it with what was published.
std::string verify(const std::string& path, const Release& rel, const Job& job) {
    FileHash fh;
    bool read = hash_file(path, &job.cancel, fh);
    if (job.cancel) return "Cancelled";
    if (!read || fh.size != rel.size) return "Couldn't read the download back";
    if (memcmp(fh.magic, "WUHB", 4)) return "The download isn't a Wii U app";
    if (fh.sha256 != rel.sha256) return util::fmt("The download didn't match %s's checksum", source(rel));
    return "";
}

// Worker: the bundle into dest, checked. Anything short of a match is deleted.
std::string fetch(const Release& rel, const std::string& dest, Job& job) {
    remove(dest.c_str());
    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) return "Couldn't write to the SD card";
    int64_t written = 0;
    bool write_failed = false, too_big = false;
    http::Request req;
    req.url = rel.url;
    req.require_tls = !rel.dev;  // developer builds come over the local network, signed instead
    req.timeout = 30 * 60;
    req.big_buffers = true;
    req.cancel = &job.cancel;
    req.on_data = [&](const http::Response& so_far, const char* data, size_t size) {
        if (so_far.status != 200) return false;
        if (written + (int64_t)size > rel.size) {
            too_big = true;
            return false;
        }
        if (fwrite(data, 1, size, f) != size) {
            write_failed = true;
            return false;
        }
        written += size;
        job.done = (int32_t)written;  // at most 256 MB
        return true;
    };
    http::Response r = http::perform(req);
    if (fclose(f) != 0) write_failed = true;
    std::string err = job.cancel                         ? "Cancelled"
                      : write_failed                     ? "Couldn't save the download. Is the SD card full?"
                      : too_big                          ? util::fmt("The download was bigger than %s said", source(rel))
                      : r.status >= 300                  ? util::fmt("%s answered %ld", Source(rel), r.status)
                      : !r.error.empty()                 ? r.error
                      : written != rel.size              ? "The download stopped early"
                                                         : verify(dest, rel, job);
    if (!err.empty()) remove(dest.c_str());
    return err;
}

void remember_ready(const Release* rel) {
    store::set_str("update_ready", rel ? rel->version : "");
    store::set_str("update_ready_sha256", rel ? rel->sha256 : "");
    store::set_int("update_ready_size", rel ? rel->size : 0);
    store::set_str("update_ready_notes", rel ? rel->notes.substr(0, 4000) : "");
    store::set_bool("update_ready_dev", rel && rel->dev);
}

void on_checked(CheckResult r, bool automatic_check) {
    if (!r.error.empty()) {
        log_message(LOG_WARNING, "Update", "Checking for updates failed: %s", r.error.c_str());
        g_next_check = util::now_seconds() + 3600;
        g_state = automatic_check ? IDLE : FAILED;
        g_error = r.error;
        return;
    }
    g_last_checked = util::unix_time();
    if (r.rel.dev) {
        g_dev_server = r.server;
        store::set_str("update_dev_server", r.server);
        if (r.running) {
            log_message(LOG_OK, "Update", "Running the developer build %s from %s", r.rel.version.c_str(), r.server.c_str());
            g_state = UP_TO_DATE;
            return;
        }
    } else {
        store::set_int("update_checked", g_last_checked);
        // A build from in between releases (a developer build) goes back to the release too.
        bool back = !release_build(APP_VERSION) && !newer(APP_VERSION, r.rel.version);
        if (!newer(r.rel.version, APP_VERSION) && !back) {
            log_message(LOG_OK, "Update", "Up to date (%s; the latest release is %s)", APP_VERSION, r.rel.version.c_str());
            g_state = UP_TO_DATE;
            return;
        }
    }
    g_release = r.rel;
    g_state = AVAILABLE;
    log_message(LOG_OK, "Update", "%s %s is out (this is %s)", r.rel.dev ? "The developer build" : "CoffeeFlix",
                r.rel.version.c_str(), APP_VERSION);
    if (automatic_check && !automatic() && store::get_str("update_notified") != key(r.rel)) {
        store::set_str("update_notified", key(r.rel));
        ui::toast(r.rel.dev ? util::fmt("Developer build %s is available. Install it in Settings.", r.rel.version.c_str())
                            : util::fmt("CoffeeFlix %s is available. Update it in Settings.", r.rel.version.c_str()),
                  ic::CLOUD_DOWNLOAD);
    }
}

void start_check(bool automatic_check) {
    g_state = CHECKING;
    g_error.clear();
    int check = ++g_check;
    bool dev = developer();
    std::string bundle = g_bundle;
    g_scope.run<CheckResult>([dev, bundle] { return dev ? fetch_dev(bundle) : fetch_latest(); },
                             [automatic_check, check](CheckResult r) {
                                 if (check == g_check) on_checked(std::move(r), automatic_check);
                             });
}

// A download from an earlier session, checked again before it's trusted.
void reverify(const Release& rel) {
    g_release = rel;
    g_state = CHECKING;
    g_job = std::make_shared<Job>();
    std::shared_ptr<Job> job = g_job;
    std::string path = g_bundle + ".download";
    g_scope.run<std::string>([rel, path, job] { return verify(path, rel, *job); }, [rel, path, job](std::string err) {
        if (job != g_job) return;
        g_job.reset();
        if (err.empty()) {
            g_state = READY;
            log_message(LOG_OK, "Update", "CoffeeFlix %s is downloaded and installs when CoffeeFlix closes", rel.version.c_str());
            return;
        }
        log_message(LOG_WARNING, "Update", "Dropped the download of %s: %s", rel.version.c_str(), err.c_str());
        remove(path.c_str());
        remember_ready(nullptr);
        g_state = IDLE;
    });
}

// Written for the next start, which reports it.
void fail(const std::string& why) {
    log_message(LOG_ERROR, "Update", "%s", why.c_str());
    store::set_str("update_error", why);
    store::save_now();
}

void install() {
    std::string dl = g_bundle + ".download", bak = g_bundle + ".bak";
    const Release& rel = g_release;
    // Last look: still the file that was checked.
    if (file_size(dl) != rel.size || !is_bundle(dl)) {
        remove(dl.c_str());
        remember_ready(nullptr);
        fail("The downloaded update went missing, so nothing changed");
        return;
    }
    std::string settings = platform::data_dir() + "/coffeeflix.json", data;
    if (util::read_file(settings, data)) util::write_file_atomic(settings + ".bak", data);
    if (!platform::release_app_bundle()) {
        fail("Aroma didn't let go of the running app, so nothing changed");
        return;
    }
    if (util::file_exists(bak) && remove(bak.c_str()) != 0) {
        fail(util::fmt("Couldn't remove the older kept version (%s), so nothing changed", strerror(errno)));
        return;
    }
    if (rename(g_bundle.c_str(), bak.c_str()) != 0) {
        fail(util::fmt("Couldn't set the current version aside (%s), so nothing changed", strerror(errno)));
        return;
    }
    if (rename(dl.c_str(), g_bundle.c_str()) != 0) {
        std::string why = strerror(errno);
        if (rename(bak.c_str(), g_bundle.c_str()) == 0) {
            fail("Couldn't put the new version in place (" + why + "), so nothing changed");
        } else {
            fail(util::fmt("Couldn't put the new version in place (%s). On a PC, rename %s to %s", why.c_str(),
                           util::file_name(bak).c_str(), util::file_name(g_bundle).c_str()));
        }
        return;
    }
    store::set_str("update_prev_version", APP_VERSION);
    store::set_str("update_installed", rel.version);
    store::set_str("update_skip", "");
    remember_ready(nullptr);
    store::save_now();
    log_message(LOG_OK, "Update", "Installed CoffeeFlix %s; %s is kept as %s", rel.version.c_str(), APP_VERSION,
                util::file_name(bak).c_str());
}

// Swaps the running version and the kept one.
void swap_back() {
    std::string bak = g_bundle + ".bak", aside = g_bundle + ".old", dl = g_bundle + ".download";
    std::string prev = store::get_str("update_prev_version");
    remove(dl.c_str());  // or it would install again next time
    remember_ready(nullptr);
    if (!platform::release_app_bundle()) {
        fail("Aroma didn't let go of the running app, so nothing changed");
        return;
    }
    remove(aside.c_str());
    if (rename(g_bundle.c_str(), aside.c_str()) != 0) {
        fail(util::fmt("Couldn't set the current version aside (%s), so nothing changed", strerror(errno)));
        return;
    }
    if (rename(bak.c_str(), g_bundle.c_str()) != 0) {
        std::string why = strerror(errno);
        rename(aside.c_str(), g_bundle.c_str());
        fail("Couldn't put the kept version in place (" + why + "), so nothing changed");
        return;
    }
    rename(aside.c_str(), bak.c_str());  // else init() finishes it
    store::set_str("update_prev_version", APP_VERSION);
    store::set_str("update_installed", prev);
    // Going back from a release, or to a developer build: automatic updates leave this one alone.
    // (Developer builds are told apart by their checksum.)
    std::string skip;
    if (!release_build(APP_VERSION)) skip = store::get_str("update_running_sha256");
    else if (newer(APP_VERSION, prev) || !release_build(prev)) skip = APP_VERSION;
    store::set_str("update_skip", skip);
    store::save_now();
    log_message(LOG_OK, "Update", "Switched to the kept version %s; %s is kept now", prev.c_str(), APP_VERSION);
}

}  // namespace

const char* version() { return APP_VERSION; }
bool supported() { return g_supported; }
const std::string& unsupported_reason() { return g_reason; }
State state() { return g_state; }
const Release& release() { return g_release; }
const std::string& error() { return g_error; }
int64_t last_checked() { return g_last_checked; }

float progress() {
    if (!g_job || g_release.size <= 0) return 0;
    return std::clamp((float)g_job->done / (float)g_release.size, 0.0f, 1.0f);
}

bool automatic() { return store::get_bool("update_auto", false); }
void set_automatic(bool on) {
    store::set_bool("update_auto", on);
    if (on) g_declined = false;
}

void init() {
    g_started = util::now_seconds();
    g_last_checked = store::get_int("update_checked", 0);

    // What the last exit did.
    std::string installed = store::get_str("update_installed"), failed = store::get_str("update_error");
    if (!installed.empty()) {
        store::set_str("update_installed", "");
        if (installed == APP_VERSION) ui::toast(util::fmt("Now running CoffeeFlix %s", APP_VERSION), ic::CHECK_CIRCLE);
    }
    if (!failed.empty()) {
        store::set_str("update_error", "");
        ui::toast("The update didn't install: " + failed, ic::ERROR_OUTLINE);
    }

    if (numbers(APP_VERSION).empty()) {
        g_reason = util::fmt("This test build can't tell which release is newer. Get new versions from %s.", RELEASES_PAGE);
        return;
    }
    g_bundle = platform::app_bundle();
    if (g_bundle.empty()) {
        g_reason = platform::is_wiiu()
                       ? util::fmt("Updating here works when CoffeeFlix runs as coffeeflix.wuhb from the Wii U Menu "
                                   "(Aroma). Get new versions from %s.", RELEASES_PAGE)
                       : std::string("Updating is for the Wii U app.");
        return;
    }
    g_supported = true;
    log_message(LOG_OK, "Update", "CoffeeFlix %s from %s", APP_VERSION, g_bundle.c_str());

    std::string dl = g_bundle + ".download", bak = g_bundle + ".bak", aside = g_bundle + ".old";
    // A switch that stopped after its main step: the other version goes back to being the kept one.
    if (util::file_exists(aside)) {
        if (!util::file_exists(bak)) rename(aside.c_str(), bak.c_str());
        else remove(aside.c_str());
    }
    g_has_previous = util::file_exists(bak);

    Release ready;
    ready.version = store::get_str("update_ready");
    ready.sha256 = store::get_str("update_ready_sha256");
    ready.size = store::get_int("update_ready_size", 0);
    ready.notes = store::get_str("update_ready_notes");
    ready.dev = store::get_bool("update_ready_dev", false);
    bool have = util::file_exists(dl);
    if (have && ready.dev == developer() && (ready.dev || newer(ready.version, APP_VERSION)) && is_sha256(ready.sha256) &&
        file_size(dl) == ready.size) {
        reverify(ready);
    } else if (have || !ready.version.empty()) {
        remove(dl.c_str());
        remember_ready(nullptr);
    }
}

void tick() {
    if (!g_supported) return;
    double now = util::now_seconds();
    // Once a day, a while after start-up so it doesn't compete with the first screens. Developer
    // builds: at every start (the Updates screen looks again).
    bool settled = g_state == IDLE || g_state == UP_TO_DATE || g_state == FAILED;
    bool due = developer() ? !g_dev_checked : util::unix_time() - g_last_checked >= DAY;
    if (settled && due && now - g_started > 20 && now >= g_next_check && platform::network_connected()) {
        g_next_check = now + 3600;
        g_dev_checked = true;
        start_check(true);
    }
    if (g_state == AVAILABLE && automatic() && !g_declined && now >= g_next_download &&
        store::get_str("update_skip") != key(g_release) && !player_busy()) {
        g_next_download = now + 6 * 3600;  // after a failed attempt
        download();
    }
}

void stop() {
    if (g_job) g_job->cancel = true;
}

void check() {
    if (!g_supported || g_state == CHECKING || g_state == DOWNLOADING || g_state == READY) return;
    start_check(false);
}

void download() {
    if (!g_supported || (g_state != AVAILABLE && g_state != FAILED) || g_release.url.empty()) return;
    g_state = DOWNLOADING;
    g_error.clear();
    g_job = std::make_shared<Job>();
    std::shared_ptr<Job> job = g_job;
    Release rel = g_release;
    std::string dest = g_bundle + ".download";
    log_message(LOG_OK, "Update", "Downloading CoffeeFlix %s (%s)", rel.version.c_str(), util::format_bytes(rel.size).c_str());
    g_scope.run<std::string>([rel, dest, job] { return fetch(rel, dest, *job); }, [rel, job](std::string err) {
        if (job != g_job) return;
        g_job.reset();
        if (err == "Cancelled") {
            g_state = AVAILABLE;
            return;
        }
        if (!err.empty()) {
            log_message(LOG_WARNING, "Update", "Downloading %s failed: %s", rel.version.c_str(), err.c_str());
            g_state = FAILED;
            g_error = err;
            return;
        }
        remember_ready(&rel);
        g_state = READY;
        log_message(LOG_OK, "Update", "%s is downloaded and matches %s's checksum", rel.version.c_str(), source(rel));
        ui::toast(util::fmt("%s %s is ready. It installs when you close CoffeeFlix.", rel.dev ? "Developer build" : "CoffeeFlix",
                            rel.version.c_str()),
                  ic::CHECK_CIRCLE);
    });
}

void cancel() {
    g_declined = true;
    if (g_job) {
        g_job->cancel = true;  // the download ends, deletes its file, then goes back to AVAILABLE
        return;
    }
    if (g_state == READY) {
        remove((g_bundle + ".download").c_str());
        remember_ready(nullptr);
        g_state = AVAILABLE;
        log_message(LOG_OK, "Update", "Won't install %s", g_release.version.c_str());
    }
}

bool cancelling() { return g_job && g_job->cancel; }

void install_now() {
    if (g_state == READY) platform::exit_to_menu();
}

bool has_previous() { return g_supported && g_has_previous; }
std::string previous_version() { return store::get_str("update_prev_version"); }

void switch_to_previous() {
    if (!has_previous()) return;
    stop();
    g_switch_back = true;
    platform::exit_to_menu();
}

bool dev_unlocked() { return store::get_bool("update_dev_unlocked", false); }

bool set_dev_unlocked(bool on) {
    if (!on && !set_developer(false)) return false;
    store::set_bool("update_dev_unlocked", on);
    log_message(LOG_OK, "Update", "Developer updates %s", on ? "unlocked" : "hidden");
    return true;
}

bool developer() { return dev_unlocked() && store::get_bool("update_dev", false); }

bool set_developer(bool on) {
    if (on == developer()) return true;
    if (g_state == DOWNLOADING) return false;
    store::set_bool("update_dev", on);
    // What the other kind of update had found or downloaded is dropped.
    if (g_job) {
        g_job->cancel = true;  // rechecking a download from before
        g_job.reset();
    }
    remove((g_bundle + ".download").c_str());
    remember_ready(nullptr);
    g_release = Release();
    g_state = IDLE;
    g_declined = false;
    g_dev_checked = true;
    log_message(LOG_OK, "Update", "Updates now come from %s", on ? "the developer's computer" : "GitHub releases");
    if (g_supported) start_check(false);
    return true;
}

const std::string& dev_server() { return g_dev_server; }

void finish_on_exit() {
    if (!g_supported) return;
    if (g_switch_back) swap_back();
    else if (g_state == READY) install();
}

}  // namespace updater
