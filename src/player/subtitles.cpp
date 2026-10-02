#include "player/subtitles.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/util.hpp"

namespace player {

namespace {

constexpr size_t kBlock = 32;               // cues per block of the end-time index
constexpr size_t kFresh = (size_t)-1;       // stale_from_ when the index is current
constexpr uint32_t kFileSeq = 0xFFFFFFFFu;  // load()'s cues
constexpr size_t npos = std::string_view::npos;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
char to_lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }

// `lower` is lower-case.
bool istarts(std::string_view s, std::string_view lower) {
    if (s.size() < lower.size()) return false;
    for (size_t i = 0; i < lower.size(); i++)
        if (to_lower(s[i]) != lower[i]) return false;
    return true;
}
bool iequals(std::string_view s, std::string_view lower) { return s.size() == lower.size() && istarts(s, lower); }

std::string_view trim_view(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool has_bom(std::string_view s) {
    return s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF;
}

// "00:01:02,345" / "01:02.345" / "1:02:03.4" / ASS "0:01:02.34"
bool parse_time(std::string_view s, double& out) {
    int parts[3] = {0, 0, 0};
    int n = 0;
    double frac = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        int v = 0;
        size_t start = i;
        while (i < s.size() && is_digit(s[i])) {
            if (v < 100000000) v = v * 10 + (s[i] - '0');
            i++;
        }
        if (i == start) return false;
        parts[n++] = v;
        if (i < s.size() && s[i] == ':') {
            i++;
            continue;
        }
        if (i < s.size() && (s[i] == ',' || s[i] == '.')) {
            i++;
            double scale = 0.1;
            while (i < s.size() && is_digit(s[i])) {
                frac += (s[i++] - '0') * scale;
                scale *= 0.1;
            }
        }
        break;
    }
    if (n == 3) out = parts[0] * 3600.0 + parts[1] * 60.0 + parts[2] + frac;
    else if (n == 2) out = parts[0] * 60.0 + parts[1] + frac;
    else return false;
    return true;
}

// ---- Text encoding ----

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Length of the well-formed UTF-8 sequence starting at s[i] (a byte >= 0x80), or 0.
size_t utf8_length(std::string_view s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!n || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    unsigned char c1 = (unsigned char)s[i + 1];
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
        return 0;  // overlong, surrogate or past U+10FFFF
    return n;
}

// Windows-1252's 0x80-0x9F; the rest of the high half is Latin-1.
const uint16_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

// The file as UTF-8 without a BOM: a view of `data` when it already is, else converted into
// `storage`. Bytes that aren't valid UTF-8 are read as Windows-1252 (old SRT/SSA files).
std::string_view as_utf8(const std::string& data, std::string& storage) {
    const unsigned char* b = (const unsigned char*)data.data();
    const size_t n = data.size();
    int utf16 = 0;  // 1 little-endian, 2 big-endian
    size_t i = 0;
    if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        utf16 = b[0] == 0xFF ? 1 : 2;
        i = 2;
    } else if (n >= 4 && b[0] && !b[1] && b[2] && !b[3]) utf16 = 1;  // ASCII text as UTF-16 without a BOM
    else if (n >= 4 && !b[0] && b[1] && !b[2] && b[3]) utf16 = 2;
    if (utf16) {
        auto unit = [&](size_t k) -> uint32_t { return utf16 == 1 ? b[k] | (b[k + 1] << 8) : (b[k] << 8) | b[k + 1]; };
        storage.clear();
        storage.reserve(n + n / 2);
        for (; i + 1 < n; i += 2) {
            uint32_t u = unit(i);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n && unit(i + 2) >= 0xDC00 && unit(i + 2) <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (unit(i + 2) - 0xDC00);
                i += 2;
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            if (u) put_utf8(storage, u);
        }
        return storage;
    }
    std::string_view s(data);
    if (has_bom(s)) s.remove_prefix(3);
    size_t k = 0;
    while (k < s.size()) {
        size_t len = (unsigned char)s[k] < 0x80 ? 1 : utf8_length(s, k);
        if (!len) break;
        k += len;
    }
    if (k == s.size()) return s;
    storage.assign(s.data(), k);
    storage.reserve(s.size() + s.size() / 4);
    while (k < s.size()) {
        unsigned char c = (unsigned char)s[k];
        size_t len = c < 0x80 ? 1 : utf8_length(s, k);
        if (len) {
            storage.append(s.data() + k, len);
            k += len;
        } else {
            put_utf8(storage, c < 0xA0 ? kCp1252[c - 0x80] : c);
            k++;
        }
    }
    return storage;
}

