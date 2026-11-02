#include "core/lan_form.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace lan_form {

struct Server::State {
    std::atomic<bool> stop{false};
    int fd = -1;
    std::string path, page, sent;
    std::mutex m;
    std::map<std::string, std::string> fields;
    bool fresh = false;
};

namespace {

constexpr size_t MAX_REQUEST = 64 * 1024;

// "a=1&b=x+y" -> {a: 1, b: x y}
std::map<std::string, std::string> parse_form(const std::string& body) {
    std::map<std::string, std::string> out;
    for (const std::string& part : util::split(body, '&')) {
        size_t eq = part.find('=');
        std::string k = part.substr(0, eq), v = eq == std::string::npos ? "" : part.substr(eq + 1);
        out[util::url_decode(util::replace_all(k, "+", " "))] = util::url_decode(util::replace_all(v, "+", " "));
    }
    return out;
}

bool send_all(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
#if defined(MSG_NOSIGNAL) && !defined(__WIIU__)
        ssize_t n = send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);  // Linux
#else
        ssize_t n = send(fd, s.data() + done, s.size() - done, 0);
#endif
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

void answer(int fd, const char* status, const std::string& html) {
    send_all(fd, util::fmt("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           status, html.size()) +
                     html);
}

// One request on a new connection: the page, or a form sent to it.
void serve(Server::State& s, int fd) {
    std::string req;
    size_t head_end = std::string::npos, want = 0;
    while (req.size() < MAX_REQUEST) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 4000) <= 0) return;  // a phone that went quiet
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        if (head_end == std::string::npos) {
            head_end = req.find("\r\n\r\n");
            if (head_end == std::string::npos) continue;
            std::string head = util::lower(req.substr(0, head_end));
            size_t cl = head.find("\r\ncontent-length:");
            want = head_end + 4 + (cl == std::string::npos ? 0 : (size_t)atol(head.c_str() + cl + 17));
        }
        if (req.size() >= want) break;
    }
    if (head_end == std::string::npos) return;
    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    std::string method = req.substr(0, sp1), path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));
    if (path != s.path) {
        answer(fd, "404 Not Found", "<!doctype html><title>404</title>");
        return;
    }
    if (method == "POST") {
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.fields = parse_form(req.substr(head_end + 4));
            s.fresh = true;
        }
        answer(fd, "200 OK", s.sent);
    } else {
        answer(fd, "200 OK", s.page);
    }
}

void run(std::shared_ptr<Server::State> s) {
    while (!s->stop.load()) {
        pollfd p{s->fd, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        sockaddr_in from;
        socklen_t len = sizeof(from);
        int c = accept(s->fd, (sockaddr*)&from, &len);
        if (c < 0) continue;
#ifdef SO_NOSIGPIPE
        int on = 1;  // a phone that left mid-answer mustn't end the app (macOS)
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        serve(*s, c);
        close(c);
    }
    close(s->fd);
}

}  // namespace

Server::Server(std::string page, std::string sent) : s_(std::make_shared<State>()) {
    std::string ip = platform::ip_address();
    if (ip.empty() || ip == "0.0.0.0") return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    int port = 0;
    // 8080 is short to type; a few others in case something has it.
    for (int p = 8080; p < 8090 && !port; p++) {
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(p);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) == 0) port = p;
    }
    if (!port || listen(fd, 4) != 0) {
        close(fd);
        log_message(LOG_WARNING, "LanForm", "No port to serve the page on");
        return;
    }
    // The address is what keeps others on the network from sending to the page.
    const std::string token = util::secure_random_hex(6);
    if (token.empty()) {
        close(fd);
        log_message(LOG_WARNING, "LanForm", "No random numbers for the address of the page");
        return;
    }
    s_->fd = fd;
    s_->path = "/" + token;
    s_->page = std::move(page);
    s_->sent = std::move(sent);
    url_ = util::fmt("http://%s:%d%s", ip.c_str(), port, s_->path.c_str());
    std::thread(run, s_).detach();  // it closes the socket once it sees `stop`
    log_message(LOG_OK, "LanForm", "Page served on port %d", port);
}

Server::~Server() { s_->stop.store(true); }

bool Server::take(std::map<std::string, std::string>& fields) {
    std::lock_guard<std::mutex> lk(s_->m);
    if (!s_->fresh) return false;
    fields = std::move(s_->fields);
    s_->fields.clear();
    s_->fresh = false;
    return true;
}

}  // namespace lan_form
