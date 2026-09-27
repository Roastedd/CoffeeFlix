// Blocking HTTP client (libcurl). Call from worker threads via tasks::run.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace http {

struct Response;
class Connection;

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeout = 20;                         // seconds, whole transfer
    // Gives up on a transfer that got under 1 KB a second for this many seconds (a connection
    // gone quiet) instead of waiting out `timeout`. 0: only the timeout.
    long stall_seconds = 0;
    size_t max_bytes = 32 * 1024 * 1024;       // abort larger responses
    const std::atomic<bool>* cancel = nullptr; // abort when set
    // Asked now and then while the transfer runs (at least once a second), with when the request
    // went out (0 while it is still connecting): false gives up on it, as `cancel` does.
    std::function<bool(double sent_at)> keep_going;
    // Hands the body over as it arrives instead of collecting it in Response::body (the
    // status and headers are already in the response). Return false to stop the transfer.
    std::function<bool(const Response& so_far, const char* data, size_t size)> on_data;
    // Media downloads: new sockets get the socket_setup from init() (large receive buffers).
    bool big_buffers = false;
    // Checks the server's certificate even when set_verify_tls(false), and follows redirects to
    // https only: for what gets installed (app updates).
    bool require_tls = false;
    // Made on this connection rather than one from the shared pool (see Connection).
    Connection* connection = nullptr;
    // On a new connection even when one to the host is open: after giving up on a transfer that
    // went quiet (over HTTP/2 curl would keep the connection it was on).
    bool fresh_connection = false;
};

struct Response {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string error;
    std::string effective_url;
    long connects = 0;  // connections the request had to open (0: it reused one)

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// socket_setup, when given, runs on new sockets of big_buffers requests before they connect
// (Wii U buffer sizes).
void init(const std::string& ca_bundle_path, void (*socket_setup)(int fd) = nullptr);
void shutdown();
void set_verify_tls(bool verify);
bool verify_tls();
const std::string& ca_bundle();

Response perform(const Request& req);

// A connection of one caller's own (a download thread of the player's), apart from the pool the
// other requests share: its next request goes out on the same connection, which nothing else
// can take over in between (and close, for one to another server).
class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

private:
    friend Response perform(const Request& req);
    void* curl_ = nullptr;
};

Response get(const std::string& url, std::vector<std::pair<std::string, std::string>> headers = {}, long timeout = 20);
Response post_json(const std::string& url, const std::string& body,
                   std::vector<std::pair<std::string, std::string>> headers = {}, long timeout = 20);

const char* user_agent();

}  // namespace http
