// Files in the folder browsers: kinds, sorting, the subtitle files that go with a video, and
// renaming, deleting and making folders on the SD card. Nothing here draws, so
// tools/tests/media_files_test.cpp can try it on a temporary folder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct stat;  // <sys/stat.h>

namespace screens::media {

enum Kind { K_DIR, K_VIDEO, K_AUDIO, K_IMAGE, K_BOOK, K_OTHER };

// A subtitle file next to a video: "Movie.srt", "Movie.en.srt", "Movie.English.ass"...
struct Sidecar {
    std::string name, path;
    std::string label;     // "English", "Subtitles" (in the current language)
    std::string language;  // ISO 639-1 code ("en"); empty when the name doesn't tell
};

struct Entry {
    std::string name, path;
    Kind kind = K_OTHER;
    uint64_t size = 0;
    int64_t mtime = 0;          // last modified (unix seconds); local listings only
    bool link = false;          // a symbolic link (local listings)
    std::vector<Sidecar> subs;  // a video's subtitle files, best first (local listings)
    bool subs_listed = false;   // subs is what the folder has; else they're looked up by name
};

Kind kind_of(const std::string& name);  // by extension
std::string strip_ext(const std::string& name);
bool is_subtitle_file(const std::string& name);  // .srt .vtt .ass .ssa

enum SortMode { SORT_NAME, SORT_NEWEST, SORT_SIZE };
// Folders first, then by name (natural order), newest or biggest first; folders have no size,
// so they stay by name then.
void sort_entries(std::vector<Entry>& entries, SortMode mode = SORT_NAME);
const char* sort_key(SortMode mode);  // for the settings: "name", "newest", "size"
SortMode sort_from_key(const std::string& key, SortMode def);

// Whether `sub` is a subtitle file for the video `video` (file names in the same folder), and
// its label. "<video name>.forced.srt" and other tags that aren't a language don't count.
bool match_sidecar(const std::string& video, const std::string& sub, Sidecar& out);
// Every subtitle file for `video` among `names` (a folder's files) in `dir`, best first: the
// one in the app's language (`ui_language`, "es"), then one without a language, then the rest.
std::vector<Sidecar> find_sidecars(const std::string& dir, const std::string& video, const std::vector<std::string>& names,
                                   const std::string& ui_language);
void order_sidecars(std::vector<Sidecar>& subs, const std::string& ui_language);
// "en", "eng", "English", "pt-BR" -> the language's English name ("English"), else nullptr.
const char* language_name(const std::string& tag);

// What's wrong with a name typed for a file or folder (in the current language), empty when
// it's fine: slashes and characters memory cards can't store, a leading dot, too long.
std::string name_problem(const std::string& name);
// A file keeps its extension: "Holiday" for "IMG_0042.MP4" -> "Holiday.MP4".
std::string renamed(const std::string& old_name, const std::string& typed, bool is_dir);
// Whether path is somewhere below root (not root itself), by its text.
bool inside(const std::string& path, const std::string& root);

// lstat(): links are told apart and never followed (stat() on the Wii U, whose SD card has none).
int link_stat(const std::string& path, struct stat* st);

// Blocking file operations. Each refuses anything outside `root` and symbolic links.
struct FileOp {
    bool ok = false;
    std::string error;  // for a toast
    std::string path;   // what was renamed / made
};
// A video's subtitle files (e.subs) are renamed along with it, so they stay paired.
FileOp rename_entry(const std::string& root, const Entry& e, const std::string& typed);
// A file, and a video's subtitle files.
FileOp delete_file(const std::string& root, const Entry& e);
// Only an empty folder (the junk computers leave, like .DS_Store, aside).
FileOp delete_folder(const std::string& root, const std::string& dir);
FileOp make_folder(const std::string& root, const std::string& parent, const std::string& typed);

}  // namespace screens::media
