// Text subtitle cues (SRT/WebVTT/ASS/SSA files and embedded text streams).
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace player {

class Subtitles {
public:
    // Replaces the cues with a file's: SubRip, WebVTT or ASS/SSA, told apart by content.
    // UTF-8 (with or without BOM), UTF-16 with a BOM, or stray Windows-1252 bytes.
    void load(const std::string& data);
    // One cue of an embedded stream; a repeat (same start and text) is ignored. Only the
    // newest kMaxAdded of these are kept. `sign` marks positioned typesetting (\pos/\move),
    // shown only when there's room next to dialogue.
    void add(double start, double end, const std::string& text, bool sign = false);
    void clear();
    // Exchanges the cues with `other`, so a file can be parsed without holding a caller's lock.
    void swap(Subtitles& other);
    size_t size();
    // What to show at `t`: up to kMaxLines distinct cues, the most recently started first
    // (dialogue before signs), joined by newlines in start order.
    std::string at(double t);
    // The text of a decoded ASS event: 8 fields before Text, or 9 for a full "Dialogue:"
    // line. Override blocks are removed; vector drawings give "". `sign`, when given, is
    // set for positioned events.
    static std::string strip_ass(const std::string& ass_line, bool* sign = nullptr);

    static constexpr size_t kMaxAdded = 20000;
    static constexpr int kMaxLines = 3;        // the player shows three lines
    static constexpr int kMaxCandidates = 32;  // showing cues looked at per at()

private:
    struct Cue {
        double start, end;
        std::string text;
        uint32_t seq;  // order of add(); files' cues use UINT32_MAX and are never dropped
        bool sign;
    };
    static void parse_srt(std::string_view data, std::vector<Cue>& out);
    static void parse_ass(std::string_view data, std::vector<Cue>& out);
    void reindex_locked();
    void evict_locked();

    std::mutex m_;
    std::vector<Cue> cues_;  // always sorted by start; equal starts keep their order
    // The latest end in each block of cues, and over all blocks up to it: at() skips blocks
    // that finished before `t` and stops once nothing earlier can still be showing.
    std::vector<double> block_end_, block_end_prefix_;
    size_t stale_from_ = (size_t)-1;  // first cue whose block data is out of date
    size_t added_ = 0;
    uint32_t next_seq_ = 0;
};

}  // namespace player
