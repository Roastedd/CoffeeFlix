#include "services/locale_bias.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace locale_bias {

namespace {

constexpr const char* KEY = "yt_locale";  // marker -> how much of it was enjoyed

struct Script {
    const char* name;
    uint32_t from, to;
    const char* countries;  // the places where it is the home script, as "|IN|PK|"
};
// UTF-8 code point ranges. Han is shared by Chinese and Japanese; Arabic by Arabic, Urdu and Persian.
const Script SCRIPTS[] = {
    {"devanagari", 0x0900, 0x097F, "|IN|NP|"}, {"bengali", 0x0980, 0x09FF, "|IN|BD|"}, {"gurmukhi", 0x0A00, 0x0A7F, "|IN|"},
    {"gujarati", 0x0A80, 0x0AFF, "|IN|"},      {"tamil", 0x0B80, 0x0BFF, "|IN|LK|"},   {"telugu", 0x0C00, 0x0C7F, "|IN|"},
    {"kannada", 0x0C80, 0x0CFF, "|IN|"},       {"malayalam", 0x0D00, 0x0D7F, "|IN|"},  {"sinhala", 0x0D80, 0x0DFF, "|LK|"},
    {"thai", 0x0E00, 0x0E7F, "|TH|"},          {"arabic", 0x0600, 0x06FF, "|SA|AE|EG|PK|IQ|MA|DZ|JO|KW|QA|"},
    {"cyrillic", 0x0400, 0x04FF, "|RU|UA|BY|KZ|BG|RS|"},
    {"hangul", 0xAC00, 0xD7AF, "|KR|"},        {"kana", 0x3040, 0x30FF, "|JP|"},       {"han", 0x4E00, 0x9FFF, "|CN|TW|HK|JP|SG|"},
};

struct Word {
    const char* word;
    const char* countries;
};
// Latin-script titles of the same kind of video. Whole words, lower case.
const Word WORDS[] = {
    {"hindi", "|IN|"},      {"bollywood", "|IN|"},  {"tamil", "|IN|LK|"}, {"telugu", "|IN|"},     {"kannada", "|IN|"},
    {"malayalam", "|IN|"},  {"bhojpuri", "|IN|"},   {"haryanvi", "|IN|"}, {"marathi", "|IN|"},    {"gujarati", "|IN|"},
    {"punjabi", "|IN|PK|"}, {"bengali", "|IN|BD|"}, {"desi", "|IN|PK|BD|"}, {"urdu", "|PK|IN|"},   {"pakistani", "|PK|"},
    {"ipl", "|IN|"},        {"kollywood", "|IN|"},  {"tollywood", "|IN|"}, {"mollywood", "|IN|"},  {"sandalwood", "|IN|"},
    {"dhamaka", "|IN|"},    {"arabic", "|SA|AE|EG|"}, {"nepali", "|NP|"},   {"india", "|IN|"},      {"indian", "|IN|"},
    {"bharat", "|IN|"},     {"hindustani", "|IN|"}, {"pakistan", "|PK|"},
};
// Hindi and Urdu written in Latin letters ("Hinglish"), which most Indian uploads use in titles. One of the first group
// marks a title, two of the second do.
const char* const HINGLISH_STRONG[] = {"hai", "nahi", "nahin", "kya", "kaise", "bhai", "bhaiya", "yaar", "wala", "wali", "waala",
                                       "khatarnak", "maaro", "chappal", "bahut", "sabse", "zindagi", "ishq", "kyun", "mazedaar"};
const char* const HINGLISH_WEAK[] = {"aur", "aaj", "ghar", "pyar", "dekho", "kuch", "tera", "teri", "mera", "meri", "humne", "kiya",
                                     "raha", "rahe", "mein", "lagao", "paisa", "dost", "dosti", "jaan", "dil", "saas", "bahu", "beta",
                                     "beti", "bhabhi", "sach", "jhooth", "chalo", "tum", "hum", "tumhare"};
// Channel names (parts of, lower case) of the big regional networks and labels.
const Word CHANNELS[] = {
    {"t-series", "|IN|"},   {"tseries", "|IN|"},     {"zee music", "|IN|"},  {"zee tv", "|IN|"},     {"zee5", "|IN|"},
    {"sony music india", "|IN|"}, {"tips official", "|IN|"}, {"tips music", "|IN|"}, {"saregama", "|IN|"}, {"aaj tak", "|IN|"},
    {"abp news", "|IN|"},   {"ndtv", "|IN|"},        {"india tv", "|IN|"},   {"news18", "|IN|"},     {"republic world", "|IN|"},
    {"republic bharat", "|IN|"}, {"lahari", "|IN|"}, {"wave music", "|IN|"}, {"speed records", "|IN|"}, {"goldmines", "|IN|"},
    {"shemaroo", "|IN|"},   {"think music", "|IN|"}, {"sun tv", "|IN|"},    {"star plus", "|IN|"},  {"colors tv", "|IN|"},
    {"hungama", "|IN|"},    {"pen movies", "|IN|"},  {"aditya music", "|IN|"}, {"mythri", "|IN|"},
    {"india", "|IN|"},      {"bharat", "|IN|"},      {"maddock", "|IN|"},    {"dharma", "|IN|"},     {"yash raj", "|IN|"},
    {"excel entertainment", "|IN|"}, {"eros now", "|IN|"}, {"jio", "|IN|"}, {"hotstar", "|IN|"},   {"sony liv", "|IN|"},
    {"voot", "|IN|"},       {"mx player", "|IN|"},
};

std::mutex g_m;
std::map<std::string, double> g_enjoyed;
bool g_loaded = false;

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    auto d = json::Doc::parse(store::get_str(KEY, "{}"));
    const char* k;
    json_t* v;
    json_object_foreach(d.get(), k, v) g_enjoyed[k] = json_number_value(v);
}