// Lines ending in LF, CRLF, CR, or CR CR LF (a common double conversion).
struct Lines {
    std::string_view s;
    size_t pos = 0;
    bool next(std::string_view& line) {
        if (pos >= s.size()) return false;
        size_t e = pos;
        while (e < s.size() && s[e] != '\n' && s[e] != '\r') e++;
        line = s.substr(pos, e - pos);
        if (e + 1 < s.size() && s[e] == '\r') {
            if (s[e + 1] == '\n') e++;
            else if (s[e + 1] == '\r' && e + 2 < s.size() && s[e + 2] == '\n') e += 2;
        }
        pos = e + 1;
        return true;
    }
};

// ---- Cue text ----

// If s[i] ('<') opens an HTML-style tag, the index of its '>'; else npos, and the '<' is
// text ("I <3 you"). `known_only` accepts just the formatting tags converters leave.
size_t html_tag_end(std::string_view s, size_t i, bool known_only) {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '/') j++;
    if (j >= s.size()) return npos;
    if (is_digit(s[j])) {  // WebVTT timestamp: <00:00:01.500>
        size_t k = j;
        while (k < s.size() && (is_digit(s[k]) || s[k] == ':' || s[k] == '.')) k++;
        return k < s.size() && s[k] == '>' ? k : npos;
    }
    if (!is_alpha(s[j]) && !(s[j] == '!' && !known_only)) return npos;
    size_t name_end = j + 1;
    while (name_end < s.size() && is_alpha(s[name_end])) name_end++;
    if (known_only) {
        static const char* const kKnown[] = {"i", "b", "u", "s", "font", "br", "c", "v", "span", "ruby", "rt", "rp", "lang"};
        std::string_view name = s.substr(j, name_end - j);
        bool known = false;
        for (const char* k : kKnown) known = known || iequals(name, k);
        if (!known) return npos;
    }
    for (size_t k = name_end; k < s.size() && k - i < 256; k++) {
        if (s[k] == '>') return k;
        if (s[k] == '<' || s[k] == '\n') return npos;
    }
    return npos;
}

// Trims lines, collapses spaces, drops empty lines.
struct TextSink {
    std::string s;
    bool line = false, space = false, entity = false;
    explicit TextSink(size_t n) { s.reserve(n); }
    void put(char c) {
        if (c == '\n') {
            if (line) s += '\n';
            line = space = false;
        } else if (is_space(c)) {
            space = line;
        } else {
            if (space) s += ' ';
            space = false;
            line = true;
            entity = entity || c == '&';
            s += c;
        }
    }
    std::string take() {
        if (!s.empty() && s.back() == '\n') s.pop_back();
        if (entity && !s.empty()) {  // "&amp;", "&#39;": rare, so the slower general decoder
            s = util::replace_all(std::move(s), "<", "&lt;");  // literal by now, not tags
            s = util::html_to_text(s);
        }
        return std::move(s);
    }
};

// SubRip/WebVTT: HTML tags, ASS override tags some SRTs carry ({\an8}), entities.
std::string strip_tags(std::string_view s) {
    TextSink out(s.size());
    size_t stop = 0;  // the next '}' or line break, looked up again only once passed
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '{' && i + 1 < s.size() && s[i + 1] == '\\') {
            if (stop != npos && stop <= i) stop = s.find_first_of("}\n", i + 1);
            size_t e = stop;
            if (e != npos && s[e] == '}') {
                i = e;
                continue;
            }
        } else if (c == '<') {
            size_t e = html_tag_end(s, i, false);
            if (e != npos) {
                i = e;
                continue;
            }
        }
        out.put(c);
    }
    return out.take();
}

