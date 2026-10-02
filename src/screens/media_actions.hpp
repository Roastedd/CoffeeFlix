// Folder listings and opening files, shared by the SD card browser (My Media)
// and the network share browser so both behave the same.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "screens/media_files.hpp"  // Entry, kinds, sorting, subtitle files
#include "screens/widgets.hpp"

namespace screens::media {

int kind_icon(Kind k);

// Grid card for an entry; `service` is where its resume point is kept.
CardInfo entry_card(const Entry& e, const char* service);

// Opens a file the way a file manager would: videos play (resuming, with
// sidecar subtitles), music queues the folder's tracks, photos open the viewer
// on the folder's pictures. `service` keys resume points ("local", "smb") and
// `exists` looks up sidecar files (subtitles, cover art) by path; a video's
// subtitles come from Entry::subs instead when the listing found them.
void open_file(const std::vector<Entry>& siblings, size_t index, const char* service,
               const std::function<bool(const std::string&)>& exists);

}  // namespace screens::media