void save_locked() {
    json::Doc d(json_object());
    for (const auto& [k, v] : g_enjoyed) json_object_set_new(d.get(), k.c_str(), json_real(v));
    store::set_str(KEY, json::dump(d.get()));
}

// The viewer's own countries: Settings > Country, and where the app's language is spoken.
std::set<std::string> home() {
    std::set<std::string> out;
    std::string region = store::get_str("yt_region", "US");
    for (char& c : region) c = (char)std::toupper((unsigned char)c);
    out.insert(region);
    const std::string lang = i18n::current().code;
    if (lang == "ko") out.insert("KR");
    else if (lang == "ja") out.insert("JP");
    else if (lang == "zh") { out.insert("CN"); out.insert("TW"); out.insert("HK"); }
    return out;
}

bool at_home(const char* countries, const std::set<std::string>& mine) {
    for (const auto& c : mine)
        if (std::string(countries).find("|" + c + "|") != std::string::npos) return true;
    return false;
}

// Code points of a UTF-8 string.
std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) { cp = (c & 7u) << 18 | (s[i + 1] & 63u) << 12 | (s[i + 2] & 63u) << 6 | (s[i + 3] & 63u); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = (c & 15u) << 12 | (s[i + 1] & 63u) << 6 | (s[i + 2] & 63u); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = (c & 31u) << 6 | (s[i + 1] & 63u); n = 2; }
        out.push_back(cp);
        i += n;
    }
    return out;
}

struct Mark {
    std::string tag;
    const char* countries;
};

std::vector<Mark> find_marks(const std::string& title, const std::string& channel) {
    std::vector<Mark> marks;
    auto add = [&](const std::string& tag, const char* countries) {
        for (const Mark& m : marks)
            if (m.tag == tag) return;
        marks.push_back({tag, countries});
    };
    // Scripts: it takes a few letters (an emoji or one foreign word in an English title shouldn't mark it).
    std::map<const Script*, int> counts;
    for (uint32_t cp : decode(title + " " + channel))
        for (const Script& s : SCRIPTS)
            if (cp >= s.from && cp <= s.to) { counts[&s]++; break; }
    for (const auto& [s, n] : counts)
        if (n >= 3) add(std::string("script:") + s->name, s->countries);
    // Words of the title, and the channel's name.
    std::string word;
    int strong = 0, weak = 0;
    auto flush = [&] {
        if (word.size() >= 3)
            for (const Word& w : WORDS)
                if (word == w.word) add(std::string("word:") + w.word, w.countries);
        for (const char* h : HINGLISH_STRONG) strong += word == h;
        for (const char* h : HINGLISH_WEAK) weak += word == h;
        word.clear();
    };
    for (unsigned char c : title) {
        if (std::isalnum(c)) word += (char)std::tolower(c);
        else flush();
    }
    flush();
    if (strong >= 1 || weak >= 2) add("word:hinglish", "|IN|PK|BD|NP|LK|");
    const std::string ch = util::lower(channel);
    for (const Word& w : CHANNELS)
        if (ch.find(w.word) != std::string::npos) add(std::string("channel:") + w.word, w.countries);
    return marks;
}

}  // namespace

bool enabled() { return store::get_bool("yt_local_bias", true); }

std::vector<std::string> markers(const std::string& title, const std::string& channel) {
    std::vector<std::string> out;
    for (const Mark& m : find_marks(title, channel)) out.push_back(m.tag);
    return out;
}

double penalty(const std::string& title, const std::string& channel) {
    if (!enabled()) return 0;
    const auto marks = find_marks(title, channel);
    if (marks.empty()) return 0;
    const auto mine = home();
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    double worst = 0, extra = 0;
    for (const Mark& m : marks) {
        if (at_home(m.countries, mine)) continue;
        // What was enjoyed takes the push away: a video of the same kind watched, or two, and it is gone.
        auto it = g_enjoyed.find(m.tag);
        const double wanted = it == g_enjoyed.end() ? 0 : std::min(1.0, it->second / 1.0);
        const double p = 0.5 * (1 - wanted);
        if (p > worst) { extra += worst > 0 ? 0.1 : 0; worst = p; }
        else if (p > 0) extra += 0.1;
    }
    return std::min(0.9, worst + std::min(extra, 0.3));
}

void note_enjoyed(const std::string& title, const std::string& channel) {
    const auto marks = find_marks(title, channel);
    std::lock_guard<std::mutex> lk(g_m);
    load_locked();
    for (auto& kv : g_enjoyed) kv.second *= 0.98;
    for (const Mark& m : marks) g_enjoyed[m.tag] = std::min(3.0, g_enjoyed[m.tag] + 0.5);
    for (auto it = g_enjoyed.begin(); it != g_enjoyed.end();) it = it->second < 0.05 ? g_enjoyed.erase(it) : std::next(it);
    if (!marks.empty()) save_locked();
}

void reset() {
    std::lock_guard<std::mutex> lk(g_m);
    g_enjoyed.clear();
    g_loaded = true;
    store::set_str(KEY, "{}");
}

void filter(std::vector<youtube::Video>& items, size_t keep) {
    if (!enabled()) return;
    std::vector<youtube::Video> good;
    std::vector<std::pair<double, youtube::Video>> marked;
    for (auto& v : items) {
        const double p = penalty(v);
        if (p < 0.3) good.push_back(std::move(v));
        else marked.push_back({p, std::move(v)});
    }
    if (good.size() < keep) {  // too few others: the least marked ones fill up
        std::stable_sort(marked.begin(), marked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < marked.size() && good.size() < keep; i++) good.push_back(std::move(marked[i].second));
    }
    items = std::move(good);
}

}  // namespace locale_bias