// The tags of one override block that matter here: \pN (N > 0 starts a vector drawing,
// \p0 ends it) and \pos / \move (typesetting placed on the picture).
void read_override(std::string_view block, int& drawing, bool& positioned) {
    for (size_t j = 0; j + 1 < block.size(); j++) {
        if (block[j] != '\\') continue;
        std::string_view tag = block.substr(j + 1);
        if (tag.size() > 1 && tag[0] == 'p' && is_digit(tag[1])) {
            int v = 0;
            for (size_t k = 1; k < tag.size() && is_digit(tag[k]); k++)
                if (v < 1000) v = v * 10 + (tag[k] - '0');
            drawing = v;
        } else if (tag.substr(0, 4) == "pos(" || tag.substr(0, 5) == "move(") {
            positioned = true;
        }
    }
}

// The Text field of an ASS event as plain lines.
std::string ass_text(std::string_view in, bool* sign) {
    TextSink out(in.size());
    int drawing = 0;
    bool positioned = false;
    size_t close = 0;  // the next '}', looked up again only once passed
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '{') {
            // A block runs to the next '}'. An unclosed '{' is text, unless it starts a tag.
            if (close != npos && close <= i) close = in.find('}', i + 1);
            size_t e = close;
            if (e == npos && i + 1 < in.size() && in[i + 1] == '\\') e = in.size();
            if (e != npos) {
                read_override(in.substr(i + 1, e - i - 1), drawing, positioned);
                i = e;
                continue;
            }
        }
        if (drawing > 0) continue;  // "m 0 0 l 100 0 ..." is a shape, not words
        if (c == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 'N' || n == 'n' || n == 'h') {
                out.put(n == 'h' ? ' ' : '\n');
                i++;
                continue;
            }
        }
        if (c == '<') {  // tags from converted SubRip/WebVTT
            size_t e = html_tag_end(in, i, true);
            if (e != npos) {
                i = e;
                continue;
            }
        }
        out.put(c);
    }
    if (sign) *sign = positioned;
    return out.take();
}

bool looks_like_ass(std::string_view s) {
    Lines lines{s};
    std::string_view line;
    while (lines.next(line)) {
        line = trim_view(line);
        if (has_bom(line)) line = trim_view(line.substr(3));
        if (line.empty()) continue;
        return line[0] == '[' || istarts(line, "dialogue:");  // "[Script Info]", or bare events
    }
    return false;
}

bool icontains_dialogue(std::string_view s) {
    for (size_t i = s.find(':'); i != npos; i = s.find(':', i + 1))
        if (i >= 8 && istarts(s.substr(i - 8), "dialogue")) return true;
    return false;
}

}  // namespace

void Subtitles::parse_srt(std::string_view data, std::vector<Cue>& out) {
    Lines lines{data};
    std::string_view line;
    std::string text;
    double start = 0, end = 0;
    bool in_cue = false;
    auto finish = [&]() {
        if (in_cue) {
            std::string t = strip_tags(text);
            if (!t.empty()) out.push_back(Cue{start, end, std::move(t), kFileSeq, false});
        }
        in_cue = false;
        text.clear();
    };
    while (lines.next(line)) {
        if (has_bom(line)) line.remove_prefix(3);  // concatenated files
        size_t arrow = line.find("-->");
        if (arrow != npos) {
            double a, b;
            std::string_view l = trim_view(line.substr(0, arrow)), r = trim_view(line.substr(arrow + 3));
            size_t sp = r.find_first_of(" \t");
            if (sp != npos) r = r.substr(0, sp);  // VTT cue settings, SRT coordinates
            if (parse_time(l, a) && parse_time(r, b)) {
                finish();
                start = a;
                end = b;
                in_cue = true;
                continue;
            }
        }
        if (line.empty()) {
            finish();
            continue;
        }
        if (in_cue && !trim_view(line).empty()) {  // YouTube pads cues with " " lines
            if (!text.empty()) text += '\n';
            text.append(line.data(), line.size());
        }
    }
    finish();
}

