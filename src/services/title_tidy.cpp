#include "services/title_tidy.hpp"

#include <cctype>
#include <set>
#include <vector>

#include "core/store.hpp"

namespace title {

namespace {

// Short names that stay in capitals when the rest is brought down.
const std::set<std::string>& acronyms() {
    static const std::set<std::string> a = {"TV", "UK", "US", "USA", "EU", "UN", "NASA", "FBI", "CIA", "AI", "DIY", "ASMR", "UFC", "NBA",
                                            "NFL", "MLB", "NHL", "FIFA", "BBC", "CNN", "PBS", "HD", "VR", "PC", "GTA", "DC", "WWE", "ABC",
                                            "NBC", "CBS", "MTV", "NYC", "LA", "DJ", "MP3", "MP4", "PS5", "PS4", "LEGO", "SUV", "CEO", "FAQ"};
    return a;
}

bool is_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(unsigned char c) { return c >= 'a' && c <= 'z'; }

// Many exclamation or question marks in a row: one of them. ("?!" stays, it says two things.)
std::string collapse_marks(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if ((s[i] == '!' || s[i] == '?') && !out.empty() && out.back() == s[i]) continue;
        out += s[i];
    }
    return out;
}

std::string collapse_spaces(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// Shouting: most of the letters are capitals, and there are enough of them to tell. Other alphabets don't count, and a
// title that is mostly another alphabet is left alone.
bool shouting(const std::string& s) {
    size_t upper = 0, lower = 0, other = 0;
    for (unsigned char c : s) {
        if (is_upper(c)) upper++;
        else if (is_lower(c)) lower++;
        else if (c >= 0x80) other++;
    }
    return upper + lower >= 8 && other * 3 < upper + lower + other && upper * 100 >= 80 * (upper + lower);
}

std::string sentence_case(const std::string& s) {
    std::string out;
    size_t i = 0;
    bool start = true;  // the start of a sentence
    while (i < s.size()) {
        if (!isalnum((unsigned char)s[i]) && (unsigned char)s[i] < 0x80) {
            if (s[i] == '.' || s[i] == '!' || s[i] == '?' || s[i] == ':') start = true;
            out += s[i++];
            continue;
        }
        if ((unsigned char)s[i] >= 0x80) {  // a character of another alphabet: as it is
            start = false;
            out += s[i++];
            continue;
        }
        size_t j = i;
        bool digit = false;
        while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '\'')) {
            digit |= isdigit((unsigned char)s[j]) != 0;
            j++;
        }
        const std::string word = s.substr(i, j - i);
        std::string up;
        for (char c : word) up += (char)toupper((unsigned char)c);
        const bool keep = digit || acronyms().count(up) || word == "I" || up == "I'M" || up == "I'LL" || up == "I'VE" || up == "I'D";
        for (size_t k = 0; k < word.size(); k++) {
            const unsigned char c = (unsigned char)word[k];
            if (keep) out += (char)c;
            else if (k == 0 && start) out += (char)toupper(c);
            else out += (char)tolower(c);
        }
        start = false;
        i = j;
    }
    return out;
}

}  // namespace

bool enabled() { return store::get_bool("yt_tidy_titles", true); }

std::string tidy(const std::string& title) {
    std::string t = collapse_spaces(collapse_marks(title));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (shouting(t)) t = sentence_case(t);
    return t;
}

std::string shown(const std::string& title) { return enabled() ? tidy(title) : title; }

}  // namespace title
