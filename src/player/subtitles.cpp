#include "player/subtitles.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "core/util.hpp"

namespace player {

namespace {

// "00:01:02,345" / "01:02.345" / "1:02:03.4"
bool parse_time(const std::string& s, double& out) {
    int parts[3] = {0, 0, 0};
    int n = 0;
    double frac = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        int v = 0;
        size_t start = i;
        while (i < s.size() && isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
        if (i == start) return false;
        parts[n++] = v;
        if (i < s.size() && s[i] == ':') {
            i++;
            continue;
        }
        if (i < s.size() && (s[i] == ',' || s[i] == '.')) {
            i++;
            double scale = 0.1;
            while (i < s.size() && isdigit((unsigned char)s[i])) {
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

std::string strip_tags(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<' || c == '{') tag = true;
        else if ((c == '>' || c == '}') && tag) tag = false;
        else if (!tag) out += c;
    }
    return util::html_to_text(out);
}

}  // namespace

void Subtitles::load(const std::string& data) {
    std::lock_guard<std::mutex> lk(m_);
    cues_.clear();
    std::istringstream in(data);
    std::string line;
    Cue cur{0, 0, ""};
    bool in_cue = false;
    auto finish = [&]() {
        if (in_cue && !cur.text.empty()) {
            while (!cur.text.empty() && cur.text.back() == '\n') cur.text.pop_back();
            cur.text = strip_tags(cur.text);
            cues_.push_back(cur);
        }
        in_cue = false;
        cur = Cue{0, 0, ""};
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);  // BOM
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            finish();
            double a, b;
            std::string l = util::trim(line.substr(0, arrow)), r = util::trim(line.substr(arrow + 3));
            size_t sp = r.find(' ');
            if (sp != std::string::npos) r = r.substr(0, sp);  // VTT cue settings
            if (parse_time(l, a) && parse_time(r, b)) {
                cur.start = a;
                cur.end = b;
                in_cue = true;
            }
            continue;
        }
        if (line.empty()) {
            finish();
            continue;
        }
        if (in_cue) cur.text += line + "\n";
    }
    finish();
    std::sort(cues_.begin(), cues_.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    sorted_ = true;
}

void Subtitles::add(double start, double end, const std::string& text) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& c : cues_)
        if (c.start == start && c.text == text) return;
    cues_.push_back(Cue{start, end, text});
    sorted_ = false;
}

void Subtitles::clear() {
    std::lock_guard<std::mutex> lk(m_);
    cues_.clear();
}

std::string Subtitles::at(double t) {
    std::lock_guard<std::mutex> lk(m_);
    if (!sorted_) {
        std::sort(cues_.begin(), cues_.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
        sorted_ = true;
    }
    auto it = std::upper_bound(cues_.begin(), cues_.end(), t, [](double v, const Cue& c) { return v < c.start; });
    std::string out;
    // Several cues can overlap; walk back a little.
    for (int k = 0; k < 4 && it != cues_.begin(); k++) {
        --it;
        if (t >= it->start && t <= it->end) out = out.empty() ? it->text : it->text + "\n" + out;
    }
    return out;
}

std::string Subtitles::strip_ass(const std::string& ass) {
    // "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (8 commas)
    size_t p = 0;
    for (int i = 0; i < 8 && p != std::string::npos; i++) {
        p = ass.find(',', p);
        if (p != std::string::npos) p++;
    }
    std::string text = p == std::string::npos ? ass : ass.substr(p);
    text = util::replace_all(text, "\\N", "\n");
    text = util::replace_all(text, "\\n", "\n");
    text = util::replace_all(text, "\\h", " ");
    return strip_tags(text);
}

}  // namespace player
