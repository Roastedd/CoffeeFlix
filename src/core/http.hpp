// Blocking HTTP client (libcurl). Call from worker threads via tasks::run.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace http {

struct Response;

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeout = 20;                         // seconds, whole transfer
    size_t max_bytes = 32 * 1024 * 1024;       // abort larger responses
    const std::atomic<bool>* cancel = nullptr; // abort when set
    // Hands the body over as it arrives instead of collecting it in Response::body (the
    // status and headers are already in the response). Return false to stop the transfer.
    std::function<bool(const Response& so_far, const char* data, size_t size)> on_data;
    // Media downloads: new sockets get the socket_setup from init() (large receive buffers).
    bool big_buffers = false;
};

struct Response {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string error;
    std::string effective_url;

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

Response get(const std::string& url, std::vector<std::pair<std::string, std::string>> headers = {}, long timeout = 20);
Response post_json(const std::string& url, const std::string& body,
                   std::vector<std::pair<std::string, std::string>> headers = {}, long timeout = 20);

const char* user_agent();

}  // namespace http