void Subtitles::parse_ass(std::string_view data, std::vector<Cue>& out) {
    enum { NONE, EVENTS, OTHER, BINARY } section = NONE;
    // Columns of [Events]; without a Format line, the v4+/v4 order: Layer (or Marked), Start,
    // End, Style, Name, MarginL, MarginR, MarginV, Effect, Text.
    int columns = 10, col_start = 1, col_end = 2, col_text = 9;
    Lines lines{data};
    std::string_view line;
    while (lines.next(line)) {
        line = trim_view(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            std::string_view name = trim_view(line.substr(1, close == npos ? npos : close - 1));
            section = iequals(name, "events")                            ? EVENTS
                      : iequals(name, "fonts") || iequals(name, "graphics") ? BINARY
                                                                          : OTHER;
            continue;
        }
        if (section == BINARY) continue;  // embedded font data
        if (istarts(line, "format:")) {
            if (section != EVENTS && section != NONE) continue;  // [V4+ Styles] has its own
            int n = 0, s = -1, e = -1, t = -1;
            std::string_view spec = line.substr(7);
            for (size_t p = 0;;) {
                size_t comma = spec.find(',', p);
                std::string_view name = trim_view(spec.substr(p, comma == npos ? npos : comma - p));
                if (iequals(name, "start")) s = n;
                else if (iequals(name, "end")) e = n;
                else if (iequals(name, "text")) t = n;
                n++;
                if (comma == npos || n > 64) break;
                p = comma + 1;
            }
            if (s >= 0 && e >= 0 && t >= 0 && n <= 64) {
                columns = n;
                col_start = s;
                col_end = e;
                col_text = t;
            }
            continue;
        }
        if (!istarts(line, "dialogue:")) continue;  // Comment:, Style:, Picture:, ...
        std::string_view rest = trim_view(line.substr(9)), f_start, f_end, f_text;
        bool ok = true;
        size_t p = 0;
        for (int k = 0; k < columns; k++) {
            std::string_view f;
            if (k == columns - 1) {
                f = rest.substr(p);  // the last column (Text) keeps its commas
            } else {
                size_t comma = rest.find(',', p);
                if (comma == npos) {
                    ok = false;
                    break;
                }
                f = rest.substr(p, comma - p);
                p = comma + 1;
            }
            if (k == col_start) f_start = f;
            if (k == col_end) f_end = f;
            if (k == col_text) f_text = f;
        }
        double a, b;
        if (!ok || !parse_time(trim_view(f_start), a) || !parse_time(trim_view(f_end), b) || b < a) continue;
        bool sign = false;
        std::string text = ass_text(f_text, &sign);
        if (!text.empty()) out.push_back(Cue{a, b, std::move(text), kFileSeq, sign});
    }
}

void Subtitles::load(const std::string& data) {
    // Parsed without the lock; only the exchange happens under it.
    std::string storage;
    std::string_view s = as_utf8(data, storage);
    std::vector<Cue> cues;
    if (looks_like_ass(s)) {
        parse_ass(s, cues);
    } else {
        parse_srt(s, cues);
        if (cues.empty() && icontains_dialogue(s)) parse_ass(s, cues);
    }
    auto by_start = [](const Cue& a, const Cue& b) { return a.start < b.start; };
    if (!std::is_sorted(cues.begin(), cues.end(), by_start)) std::stable_sort(cues.begin(), cues.end(), by_start);
    std::lock_guard<std::mutex> lk(m_);
    cues_.swap(cues);  // the old cues are freed after the lock is released
    added_ = 0;
    next_seq_ = 0;
    stale_from_ = 0;
}

void Subtitles::add(double start, double end, const std::string& text, bool sign) {
    if (text.empty() || !std::isfinite(start) || !std::isfinite(end)) return;
    std::lock_guard<std::mutex> lk(m_);
    auto it = std::lower_bound(cues_.begin(), cues_.end(), start, [](const Cue& c, double v) { return c.start < v; });
    for (; it != cues_.end() && it->start == start; ++it)
        if (it->text == text) return;
    size_t at = it - cues_.begin();
    cues_.insert(it, Cue{start, end, text, next_seq_++, sign});
    stale_from_ = std::min(stale_from_, at);
    if (++added_ > kMaxAdded) evict_locked();
}

// Drops the oldest-added eighth. In normal playback those are the earliest cues; after a
// seek back, the ones just re-added for the new position stay.
void Subtitles::evict_locked() {
    const uint32_t keep_from = next_seq_ - (uint32_t)(kMaxAdded - kMaxAdded / 8);
    cues_.erase(std::remove_if(cues_.begin(), cues_.end(),
                               [keep_from](const Cue& c) { return c.seq != kFileSeq && c.seq < keep_from; }),
                cues_.end());
    added_ = (size_t)std::count_if(cues_.begin(), cues_.end(), [](const Cue& c) { return c.seq != kFileSeq; });
    stale_from_ = 0;
}

