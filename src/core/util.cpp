#include "core/util.hpp"

#include <sys/stat.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <ctime>
#include <random>
#include <cerrno>
#include <cstdlib>

namespace util {

std::string url_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string html_to_text(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (size_t i = 0; i < html.size(); i++) {
        char c = html[i];
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        if (c == '<') {
            // Paragraph/line breaks become newlines.
            std::string_view rest = html.substr(i, 6);
            if (rest.substr(0, 3) == "<br" || rest.substr(0, 3) == "<p>" || rest.substr(0, 4) == "</p>" ||
                rest.substr(0, 3) == "<li")
                out += '\n';
            in_tag = true;
            continue;
        }
        if (c == '&') {
            size_t semi = html.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                std::string_view ent = html.substr(i + 1, semi - i - 1);
                uint32_t cp = 0;
                if (ent == "amp") cp = '&';
                else if (ent == "lt") cp = '<';
                else if (ent == "gt") cp = '>';
                else if (ent == "quot") cp = '"';
                else if (ent == "apos" || ent == "#39") cp = '\'';
                else if (ent == "nbsp") cp = ' ';
                else if (ent == "hellip") cp = 0x2026;
                else if (ent == "mdash") cp = 0x2014;
                else if (ent == "ndash") cp = 0x2013;
                else if (ent == "rsquo" || ent == "lsquo") cp = '\'';
                else if (ent == "ldquo" || ent == "rdquo") cp = '"';
                else if (!ent.empty() && ent[0] == '#') {
                    std::string num(ent.substr(1));
                    cp = (!num.empty() && (num[0] == 'x' || num[0] == 'X')) ? (uint32_t)strtoul(num.c_str() + 1, nullptr, 16)
                                                                             : (uint32_t)strtoul(num.c_str(), nullptr, 10);
                }
                if (cp) {
                    append_utf8(out, cp);
                    i = semi;
                    continue;
                }
            }
        }
        out += c;
    }
    // Collapse runs of whitespace, keep single newlines.
    std::string clean;
    clean.reserve(out.size());
    int newlines = 0;
    bool space = false;
    for (char c : out) {
        if (c == '\r' || c == '\t') c = ' ';
        if (c == '\n') {
            if (newlines < 2 && !clean.empty()) clean += '\n';
            newlines++;
            space = false;
            continue;
        }
        if (c == ' ') {
            space = true;
            continue;
        }
        if (space && !clean.empty() && clean.back() != '\n') clean += ' ';
        space = false;
        newlines = 0;
        clean += c;
    }
    return trim(clean);
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return std::string(s.substr(a, b - a));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

bool icontains(std::string_view h, std::string_view n) {
    if (n.empty()) return true;
    return lower(h).find(lower(n)) != std::string::npos;
}

bool natural_less(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) i2++;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) j2++;
            // Compare by value without overflowing: drop leading zeros, then length, then digits.
            std::string_view x = a.substr(i, i2 - i), y = b.substr(j, j2 - j);
            x.remove_prefix(std::min(x.find_first_not_of('0'), x.size()));
            y.remove_prefix(std::min(y.find_first_not_of('0'), y.size()));
            if (x.size() != y.size()) return x.size() < y.size();
            if (x != y) return x < y;
            i = i2;
            j = j2;
            continue;
        }
        char ca = (char)std::tolower((unsigned char)a[i]), cb = (char)std::tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string file_extension(std::string_view path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return "";
    return lower(path.substr(dot + 1));
}

std::string file_name(std::string_view path) {
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string parent_dir(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return "";
    if (slash == 0) return "/";
    return std::string(path.substr(0, slash));
}

std::string join_path(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    std::string out(a);
    if (out.back() != '/') out += '/';
    while (!b.empty() && b.front() == '/') b.remove_prefix(1);
    out.append(b);
    return out;
}

std::string format_duration(double seconds) {
    if (seconds < 0 || seconds != seconds) seconds = 0;
    long s = (long)seconds;
    long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, sec);
    else snprintf(buf, sizeof(buf), "%ld:%02ld", m, sec);
    return buf;
}

std::string format_count(int64_t n) {
    char buf[32];
    if (n >= 1000000000) snprintf(buf, sizeof(buf), "%.1fB", n / 1e9);
    else if (n >= 1000000) snprintf(buf, sizeof(buf), "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(buf, sizeof(buf), "%.0fK", n / 1e3);
    else if (n >= 1000) snprintf(buf, sizeof(buf), "%.1fK", n / 1e3);
    else snprintf(buf, sizeof(buf), "%lld", (long long)n);
    std::string s = buf;
    // "1.0M" -> "1M"
    size_t p = s.find(".0");
    if (p != std::string::npos && p + 2 < s.size() && !std::isdigit((unsigned char)s[p + 2])) s.erase(p, 2);
    return s;
}

std::string format_bytes(uint64_t n) {
    char buf[32];
    if (n >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1f GB", n / (double)(1ull << 30));
    else if (n >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.1f MB", n / (double)(1ull << 20));
    else if (n >= 1024) snprintf(buf, sizeof(buf), "%.0f KB", n / 1024.0);
    else snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    return buf;
}

std::string format_ticks_duration(int64_t ticks) {
    long minutes = (long)(ticks / 600000000LL);
    if (minutes <= 0) return "";
    char buf[32];
    if (minutes >= 60) snprintf(buf, sizeof(buf), "%ldh %ldm", minutes / 60, minutes % 60);
    else snprintf(buf, sizeof(buf), "%ldm", minutes);
    return buf;
}

std::string fmt(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < (int)sizeof(buf)) return std::string(buf, n > 0 ? n : 0);
    std::string big(n + 1, '\0');
    va_start(args, format);
    vsnprintf(big.data(), big.size(), format, args);
    va_end(args);
    big.resize(n);
    return big;
}

uint64_t hash64(std::string_view s, uint64_t h) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string random_hex(int bytes) {
    static std::mt19937_64 rng((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < bytes; i++) {
        unsigned v = (unsigned)(rng() & 0xFF);
        out += hex[v >> 4];
        out += hex[v & 15];
    }
    return out;
}

double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

int64_t unix_time() { return (int64_t)time(nullptr); }

std::string clock_hhmm() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    return buf;
}

std::string env_or(const char* name, const std::string& def) {
    const char* v = getenv(name);
    return v && *v ? std::string(v) : def;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dirs(const std::string& path) {
    if (path.empty() || dir_exists(path)) return true;
    std::string parent = parent_dir(path);
    if (!parent.empty() && parent != path && !dir_exists(parent)) make_dirs(parent);
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n <= 0 || fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

bool write_file_atomic(const std::string& path, std::string_view data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str());  // rename over an existing file isn't atomic on FAT
    return rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace util
