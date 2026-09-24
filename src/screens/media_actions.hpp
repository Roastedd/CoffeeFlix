// Folder listings and opening files, shared by the SD card browser (My Media)
// and the network share browser so both behave the same.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "screens/widgets.hpp"

namespace screens::media {

enum Kind { K_DIR, K_VIDEO, K_AUDIO, K_IMAGE, K_BOOK, K_OTHER };

struct Entry {
    std::string name, path;
    Kind kind = K_OTHER;
    uint64_t size = 0;
};

Kind kind_of(const std::string& name);  // by extension
int kind_icon(Kind k);
// Natural sort: "Episode 2" before "Episode 10".
bool natural_less(const std::string& a, const std::string& b);
void sort_entries(std::vector<Entry>& entries);  // folders first, then natural order
std::string strip_ext(const std::string& name);

// Grid card for an entry; `service` is where its resume point is kept.
CardInfo entry_card(const Entry& e, const char* service);

// Opens a file the way a file manager would: videos play (resuming, with
// sidecar subtitles), music queues the folder's tracks, photos open the viewer
// on the folder's pictures. `service` keys resume points ("local", "smb") and
// `exists` looks up sidecar files (subtitles, cover art) by path.
void open_file(const std::vector<Entry>& siblings, size_t index, const char* service,
               const std::function<bool(const std::string&)>& exists);

}  // namespace screens::media
