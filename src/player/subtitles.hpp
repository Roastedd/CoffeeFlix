// Text subtitle cues (SRT/WebVTT files and embedded text streams).
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace player {

class Subtitles {
public:
    void load(const std::string& data);  // SRT or WebVTT
    void add(double start, double end, const std::string& text);
    void clear();
    std::string at(double t);
    static std::string strip_ass(const std::string& ass_line);

private:
    struct Cue {
        double start, end;
        std::string text;
    };
    std::mutex m_;
    std::vector<Cue> cues_;
    bool sorted_ = true;
};

}  // namespace player
