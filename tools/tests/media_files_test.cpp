// The folder browser's file logic without the screen: sorting, subtitle files, names, and
// renaming / deleting / making folders in a temporary folder.
//   make -f desktop.mk -f tools/tests/media_files.mk BUILD=<dir> media-files-tests && <dir>/media-files-test
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/util.hpp"
#include "screens/media_files.hpp"

using namespace screens::media;

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!(cond)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " << #cond << "\n";   \
            g_failed++;                                                               \
        }                                                                             \
    } while (0)

bool exists(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

void touch(const std::string& p, const std::string& data = "x") { util::write_file_atomic(p, data); }

Entry file_entry(const std::string& dir, const std::string& name, const std::vector<std::string>& folder_names = {}) {
    Entry e;
    e.name = name;
    e.path = util::join_path(dir, name);
    e.kind = kind_of(name);
    if (e.kind == K_VIDEO) {
        e.subs = find_sidecars(dir, name, folder_names, "en");
        e.subs_listed = true;
    }
    return e;
}

Entry dir_entry(const std::string& dir, const std::string& name) {
    Entry e;
    e.name = name;
    e.path = util::join_path(dir, name);
    e.kind = K_DIR;
    return e;
}

std::vector<std::string> names_of(const std::vector<Entry>& v) {
    std::vector<std::string> out;
    for (const Entry& e : v) out.push_back(e.name);
    return out;
}

void test_sorting() {
    auto make = [](const char* name, Kind k, uint64_t size, int64_t mtime) {
        Entry e;
        e.name = name;
        e.kind = k;
        e.size = size;
        e.mtime = mtime;
        return e;
    };
    std::vector<Entry> v = {make("b.mp4", K_VIDEO, 10, 300),   make("Zeta", K_DIR, 0, 50),
                            make("a10.mp4", K_VIDEO, 30, 100), make("a2.mp4", K_VIDEO, 30, 200),
                            make("Alpha", K_DIR, 0, 400),      make("c.jpg", K_IMAGE, 5, 500)};
    sort_entries(v, SORT_NAME);
    CHECK((names_of(v) == std::vector<std::string>{"Alpha", "Zeta", "a2.mp4", "a10.mp4", "b.mp4", "c.jpg"}));
    sort_entries(v, SORT_NEWEST);
    CHECK((names_of(v) == std::vector<std::string>{"Alpha", "Zeta", "c.jpg", "b.mp4", "a2.mp4", "a10.mp4"}));
    sort_entries(v, SORT_SIZE);  // equal sizes by name; folders by name
    CHECK((names_of(v) == std::vector<std::string>{"Alpha", "Zeta", "a2.mp4", "a10.mp4", "b.mp4", "c.jpg"}));
    CHECK(sort_from_key(sort_key(SORT_NEWEST), SORT_NAME) == SORT_NEWEST);
    CHECK(sort_from_key(sort_key(SORT_SIZE), SORT_NAME) == SORT_SIZE);
    CHECK(sort_from_key("", SORT_NEWEST) == SORT_NEWEST);
    CHECK(sort_from_key("bogus", SORT_NAME) == SORT_NAME);
}

void test_sidecars() {
    Sidecar s;
    CHECK(match_sidecar("Movie.mp4", "Movie.srt", s) && s.label == "Subtitles" && s.language.empty());
    CHECK(match_sidecar("Movie.mp4", "movie.SRT", s));  // the card doesn't tell case apart
    CHECK(match_sidecar("Movie.mp4", "Movie.en.srt", s) && s.label == "English" && s.language == "en");
    CHECK(match_sidecar("Movie.mp4", "Movie.eng.ass", s) && s.label == "English");
    CHECK(match_sidecar("Movie.mp4", "Movie.Spanish.ssa", s) && s.label == "Spanish" && s.language == "es");
    CHECK(match_sidecar("Movie.mp4", "Movie.pt-BR.vtt", s) && s.label == "Portuguese" && s.language == "pt");
    CHECK(match_sidecar("Movie.mp4", "Movie.fre.srt", s) && s.language == "fr");
    CHECK(match_sidecar("Movie.mp4", "Movie.en.sdh.srt", s) && s.label == "English (SDH)");
    CHECK(match_sidecar("Movie.mp4", "Movie.sdh.srt", s) && s.label == "Subtitles (SDH)");
    CHECK(!match_sidecar("Movie.mp4", "Movie.forced.srt", s));
    CHECK(!match_sidecar("Movie.mp4", "Movie.en.forced.srt", s));
    CHECK(!match_sidecar("Movie.mp4", "Movie.2.srt", s));            // not a language
    CHECK(!match_sidecar("Movie.mp4", "Movie Extras.srt", s));       // another video's
    CHECK(!match_sidecar("Movie.mp4", "Movie2.srt", s));
    CHECK(!match_sidecar("Movie.mp4", "Movie.en.txt", s));           // not subtitles
    CHECK(!match_sidecar("Movie.mp4", "Movie.en.sdh.extra.srt", s));
    CHECK(match_sidecar("Show.S01E01.mkv", "Show.S01E01.de.srt", s) && s.label == "German");
    CHECK(!match_sidecar("Show.S01E01.mkv", "Show.S01E02.srt", s));
    CHECK(language_name("ENG") && std::string(language_name("ENG")) == "English");
    CHECK(language_name("zh_Hans") && std::string(language_name("zh_Hans")) == "Chinese");
    CHECK(language_name("xx") == nullptr && language_name("") == nullptr);

    std::vector<std::string> names = {"Movie.fr.srt", "Movie.srt",       "Movie.es.srt", "Movie.es.sdh.srt",
                                      "Movie.ass",    "Movie.forced.srt", "Other.srt",   "Movie.de.srt"};
    std::vector<Sidecar> subs = find_sidecars("/m", "Movie.mkv", names, "es");
    std::vector<std::string> labels;
    for (const Sidecar& x : subs) labels.push_back(x.label);
    // Spanish app: Spanish first (plain, then SDH), then the untagged ones, then the rest by name.
    CHECK((labels == std::vector<std::string>{"Spanish", "Spanish (SDH)", "Subtitles \xC2\xB7 ASS", "Subtitles \xC2\xB7 SRT",
                                              "French", "German"}));
    CHECK(subs.size() == 6 && subs[0].path == "/m/Movie.es.srt");
    subs = find_sidecars("/m", "Movie.mkv", names, "en");
    CHECK(!subs.empty() && subs[0].language.empty());  // nothing in English: the untagged one
    CHECK(find_sidecars("/m", "Nothing.mkv", names, "en").empty());
}

void test_names() {
    CHECK(name_problem("Holiday 2024").empty());
    CHECK(name_problem("Caf\xC3\xA9 \xE2\x80\x94 d\xC3\xAD" "a 1").empty());  // UTF-8 is fine
    CHECK(!name_problem("").empty() && !name_problem("   ").empty());
    CHECK(!name_problem("a/b").empty() && !name_problem("a\\b").empty() && !name_problem("a:b").empty());
    CHECK(!name_problem("what?").empty() && !name_problem("a*b").empty() && !name_problem("\"q\"").empty());
    CHECK(!name_problem("<x>").empty() && !name_problem("a|b").empty() && !name_problem("tab\there").empty());
    CHECK(!name_problem(".hidden").empty() && !name_problem("..").empty() && !name_problem("end.").empty());
    CHECK(!name_problem(std::string(201, 'a')).empty());

    CHECK(renamed("IMG_0042.MP4", "Holiday", false) == "Holiday.MP4");
    CHECK(renamed("IMG_0042.MP4", "  Holiday  ", false) == "Holiday.MP4");
    CHECK(renamed("IMG_0042.MP4", "Holiday.mp4", false) == "Holiday.MP4");  // typed with it
    CHECK(renamed("IMG_0042.MP4", "Holiday.mkv", false) == "Holiday.mkv.MP4");  // the kind stays
    CHECK(renamed("clip.mp4", "", false).empty());
    CHECK(renamed("README", "Notes", false) == "Notes");
    CHECK(renamed("Old folder.v2", "New.folder", true) == "New.folder");

    CHECK(inside("/sd/app/Videos/a.mp4", "/sd/app"));
    CHECK(inside("/sd/app/Videos", "/sd/app/"));
    CHECK(!inside("/sd/app", "/sd/app"));
    CHECK(!inside("/sd/app/", "/sd/app"));
    CHECK(!inside("/sd/apple/x.mp4", "/sd/app"));
    CHECK(!inside("/sd/app/../other/x.mp4", "/sd/app"));
    CHECK(!inside("/sd/app/Videos/./x.mp4", "/sd/app"));
    CHECK(!inside("/other/x.mp4", "/sd/app"));
}

void test_file_ops(const std::string& base) {
    const std::string root = base + "/media", videos = root + "/Videos", outside = base + "/outside";
    util::make_dirs(videos);
    util::make_dirs(outside);

    // Rename: the video and its subtitle files, keeping the extension.
    for (const char* n : {"IMG_1.MP4", "IMG_1.srt", "IMG_1.en.srt", "img_1.fr.ass", "IMG_1.forced.srt", "IMG_10.srt"})
        touch(videos + "/" + n);
    std::vector<std::string> names = {"IMG_1.srt", "IMG_1.en.srt", "img_1.fr.ass", "IMG_1.forced.srt", "IMG_10.srt"};
    Entry v = file_entry(videos, "IMG_1.MP4", names);
    CHECK(v.subs.size() == 3);
    FileOp r = rename_entry(root, v, "Beach day");
    CHECK(r.ok && r.path == videos + "/Beach day.MP4");
    CHECK(exists(videos + "/Beach day.MP4") && !exists(videos + "/IMG_1.MP4"));
    CHECK(exists(videos + "/Beach day.srt") && exists(videos + "/Beach day.en.srt") && exists(videos + "/Beach day.fr.ass"));
    CHECK(exists(videos + "/IMG_1.forced.srt") && exists(videos + "/IMG_10.srt"));  // not its own: left alone

    // Refused: bad names, a name that's taken (by the video or one of its subtitle files).
    touch(videos + "/Taken.MP4");
    touch(videos + "/Sub taken.en.srt");
    names = {"Beach day.srt", "Beach day.en.srt", "Beach day.fr.ass", "Sub taken.en.srt"};
    v = file_entry(videos, "Beach day.MP4", names);
    CHECK(!rename_entry(root, v, "Taken").ok);
    CHECK(!rename_entry(root, v, "Sub taken").ok);
    CHECK(exists(videos + "/Beach day.en.srt"));  // nothing half done
    CHECK(!rename_entry(root, v, "a/b").ok && !rename_entry(root, v, ".hidden").ok && !rename_entry(root, v, "  ").ok);
    CHECK(exists(v.path));
    // Only the case: allowed even though the card sees it as the same name.
    r = rename_entry(root, v, "BEACH DAY");
    CHECK(r.ok && r.path == videos + "/BEACH DAY.MP4" && exists(r.path));
    names = {"BEACH DAY.srt", "BEACH DAY.en.srt", "BEACH DAY.fr.ass"};
    {
        // On a case-blind file system the old spelling may still "exist"; list the folder instead.
        bool found = false;
        if (DIR* d = opendir(videos.c_str())) {
            while (dirent* de = readdir(d)) found |= std::string(de->d_name) == "BEACH DAY.en.srt";
            closedir(d);
        }
        CHECK(found);
    }
    // The same name: nothing to do.
    v = file_entry(videos, "BEACH DAY.MP4", names);
    r = rename_entry(root, v, "BEACH DAY.MP4");
    CHECK(r.ok && r.path == v.path);

    // Folders: renamed by name only.
    util::make_dirs(videos + "/Trip");
    touch(videos + "/Trip/a.mp4");
    r = rename_entry(root, dir_entry(videos, "Trip"), "Trip 2024.old");
    CHECK(r.ok && exists(videos + "/Trip 2024.old/a.mp4"));
    CHECK(!rename_entry(root, dir_entry(videos, "Trip 2024.old"), "Taken.MP4").ok);
    // A folder isn't renamed as a file, nor a file as a folder.
    CHECK(!rename_entry(root, file_entry(videos, "Trip 2024.old"), "x").ok);

    // Outside the root, and links: refused.
    touch(outside + "/secret.mp4");
    CHECK(!rename_entry(root, file_entry(outside, "secret.mp4"), "gone").ok);
    CHECK(!delete_file(root, file_entry(outside, "secret.mp4")).ok);
    Entry sneaky = file_entry(videos, "../../outside/secret.mp4");
    CHECK(!delete_file(root, sneaky).ok && exists(outside + "/secret.mp4"));
    CHECK(symlink((outside + "/secret.mp4").c_str(), (videos + "/link.mp4").c_str()) == 0);
    CHECK(!rename_entry(root, file_entry(videos, "link.mp4"), "renamed link").ok);
    CHECK(!delete_file(root, file_entry(videos, "link.mp4")).ok);
    CHECK(exists(videos + "/link.mp4") && exists(outside + "/secret.mp4"));
    CHECK(symlink(outside.c_str(), (videos + "/linked folder").c_str()) == 0);
    CHECK(!delete_folder(root, videos + "/linked folder").ok && exists(outside));
    CHECK(!rename_entry(root, dir_entry(videos, "linked folder"), "x").ok);

    // Delete: the video and its subtitle files, nothing else.
    v = file_entry(videos, "BEACH DAY.MP4", names);
    r = delete_file(root, v);
    CHECK(r.ok && !exists(videos + "/BEACH DAY.MP4"));
    CHECK(!exists(videos + "/BEACH DAY.srt") && !exists(videos + "/BEACH DAY.en.srt") && !exists(videos + "/BEACH DAY.fr.ass"));
    CHECK(exists(videos + "/IMG_1.forced.srt") && exists(videos + "/Taken.MP4"));
    CHECK(!delete_file(root, v).ok);  // already gone
    CHECK(!delete_file(root, dir_entry(videos, "Trip 2024.old")).ok);  // a folder isn't a file

    // Folders are deleted only when empty (the junk computers leave aside).
    r = delete_folder(root, videos + "/Trip 2024.old");
    CHECK(!r.ok && !r.error.empty() && exists(videos + "/Trip 2024.old/a.mp4"));
    util::make_dirs(videos + "/Empty");
    touch(videos + "/Empty/.DS_Store");
    touch(videos + "/Empty/._a.mp4");
    touch(videos + "/Empty/Thumbs.db");
    CHECK(delete_folder(root, videos + "/Empty").ok && !exists(videos + "/Empty"));
    util::make_dirs(videos + "/Nested/inner");
    CHECK(!delete_folder(root, videos + "/Nested").ok && exists(videos + "/Nested/inner"));
    CHECK(!delete_folder(root, root).ok && exists(root));
    CHECK(!delete_folder(root, outside).ok);

    // New folders.
    r = make_folder(root, videos, "  Summer  ");
    CHECK(r.ok && r.path == videos + "/Summer" && exists(videos + "/Summer"));
    CHECK(!make_folder(root, videos, "Summer").ok);
    CHECK(!make_folder(root, videos, "Taken.MP4").ok);
    CHECK(!make_folder(root, videos, "bad:name").ok && !make_folder(root, videos, "..").ok);
    CHECK(!make_folder(root, outside, "Escape").ok && !exists(outside + "/Escape"));
    CHECK(!make_folder(root, videos + "/missing", "x").ok);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/media-files-test.XXXXXX";
    const char* base = mkdtemp(tmpl);
    if (!base) return 2;
    test_sorting();
    test_sidecars();
    test_names();
    test_file_ops(base);
    std::string cmd = std::string("rm -rf '") + base + "'";
    if (g_failed == 0) std::system(cmd.c_str());
    else std::cerr << "left for a look: " << base << "\n";
    std::cout << g_checks - g_failed << "/" << g_checks << " checks passed\n";
    return g_failed ? 1 : 0;
}
