// SMB (Windows / Samba / NAS) network shares through libsmb2.
//
// Files on a saved share are addressed as smb://<share name>/path/in/share and
// the credentials are looked up by name, so passwords never end up in URLs
// (logs, resume points). smb://[user[:password]@]host/share/path also works for
// shares that aren't saved. Everything except the saved-share list blocks on
// the network: call it from worker threads.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct smb2fh;

namespace smb {

struct Share {
    std::string name;     // display name, also the key in smb:// URLs
    std::string host;     // hostname or IP
    std::string share;    // share name ("Media")
    std::string user, password, domain;
};

std::vector<Share> saved_shares();
// Adds the share (replacing one with the same name); returns the name it was
// saved under (slashes are replaced, empty becomes default_name()).
std::string save_share(const Share& s);
void remove_share(const std::string& name);
// Name to save a share under when the user doesn't pick one ("Media on nas").
std::string default_name(const Share& s);

bool is_url(const std::string& s);
// smb://<name>[/path] for a saved share.
std::string share_url(const Share& s, const std::string& path = "");
// "\\host\share\path" (Windows notation, for display; no credentials).
std::string display_path(const std::string& url);

struct DirEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
};

// Lists a folder (hidden and system files are skipped). On failure returns
// false with a message for the user in `error`.
bool list_dir(const std::string& url, std::vector<DirEntry>& out, std::string& error);
// Connects to a share that isn't saved yet to check the address and login.
bool test_share(const Share& s, std::string& error);
// Reads a whole (small) file: artwork, photos, subtitles.
bool read_file(const std::string& url, std::string& out, size_t max_bytes = 32u << 20);

struct Conn;

// Read-only file for streaming, on a connection of its own.
class File {
public:
    static std::unique_ptr<File> open(const std::string& url, std::string& error);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    int read(uint8_t* buf, int size);          // bytes read, 0 at the end, <0 on error
    int64_t seek(int64_t offset, int whence);  // SEEK_SET/CUR/END; new position or <0
    uint64_t size() const { return size_; }

private:
    File() = default;
    bool reopen(std::string& error);  // (re)connects and opens the file
    void close_handle(bool healthy);

    Share share_;
    std::string path_;
    Conn* conn_ = nullptr;
    smb2fh* fh_ = nullptr;
    uint64_t size_ = 0, pos_ = 0;
};

}  // namespace smb
