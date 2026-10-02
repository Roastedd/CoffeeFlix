#include "screens/media_files.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/i18n.hpp"
#include "core/util.hpp"

namespace screens::media {

namespace {

// Languages subtitle files are tagged with: ISO 639-1, 639-2 (both kinds) and names.
struct Language {
    const char* code;       // 639-1
    const char* names[5];   // other ways to write it, lower case
    const char* english;    // its name (a tr() string)
};
const Language LANGUAGES[] = {
    {"en", {"eng", "english"}, N_("English")},
    {"es", {"spa", "spanish", "espanol", "espa\xC3\xB1ol", "castellano"}, N_("Spanish")},
    {"fr", {"fra", "fre", "french", "francais", "fran\xC3\xA7" "ais"}, N_("French")},
    {"de", {"deu", "ger", "german", "deutsch"}, N_("German")},
    {"it", {"ita", "italian", "italiano"}, N_("Italian")},
    {"pt", {"por", "portuguese", "portugues", "portugu\xC3\xAAs"}, N_("Portuguese")},
    {"nl", {"nld", "dut", "dutch", "nederlands"}, N_("Dutch")},
    {"ja", {"jpn", "japanese", "jp"}, N_("Japanese")},
    {"zh", {"zho", "chi", "chinese", "chs", "cht"}, N_("Chinese")},
    {"ko", {"kor", "korean"}, N_("Korean")},
    {"tl", {"tgl", "tagalog", "fil", "filipino"}, N_("Tagalog")},
    {"ar", {"ara", "arabic"}, N_("Arabic")},
    {"ru", {"rus", "russian"}, N_("Russian")},
    {"tr", {"tur", "turkish"}, N_("Turkish")},
    {"hi", {"hin", "hindi"}, N_("Hindi")},
    {"th", {"tha", "thai"}, N_("Thai")},
    {"vi", {"vie", "vietnamese"}, N_("Vietnamese")},
    {"id", {"ind", "indonesian"}, N_("Indonesian")},
    {"ms", {"msa", "may", "malay"}, N_("Malay")},
    {"pl", {"pol", "polish"}, N_("Polish")},
    {"sv", {"swe", "swedish"}, N_("Swedish")},
    {"no", {"nor", "nob", "nb", "norwegian"}, N_("Norwegian")},
    {"da", {"dan", "danish"}, N_("Danish")},
    {"fi", {"fin", "finnish"}, N_("Finnish")},
    {"cs", {"ces", "cze", "czech"}, N_("Czech")},
    {"el", {"ell", "gre", "greek"}, N_("Greek")},
    {"he", {"heb", "hebrew", "iw"}, N_("Hebrew")},
    {"hu", {"hun", "hungarian"}, N_("Hungarian")},
    {"ro", {"ron", "rum", "romanian"}, N_("Romanian")},
    {"uk", {"ukr", "ukrainian"}, N_("Ukrainian")},
};

const Language* find_language(const std::string& tag) {
    std::string t = util::lower(tag);
    t = t.substr(0, t.find_first_of("-_"));  // "pt-br", "en_us", "zh-hans"
    if (t.empty()) return nullptr;
    for (const Language& l : LANGUAGES) {
        if (t == l.code) return &l;
        for (const char* n : l.names)
            if (n && t == n) return &l;
    }
    return nullptr;
}

}  // namespace

int link_stat(const std::string& path, struct stat* st) {
#ifdef __WIIU__
    return stat(path.c_str(), st);  // the SD card's FAT has no links
#else
    return lstat(path.c_str(), st);
#endif
}

namespace {

// Left by computers, never shown: a folder with only these counts as empty.
bool junk_file(const std::string& name) {
    return name == ".DS_Store" || util::starts_with(name, "._") || util::lower(name) == "thumbs.db" ||
           util::lower(name) == "desktop.ini";
}

bool lexists(const std::string& path) {
    struct stat st;
    return link_stat(path, &st) == 0;
}

// rename(), also when only the case changes (the memory card's file system can't tell those
// apart, so it goes by way of another name).
bool move(const std::string& from, const std::string& to) {
    if (util::lower(from) != util::lower(to)) return rename(from.c_str(), to.c_str()) == 0;
    std::string tmp = from + ".renaming";
    if (lexists(tmp) || rename(from.c_str(), tmp.c_str()) != 0) return false;
    if (rename(tmp.c_str(), to.c_str()) == 0) return true;
    rename(tmp.c_str(), from.c_str());
    return false;
}

FileOp fail(const char* error) {
    FileOp r;
    r.error = error;
    return r;
}

}  // namespace

Kind kind_of(const std::string& name) {
    std::string e = util::file_extension(name);
    static const char* video[] = {"mp4", "m4v", "mkv", "webm", "avi", "mov", "ts", "m2ts", "mpg", "mpeg", "flv", "3gp"};
    static const char* audio[] = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wv", "alac", "oga", "mka"};
    static const char* image[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
    static const char* book[] = {"cbz", "epub"};
    for (auto v : video) if (e == v) return K_VIDEO;
    for (auto v : audio) if (e == v) return K_AUDIO;
    for (auto v : image) if (e == v) return K_IMAGE;
    for (auto v : book) if (e == v) return K_BOOK;
    return K_OTHER;
}

std::string strip_ext(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool is_subtitle_file(const std::string& name) {
    std::string e = util::file_extension(name);
    return e == "srt" || e == "vtt" || e == "ass" || e == "ssa";
}

// --- sorting ---------------------------------------------------------------------------------------

void sort_entries(std::vector<Entry>& entries, SortMode mode) {
    std::sort(entries.begin(), entries.end(), [mode](const Entry& a, const Entry& b) {
        bool da = a.kind == K_DIR, db = b.kind == K_DIR;
        if (da != db) return da;
        if (mode == SORT_NEWEST && a.mtime != b.mtime) return a.mtime > b.mtime;
        if (mode == SORT_SIZE && !da && a.size != b.size) return a.size > b.size;
        if (util::natural_less(a.name, b.name)) return true;
        if (util::natural_less(b.name, a.name)) return false;
        return a.name < b.name;
    });
}

const char* sort_key(SortMode mode) {
    switch (mode) {
        case SORT_NEWEST: return "newest";
        case SORT_SIZE: return "size";
        default: return "name";
    }
}

SortMode sort_from_key(const std::string& key, SortMode def) {
    if (key == "name") return SORT_NAME;
    if (key == "newest") return SORT_NEWEST;
    if (key == "size") return SORT_SIZE;
    return def;
}

// --- subtitle files --------------------------------------------------------------------------------

const char* language_name(const std::string& tag) {
    const Language* l = find_language(tag);
    return l ? l->english : nullptr;
}

bool match_sidecar(const std::string& video, const std::string& sub, Sidecar& out) {
    if (!is_subtitle_file(sub)) return false;
    // Memory cards don't tell "Movie.srt" from "movie.srt" either.
    std::string base = util::lower(strip_ext(video)), sbase = util::lower(strip_ext(sub));
    const Language* lang = nullptr;
    bool sdh = false;
    if (sbase != base) {
        if (sbase.size() <= base.size() + 1 || sbase.compare(0, base.size(), base) != 0 || sbase[base.size()] != '.')
            return false;
        // "<video>.<language>" with maybe ".sdh" (hearing impaired) after it.
        std::vector<std::string> tags = util::split(sbase.substr(base.size() + 1), '.');
        if (tags.empty() || tags.size() > 2) return false;
        for (const std::string& t : tags)
            if (t == "forced") return false;  // only the signs and foreign lines
        auto is_sdh = [](const std::string& t) { return t == "sdh" || t == "cc" || t == "hi"; };
        lang = find_language(tags[0]);
        if (tags.size() == 2) {
            if (!lang || !is_sdh(tags[1])) return false;
            sdh = true;
        } else if (!lang) {
            if (!is_sdh(tags[0])) return false;
            sdh = true;
        }
    }
    out = Sidecar();
    out.name = sub;
    out.language = lang ? lang->code : "";
    out.label = lang ? tr(lang->english) : tr("Subtitles");
    if (sdh) out.label += " (SDH)";
    return true;
}

void order_sidecars(std::vector<Sidecar>& subs, const std::string& ui_language) {
    std::string ui = util::lower(ui_language).substr(0, 2);
    auto rank = [&ui](const Sidecar& s) {
        if (!s.language.empty() && s.language == ui) return s.label.find("(SDH)") == std::string::npos ? 0 : 1;
        return s.language.empty() ? 2 : 3;
    };
    std::stable_sort(subs.begin(), subs.end(), [&](const Sidecar& a, const Sidecar& b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        if (a.label != b.label) return util::natural_less(a.label, b.label);
        return util::natural_less(a.name, b.name);
    });
    // Two with the same label ("Movie.srt" and "Movie.ass"): the kind tells them apart.
    std::vector<bool> twin(subs.size(), false);
    for (size_t i = 0; i < subs.size(); i++)
        for (size_t j = 0; j < subs.size(); j++)
            if (i != j && subs[j].label == subs[i].label) twin[i] = true;
    for (size_t i = 0; i < subs.size(); i++) {
        if (!twin[i]) continue;
        std::string ext = util::file_extension(subs[i].name);
        for (char& c : ext) c = (char)toupper((unsigned char)c);
        subs[i].label += " \xC2\xB7 " + ext;
    }
}

std::vector<Sidecar> find_sidecars(const std::string& dir, const std::string& video, const std::vector<std::string>& names,
                                   const std::string& ui_language) {
    std::vector<Sidecar> out;
    for (const std::string& n : names) {
        Sidecar s;
        if (!match_sidecar(video, n, s)) continue;
        s.path = util::join_path(dir, n);
        out.push_back(std::move(s));
    }
    order_sidecars(out, ui_language);
    return out;
}

// --- names -------------------------------------------------------------------------------------

std::string name_problem(const std::string& name) {
    if (util::trim(name).empty()) return tr("Enter a name");
    if (name.size() > 200) return tr("That name is too long");
    if (name[0] == '.' || name.back() == '.') return tr("Names can't start or end with a dot");
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c)) return tr("Names can't have / \\ : * ? \" < > or |");
    return "";
}

std::string renamed(const std::string& old_name, const std::string& typed, bool is_dir) {
    std::string name = util::trim(typed);
    if (is_dir) return name;
    size_t dot = old_name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return name;
    std::string ext = old_name.substr(dot);  // ".MP4", as it was
    if (name.size() > ext.size() && util::lower(name.substr(name.size() - ext.size())) == util::lower(ext))
        name.resize(name.size() - ext.size());  // typed with the extension
    return name.empty() ? name : name + ext;
}

bool inside(const std::string& path, const std::string& root) {
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    if (r.empty() || path.size() <= r.size() + 1 || path.compare(0, r.size(), r) != 0 || path[r.size()] != '/')
        return false;
    for (const std::string& part : util::split(path.substr(r.size() + 1), '/'))
        if (part == ".." || part == ".") return false;
    return true;
}

// --- changing files ---------------------------------------------------------------------------

FileOp rename_entry(const std::string& root, const Entry& e, const std::string& typed) {
    bool is_dir = e.kind == K_DIR;
    std::string problem = name_problem(util::trim(typed));
    std::string name = renamed(e.name, typed, is_dir);
    if (problem.empty()) problem = name_problem(name);
    if (!problem.empty()) {
        FileOp r;
        r.error = problem;
        return r;
    }
    struct stat st;
    if (!inside(e.path, root) || link_stat(e.path, &st) != 0 || S_ISLNK(st.st_mode) ||
        (is_dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))
        return fail(tr("This can't be renamed here"));
    FileOp r;
    r.ok = true;
    r.path = e.path;
    if (name == e.name) return r;

    std::string dir = util::parent_dir(e.path);
    std::string target = util::join_path(dir, name);
    const char* exists = tr("There's already something with that name here");
    if (util::lower(name) != util::lower(e.name) && lexists(target)) return fail(exists);
    // The subtitle files: "<old name>.en.srt" -> "<new name>.en.srt".
    std::string old_base = strip_ext(e.name), new_base = strip_ext(name);
    std::vector<std::pair<std::string, std::string>> subs;
    if (!is_dir) {
        for (const Sidecar& s : e.subs) {
            if (s.name.size() < old_base.size() || !inside(s.path, root)) continue;
            std::string to = util::join_path(dir, new_base + s.name.substr(old_base.size()));
            if (util::lower(to) != util::lower(s.path) && lexists(to)) return fail(exists);
            subs.emplace_back(s.path, to);
        }
    }
    if (!move(e.path, target)) return fail(tr("Couldn't rename it"));
    for (const auto& [from, to] : subs) {
        struct stat ss;
        if (link_stat(from, &ss) == 0 && S_ISREG(ss.st_mode)) move(from, to);
    }
    r.path = target;
    return r;
}

FileOp delete_file(const std::string& root, const Entry& e) {
    struct stat st;
    if (!inside(e.path, root) || link_stat(e.path, &st) != 0 || !S_ISREG(st.st_mode))
        return fail(tr("This can't be deleted here"));
    if (remove(e.path.c_str()) != 0) return fail(tr("Couldn't delete it"));
    for (const Sidecar& s : e.subs) {
        struct stat ss;
        if (inside(s.path, root) && util::parent_dir(s.path) == util::parent_dir(e.path) && link_stat(s.path, &ss) == 0 &&
            S_ISREG(ss.st_mode))
            remove(s.path.c_str());
    }
    FileOp r;
    r.ok = true;
    return r;
}

FileOp delete_folder(const std::string& root, const std::string& dir) {
    struct stat st;
    if (!inside(dir, root) || link_stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return fail(tr("This can't be deleted here"));
    DIR* d = opendir(dir.c_str());
    if (!d) return fail(tr("Couldn't delete it"));
    std::vector<std::string> junk;
    bool empty = true;
    while (dirent* de = readdir(d)) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        struct stat js;
        std::string p = util::join_path(dir, name);
        if (junk_file(name) && link_stat(p, &js) == 0 && S_ISREG(js.st_mode)) {
            junk.push_back(p);
        } else {
            empty = false;
            break;
        }
    }
    closedir(d);
    if (!empty) return fail(tr("Only empty folders can be deleted"));
    for (const std::string& p : junk) remove(p.c_str());
    if (rmdir(dir.c_str()) != 0) return fail(tr("Couldn't delete it"));
    FileOp r;
    r.ok = true;
    return r;
}

FileOp make_folder(const std::string& root, const std::string& parent, const std::string& typed) {
    std::string name = util::trim(typed);
    std::string problem = name_problem(name);
    if (!problem.empty()) {
        FileOp r;
        r.error = problem;
        return r;
    }
    std::string target = util::join_path(parent, name);
    struct stat st;
    if (!inside(target, root) || link_stat(parent, &st) != 0 || !S_ISDIR(st.st_mode))
        return fail(tr("Folders can't be made here"));
    if (lexists(target)) return fail(tr("There's already something with that name here"));
    if (mkdir(target.c_str(), 0777) != 0) return fail(tr("Couldn't make the folder"));
    FileOp r;
    r.ok = true;
    r.path = target;
    return r;
}

}  // namespace screens::media