void Subtitles::clear() {
    std::vector<Cue> old;
    std::lock_guard<std::mutex> lk(m_);
    old.swap(cues_);
    block_end_.clear();
    block_end_prefix_.clear();
    stale_from_ = kFresh;
    added_ = 0;
    next_seq_ = 0;
}

void Subtitles::swap(Subtitles& other) {
    if (this == &other) return;
    std::scoped_lock lk(m_, other.m_);
    cues_.swap(other.cues_);
    block_end_.swap(other.block_end_);
    block_end_prefix_.swap(other.block_end_prefix_);
    std::swap(stale_from_, other.stale_from_);
    std::swap(added_, other.added_);
    std::swap(next_seq_, other.next_seq_);
}

size_t Subtitles::size() {
    std::lock_guard<std::mutex> lk(m_);
    return cues_.size();
}

void Subtitles::reindex_locked() {
    const size_t n = cues_.size(), blocks = (n + kBlock - 1) / kBlock;
    block_end_.resize(blocks);
    block_end_prefix_.resize(blocks);
    for (size_t b = std::min(stale_from_, n) / kBlock; b < blocks; b++) {
        double m = std::numeric_limits<double>::lowest();
        for (size_t i = b * kBlock, e = std::min(n, i + kBlock); i < e; i++) m = std::max(m, cues_[i].end);
        block_end_[b] = m;
        block_end_prefix_[b] = b ? std::max(block_end_prefix_[b - 1], m) : m;
    }
    stale_from_ = kFresh;
}

std::string Subtitles::at(double t) {
    std::lock_guard<std::mutex> lk(m_);
    if (cues_.empty() || !(t == t)) return "";
    if (stale_from_ != kFresh) reindex_locked();
    // Cues [0, i) started by t. Walk back over those still showing, newest first, skipping
    // blocks that all ended before t; a long line that began far back is still reached.
    size_t i = std::upper_bound(cues_.begin(), cues_.end(), t, [](double v, const Cue& c) { return v < c.start; }) -
               cues_.begin();
    size_t hits[kMaxCandidates];
    int found = 0;
    while (i > 0 && found < kMaxCandidates) {
        size_t b = (i - 1) / kBlock, first = b * kBlock;
        if (block_end_prefix_[b] < t) break;
        if (block_end_[b] < t) {
            i = first;
            continue;
        }
        while (i > first && found < kMaxCandidates)
            if (cues_[--i].end >= t) hits[found++] = i;
    }
    // Dialogue first, then signs; newest first; identical text once (layered typesetting).
    size_t chosen[kMaxLines];
    int count = 0;
    for (int signs = 0; signs < 2; signs++)
        for (int k = 0; k < found && count < kMaxLines; k++) {
            const Cue& c = cues_[hits[k]];
            if (c.sign != (signs == 1)) continue;
            bool repeat = false;
            for (int j = 0; j < count && !repeat; j++) repeat = cues_[chosen[j]].text == c.text;
            if (!repeat) chosen[count++] = hits[k];
        }
    std::sort(chosen, chosen + count);
    std::string out;
    for (int j = 0; j < count; j++) {
        if (j) out += '\n';
        out += cues_[chosen[j]].text;
    }
    return out;
}

std::string Subtitles::strip_ass(const std::string& ass, bool* sign) {
    // Decoded packets normally omit timestamps (8 fields before Text). Some
    // embedded SSA/ASS packets retain the full Dialogue event (9 fields).
    // Removing punctuation from Text itself would corrupt legitimate dialogue.
    std::string_view v(ass);
    const bool dialogue = istarts(trim_view(v), "dialogue:");
    size_t p = 0;
    for (int i = 0; i < (dialogue ? 9 : 8) && p != npos; i++) {
        p = v.find(',', p);
        if (p != npos) p++;
    }
    return ass_text(p == npos ? v : v.substr(p), sign);
}

}  // namespace player
