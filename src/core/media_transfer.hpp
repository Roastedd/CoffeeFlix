// Temporary, token-protected LAN receiver. Owns its worker and never exposes local files over HTTP.
//
// Protocol (all requests need the session token in X-Upload-Key and the exact Host):
//   GET  <base>/          the upload page
//   GET  <base>/info      {"free":bytes|-1,"max":bytes,"folders":["Anime",...]}
//   GET  <base>/upload    resume query. X-Upload-Id, X-Total-Size -> {"offset":bytes already kept}
//   PUT  <base>/upload    the file, or the rest of it. X-File-Name (URL-encoded). Optional:
//                           X-Upload-Id     8-64 lowercase hex characters; the partial file is KEPT if the
//                                           connection breaks, and a later PUT continues it
//                           X-Upload-Offset where this body starts (0: start over); must equal the offset
//                                           the resume query gave, else 409 {"offset":n} to correct it
//                           X-Total-Size    the whole file (default: offset + Content-Length)
//                           X-Folder        one folder below Received to save in (URL-encoded)
//                           X-Queue-Index / X-Queue-Count  "file 2 of 5", for the Wii U screen
//                       200 {"saved":"name"} when the file is complete, 200 {"offset":n} after a body that
//                       left it incomplete.
// Without X-Upload-Id a PUT is one whole file that is removed if anything goes wrong.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
namespace media_transfer {
constexpr uint64_t MAX_FILE = 2147483647; // portable signed file offsets on Wii U
struct Status {
    bool active = false;
    uint64_t received = 0, total = 0;   // of the file, including what an earlier attempt kept
    uint64_t resumed_from = 0;          // where this attempt started
    int completed = 0;
    int queue_index = 0, queue_count = 0; // "file 2 of 5" (0: the sender didn't say)
    double bytes_per_second = 0;        // smoothed over the last few seconds (0: not measured yet)
    double seconds_left = -1;           // at that speed (<0: unknown)
    bool interrupted = false;           // the last attempt broke and its partial file was kept
    int64_t free_bytes = -1;
    std::string filename, saved_name, folder, error;
    double started = 0;
};
class Server {
public:
    explicit Server(std::string folder);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    const std::string& url() const { return url_; }
    Status status() const;
    void cancel();
    struct State;
private:
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::string url_;
};
bool valid_name(const std::string& name);
bool valid_folder(const std::string& name);
bool valid_upload_id(const std::string& id);
}
