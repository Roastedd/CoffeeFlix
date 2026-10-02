#include "core/media_transfer.hpp"
#include "core/util.hpp"
#include "core/cpu.hpp"
#include "core/json.hpp"
#include "core/i18n.hpp"
#include "platform/platform.hpp"
#include "logger/logger.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __WIIU__
#include <coreinit/filesystem.h>
#include <malloc.h>
#endif
namespace media_transfer {
struct Server::State {
    std::atomic<bool> stop{false};
    std::atomic<unsigned> cancel{0};
    int listener = -1;
    unsigned uploads = 0;
    std::string folder, token, path, authority, page;
    mutable std::mutex mutex;
    Status status;
};
namespace {
constexpr uint64_t RESERVE = 8 << 20;
std::string session_key() { return util::secure_random_hex(16); }
int64_t free_space(const std::string& path) {
    struct statvfs info{};
    if (statvfs(path.c_str(), &info) == 0) return (int64_t)info.f_bavail * (info.f_frsize ? info.f_frsize : info.f_bsize);
#ifdef __WIIU__
    auto* client = (FSClient*)memalign(64, sizeof(FSClient));
    auto* command = (FSCmdBlock*)memalign(64, sizeof(FSCmdBlock));
    int64_t result = -1;
    if (client && command && FSAddClient(client, FS_ERROR_FLAG_ALL) >= 0) {
        FSInitCmdBlock(command); uint64_t bytes = 0;
        if (FSGetFreeSpaceSize(client, command, path.c_str(), &bytes, FS_ERROR_FLAG_ALL) >= 0) result = (int64_t)bytes;
        FSDelClient(client, FS_ERROR_FLAG_ALL);
    }
    free(command); free(client); return result;
#else
    return -1;
#endif
}
bool send_all(int fd, const std::string& data) {
    size_t at = 0; const double deadline = util::now_seconds() + 2;
    while (at < data.size()) {
        if (util::now_seconds() > deadline) return false;
        pollfd ready{fd, POLLOUT, 0}; if (poll(&ready, 1, 100) <= 0) continue;
#if defined(MSG_NOSIGNAL) && !defined(__WIIU__)
        const int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
        const int flags = MSG_DONTWAIT;
#endif
        auto n = send(fd, data.data() + at, data.size() - at, flags);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n <= 0) return false;
        at += (size_t)n;
    }
    return true;
}
void answer(int fd, int status, const std::string& body, const std::string& token, bool html = false) {
    send_all(fd, util::fmt("HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nContent-Type: %s; charset=utf-8\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'nonce-%s'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n",
        status, status == 200 ? "OK" : "Error", body.size(), html ? "text/html" : "application/json", token.c_str()) + body);
}
std::string result(const char* key, const std::string& value) {
    json::Doc d(json_object()); json_object_set_new(d.get(), key, json_string(value.c_str())); return json::dump(d.get());
}
// {"<key>": value, "offset": n}: either part may be left out (key null / offset < 0).
std::string reply(const char* key, const std::string& value, int64_t offset = -1) {
    json::Doc d(json_object());
    if (key) json_object_set_new(d.get(), key, json_string(value.c_str()));
    if (offset >= 0) json_object_set_new(d.get(), "offset", json_integer((json_int_t)offset));
    return json::dump(d.get());
}
std::string partial_path(const std::string& folder, const std::string& id, uint64_t total) {
    return util::join_path(folder, ".resume-" + id + "-" + std::to_string(total) + ".part");
}
// Size of a regular file, -1 when it doesn't exist, -2 when something else is there (a link, a folder).
int64_t partial_size(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT ? -1 : -2;
    return S_ISREG(st.st_mode) ? (int64_t)st.st_size : -2;
}
// Leftovers of earlier sessions: their hidden temporary files can't be continued (the session
// token changed) and a resumable partial only waits for a week.
void remove_stale(const std::string& folder) {
    DIR* dir = opendir(folder.c_str()); if (!dir) return;
    const time_t now = time(nullptr); std::vector<std::string> doomed;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 10 || name.compare(name.size() - 5, 5, ".part") != 0) continue;
        const auto path = util::join_path(folder, name); struct stat st{};
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (util::starts_with(name, ".upload-") || (util::starts_with(name, ".resume-") && now - st.st_mtime > 7 * 24 * 3600)) doomed.push_back(path);
    }
    closedir(dir);
    for (const auto& path : doomed) remove(path.c_str());
}
bool number(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 12 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    out = 0; for (char c : s) out = out * 10 + c - '0'; return true;
}
bool available(Server::State& s, uint64_t size) {
    const int64_t free = free_space(s.folder);
    { std::lock_guard<std::mutex> lock(s.mutex); s.status.free_bytes = free; }
    return free < 0 || (uint64_t)free >= size + RESERVE;
}
// Only one writer exists. Reserve the final name exclusively before replacing our
// own empty reservation; existing files and symlinks can never be selected.
std::string publish(const std::string& folder, const std::string& name, const std::string& temp) {
    const size_t dot = name.rfind('.');
    for (int n = 1; n <= 999; ++n) {
        const auto candidate = n == 1 ? name : name.substr(0, dot) + " (" + std::to_string(n) + ")" + name.substr(dot);
        const auto path = util::join_path(folder, candidate);
#ifdef __WIIU__
        // Wii U/FAT rename refuses an existing destination (unlike POSIX rename).
        // Do not reserve an empty target: that would make every FAT rename fail.
        struct stat exists{};
        if (lstat(path.c_str(), &exists) == 0) continue;
        if (errno != ENOENT) return "";
        if (rename(temp.c_str(), path.c_str()) == 0) return candidate;
        if (errno == EEXIST) continue;
        return "";
#else
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return ""; }
        if (rename(temp.c_str(), path.c_str()) == 0) { close(fd); return candidate; }
        close(fd); remove(path.c_str()); return "";
#endif
    }
    return "";
}
void serve(Server::State& s, int fd) {
    std::string request; size_t end = std::string::npos; double last = util::now_seconds();
    char bytes[64 << 10];
    while (!s.stop && (end = request.find("\r\n\r\n")) == std::string::npos) {
        if (request.size() >= 8192 || util::now_seconds() - last > 10) return;
        pollfd p{fd, POLLIN, 0}; if (poll(&p, 1, 200) <= 0) continue;
        auto n = recv(fd, bytes, std::min<size_t>(sizeof(bytes), 8192 - request.size()), 0);
        if (n <= 0) return;
        request.append(bytes, (size_t)n);
    }
    if (s.stop || end == std::string::npos) return;
    auto error = [&](int code, const char* message) { answer(fd, code, result("error", tr(message)), s.token); };
    auto lines = util::split(request.substr(0, end), '\n');
    auto first = util::split(util::trim(lines[0]), ' ');
    if (first.size() != 3 || first[2] != "HTTP/1.1") { error(400, N_("Invalid transfer request")); return; }
    std::map<std::string, std::string> headers;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto colon = lines[i].find(':');
        if (colon == std::string::npos) { error(400, N_("Invalid transfer request")); return; }
        auto name = util::lower(util::trim(lines[i].substr(0, colon)));
        if (!headers.emplace(name, util::trim(lines[i].substr(colon + 1))).second) { error(400, N_("Invalid transfer request")); return; }
    }
    auto header = [&](const char* name) { auto it = headers.find(name); return it == headers.end() ? std::string() : it->second; };
    if (header("host") != s.authority || (headers.count("origin") && header("origin") != "http://" + s.authority) ||
        header("sec-fetch-site") == "cross-site") { error(403, N_("Open the link shown on your Wii U")); return; }
    if (first[0] == "GET" && first[1] == s.path) { answer(fd, 200, s.page, s.token, true); return; }
    const bool known = first[1] == s.path + "upload" || first[1] == s.path + "info";
    if (!known || header("x-upload-key") != s.token || (first[0] != "PUT" && first[0] != "GET") || (first[0] == "PUT" && first[1] != s.path + "upload")) {
        error(404, N_("Open the link shown on your Wii U")); return;
    }
    if (first[1] == s.path + "info") {
        json::Doc d(json_object()); auto* folders = json_array();
        json_object_set_new(d.get(), "free", json_integer((json_int_t)free_space(s.folder)));
        json_object_set_new(d.get(), "max", json_integer((json_int_t)MAX_FILE));
        if (DIR* dir = opendir(s.folder.c_str())) {
            while (dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name; struct stat st{};
                if (valid_folder(name) && lstat(util::join_path(s.folder, name).c_str(), &st) == 0 && S_ISDIR(st.st_mode) && json_array_size(folders) < 50)
                    json_array_append_new(folders, json_string(name.c_str()));
            }
            closedir(dir);
        }
        json_object_set_new(d.get(), "folders", folders);
        answer(fd, 200, json::dump(d.get()), s.token); return;
    }
    // Resume query: how much of this file an earlier attempt kept.
    if (first[0] == "GET") {
        uint64_t whole = 0; const auto& id = header("x-upload-id");
        if (!valid_upload_id(id) || !number(header("x-total-size"), whole) || !whole || whole > MAX_FILE) { error(400, N_("Invalid transfer request")); return; }
        const int64_t have = partial_size(partial_path(s.folder, id, whole));
        answer(fd, 200, reply(nullptr, "", have > 0 && (uint64_t)have <= whole ? have : 0), s.token); return;
    }
    uint64_t len = 0, offset = 0, total = 0;
    if (headers.count("transfer-encoding") || !number(header("content-length"), len)) { error(411, N_("Invalid transfer request")); return; }
    const std::string id = header("x-upload-id"); const bool resumable = headers.count("x-upload-id") > 0;
    bool bad = (resumable && !valid_upload_id(id)) || (headers.count("x-upload-offset") && !number(header("x-upload-offset"), offset));
    if (!bad) { if (headers.count("x-total-size")) bad = !number(header("x-total-size"), total); else total = offset + len; }
    if (bad || (!resumable && (offset || total != len))) { error(400, N_("Invalid transfer request")); return; }
    if (!total || total > MAX_FILE) { error(413, N_("Choose a file smaller than 2 GB")); return; }
    if (offset > total || len > total - offset || (!len && !(resumable && offset == total))) { error(400, N_("Invalid transfer request")); return; }
    const auto name = util::url_decode(header("x-file-name"));
    if (!valid_name(name)) { error(400, N_("Unsupported file type or filename")); return; }
    const auto folder_name = headers.count("x-folder") ? util::url_decode(header("x-folder")) : std::string();
    std::string dir = s.folder;
    if (!folder_name.empty()) {
        struct stat st{}; dir = util::join_path(s.folder, folder_name);
        bool ok = valid_folder(folder_name);
        if (ok && lstat(dir.c_str(), &st) != 0) ok = util::make_dirs(dir) && lstat(dir.c_str(), &st) == 0;
        if (!ok || !S_ISDIR(st.st_mode)) { error(400, N_("Unsupported folder name")); return; }
    }
    uint64_t queue_index = 0, queue_count = 0;
    if (!number(headers.count("x-queue-index") ? header("x-queue-index") : std::string("0"), queue_index) || !number(headers.count("x-queue-count") ? header("x-queue-count") : std::string("0"), queue_count) || queue_index > 999 || queue_count > 999) queue_index = queue_count = 0;
    const unsigned cancel = s.cancel.load();
    std::string temp;
    int out = -1;
    if (resumable) {
        temp = partial_path(s.folder, id, total);
        const int64_t have = partial_size(temp);
        if (have == -2) { error(507, N_("Couldn't save the file")); return; }
        // A body that doesn't start where the kept bytes end tells the sender where they do (0 starts over).
        if (offset && (uint64_t)std::max<int64_t>(have, 0) != offset) {
            answer(fd, 409, reply("error", tr("Transfer interrupted. Try again."), std::max<int64_t>(have, 0)), s.token); return;
        }
        if (!available(s, total - offset)) { error(507, N_("Not enough free space on the SD card")); return; }
        out = open(temp.c_str(), O_WRONLY | O_CREAT | (offset ? 0 : O_TRUNC), 0600);
        if (out >= 0 && offset && lseek(out, (off_t)offset, SEEK_SET) != (off_t)offset) { close(out); out = -1; }
    } else {
        if (!available(s, total)) { error(507, N_("Not enough free space on the SD card")); return; }
        temp = util::join_path(s.folder, ".upload-" + s.token + "-" + std::to_string(++s.uploads) + ".part");
        out = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (out < 0) { error(507, N_("Couldn't save the file")); return; }
    { std::lock_guard<std::mutex> lock(s.mutex); s.status.active = true; s.status.filename = name; s.status.folder = folder_name;
      s.status.received = offset; s.status.resumed_from = offset; s.status.total = total; s.status.started = util::now_seconds();
      s.status.error.clear(); s.status.interrupted = false; s.status.bytes_per_second = 0; s.status.seconds_left = -1;
      s.status.queue_index = (int)queue_index; s.status.queue_count = (int)queue_count; }
    uint64_t received = 0; std::string failure; bool disk_trouble = false;
    double sample_at = util::now_seconds(), speed = 0; uint64_t sample_bytes = 0;
    auto write_bytes = [&](const char* data, size_t size) {
        if (size > len - received) return false;
        size_t at = 0;
        while (at < size) { auto n = write(out, data + at, size - at); if (n <= 0) return false; at += (size_t)n; }
        received += size;
        std::lock_guard<std::mutex> lock(s.mutex); s.status.received = offset + received;
        // Speed over the last few seconds (smoothed), and what it leaves.
        const double now = util::now_seconds();
        if (now - sample_at >= 0.5) {
            const double instant = (double)(received - sample_bytes) / (now - sample_at);
            speed = speed > 0 ? speed * 0.7 + instant * 0.3 : instant; sample_at = now; sample_bytes = received;
            s.status.bytes_per_second = speed; s.status.seconds_left = speed > 1 ? (double)(total - offset - received) / speed : -1;
        }
        return true;
    };
    if (!write_bytes(request.data() + end + 4, request.size() - end - 4)) { failure = tr("Couldn't save the file"); disk_trouble = true; }
    last = util::now_seconds();
    while (failure.empty() && received < len && !s.stop && s.cancel == cancel) {
        const double idle = util::now_seconds() - last;
        if (idle > 30) { failure = tr("Transfer interrupted. Try again."); break; }
        pollfd p[2] = {{fd, POLLIN, 0}, {s.listener, POLLIN, 0}};
        if (poll(p, 2, 200) <= 0) continue;
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            // Only a new connection is waiting. After a Wi-Fi drop the sender comes back on a new one while
            // this one never hears that the old one is gone: give it up (the kept bytes continue on the new one).
            if (idle > 1.5) { failure = tr("Transfer interrupted. Try again."); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue;
        }
        auto n = recv(fd, bytes, (size_t)std::min<uint64_t>(sizeof(bytes), len - received), 0);
        if (n <= 0) { failure = tr("Transfer interrupted. Try again."); break; }
        if (!write_bytes(bytes, (size_t)n)) { failure = tr("Couldn't save the file"); disk_trouble = true; break; }
        last = util::now_seconds();
    }
    const bool cancelled = s.stop || s.cancel != cancel;
    if (cancelled) failure = tr("Transfer cancelled");
    if (failure.empty() && fsync(out) != 0 && errno != ENOSYS && errno != EINVAL) { failure = tr("Couldn't save the file"); disk_trouble = true; }
    if (close(out) != 0) { failure = tr("Couldn't save the file"); disk_trouble = true; }
    const bool complete = failure.empty() && offset + received == total;
    std::string saved;
    if (complete) saved = publish(dir, name, temp);
    if (complete && saved.empty()) { failure = tr("Couldn't save the file"); disk_trouble = true; }
    // A resumable upload that only broke off keeps its bytes; cancelling on the Wii U or a disk problem discards them.
    const bool keep = !failure.empty() && resumable && !(s.cancel != cancel) && !disk_trouble;
    if (!failure.empty() && !keep) remove(temp.c_str());
    const int64_t free = free_space(s.folder);
    { std::lock_guard<std::mutex> lock(s.mutex); s.status.active = false; s.status.free_bytes = free; s.status.interrupted = keep;
      s.status.error = failure; s.status.bytes_per_second = 0; s.status.seconds_left = -1;
      if (!saved.empty()) { ++s.status.completed; s.status.saved_name = folder_name.empty() ? saved : folder_name + "/" + saved; } }
    if (!saved.empty()) { log_message(LOG_OK, "Transfer", "Received %llu bytes%s", (unsigned long long)total, offset ? " (resumed)" : ""); answer(fd, 200, result("saved", saved), s.token); }
    else if (failure.empty()) answer(fd, 200, reply(nullptr, "", (int64_t)(offset + received)), s.token);
    else answer(fd, 409, reply("error", failure, keep ? (int64_t)(offset + received) : -1), s.token);
}
std::string page(const std::string& token) {
    std::string html; if (!util::read_file(platform::content_dir() + "/transfer.html", html)) return "";
    json::Doc words(json_object());
    static const char* const labels[] = {
        N_("Receive files"), N_("Send to your Wii U"), N_("Videos, music, photos and subtitles"), N_("Choose files"), N_("or drop files here"),
        N_("Keep this page and the Wii U receiver open until the transfer finishes."),
        N_("Up to 2 GB per file. H.264 video works best; files are not converted."), N_("Upload"), N_("Cancel"), N_("Retry"), N_("Waiting"),
        N_("Sending"), N_("Saved"), N_("Transfer cancelled"), N_("Transfer interrupted. Try again."), N_("Choose a file smaller than 2 GB"),
        N_("Ready to receive"), N_("All files saved"), N_("Close this page when you're done."), N_("Sending files"), N_("Received files"),
        N_("Ready to play"), N_("Plays with limits"), N_("Needs conversion"), N_("HEVC (H.265) video needs conversion"),
        N_("Video format needs conversion"), N_("10-bit video needs conversion"), N_("Larger than 1080p, needs conversion"),
        N_("HDR video needs conversion"), N_("1080p at 60 fps shows about 45 pictures a second"), N_("No sound: unsupported audio"),
        N_("Plays with software decoding, may be slow"), N_("Couldn't check this file"), N_("Folder (optional)"), N_("%s left"), N_("File %d of %d"),
        N_("Reconnecting…"), N_("Resuming from %s"), N_("Not enough free space on the SD card"), N_("%s free on SD card"), N_("Unsupported file type or filename"), N_("Unsupported folder name"),
        N_("Too big for the Wii U. Converted to 1080p it would be about %s, or about %s at 720p.")
    };
    for (const char* label : labels)
        json_object_set_new(words.get(), label, json_string(tr(label)));
    html = util::replace_all(html, "{{TOKEN}}", token);
    // The dictionary is application-owned text; escaping < also prevents script termination in translations.
    return util::replace_all(html, "{{WORDS}}", util::replace_all(json::dump(words.get()), "<", "\\u003c"));
}
// After an early answer the sender may still be sending the body. Closing with unread data resets the
// connection and the sender never sees the answer, so read (and drop) what is left for a moment.
void linger(int fd, int listener) {
    char sink[16 << 10]; size_t left = 8 << 20; const double end = util::now_seconds() + 1.5;
    while (left && util::now_seconds() < end) {
        pollfd p[2] = {{fd, POLLIN, 0}, {listener, POLLIN, 0}};
        if (poll(p, 2, 100) <= 0) continue;
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) return; // someone else is waiting
        auto n = recv(fd, sink, std::min(sizeof(sink), left), 0);
        if (n <= 0) return;
        left -= (size_t)n;
    }
}
void run(std::shared_ptr<Server::State> s) {
    cpu::ThreadTag tag("receive files");
    while (!s->stop) {
        pollfd p{s->listener, POLLIN, 0}; if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(s->listener, nullptr, nullptr); if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        serve(*s, fd); linger(fd, s->listener); close(fd);
    }
    close(s->listener);
}
}
bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 180 || name[0] == '.' || name.back() == ' ' || name.back() == '.' ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    auto* unicode = json_stringn(name.data(), name.size());
    if (!unicode) return false;
    json_decref(unicode);
    static const std::string allowed = "|mp4|m4v|mkv|webm|avi|mov|ts|m2ts|mpg|mpeg|flv|3gp|mp3|m4a|aac|flac|ogg|opus|wav|wv|alac|oga|mka|jpg|jpeg|png|gif|webp|bmp|cbz|epub|srt|vtt|ass|ssa|";
    const auto ext = util::file_extension(name);
    return !ext.empty() && allowed.find("|" + ext + "|") != std::string::npos;
}
bool valid_folder(const std::string& name) {
    if (name.empty() || name.size() > 60 || name[0] == '.' || name.back() == ' ' || name.back() == '.' ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    auto* unicode = json_stringn(name.data(), name.size());
    if (!unicode) return false;
    json_decref(unicode);
    return true;
}
bool valid_upload_id(const std::string& id) {
    return id.size() >= 8 && id.size() <= 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Server::Server(std::string folder) : state_(std::make_shared<State>()) {
    auto& s = *state_; s.folder = std::move(folder);
    struct stat dir{};
    if (!util::make_dirs(s.folder) || lstat(s.folder.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode)) { s.status.error = tr("Can't open this folder"); return; }
    const auto ip = platform::ip_address(); if (ip.empty() || ip == "0.0.0.0") { s.status.error = tr("Connect to Wi-Fi to receive files"); return; }
    remove_stale(s.folder);
    s.token = session_key();
    if (s.token.empty()) { s.status.error = tr("Couldn't start the receiver"); return; }
    s.path = "/receive/" + s.token + "/"; s.page = page(s.token);
    if (s.page.empty()) { s.status.error = tr("Couldn't open the transfer page"); return; }
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { s.status.error = tr("Couldn't start the receiver"); return; }
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); int port = 0;
    for (int p = 8080; p < 8090 && !port; ++p) {
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(p); addr.sin_addr.s_addr = inet_addr(ip.c_str());
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0) port = p;
    }
    if (!port || listen(fd, 4) != 0) { close(fd); s.status.error = tr("Couldn't start the receiver"); return; }
    s.listener = fd; s.authority = ip + ":" + std::to_string(port); s.status.free_bytes = free_space(s.folder);
    url_ = "http://" + s.authority + s.path; worker_ = std::thread(run, state_);
    log_message(LOG_OK, "Transfer", "Receiver listening on port %d", port);
}
Server::~Server() { state_->stop = true; if (worker_.joinable()) worker_.join(); }
Status Server::status() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->status; }
void Server::cancel() { ++state_->cancel; }
}
