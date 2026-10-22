// "Less from other countries" for YouTube's feeds. The popular lists (trending, the topics, Discover) are the
// world's most viewed, so a lot of it is made for another country's audience: Hindi film music, Indian news, Thai
// variety shows. Not a language detector: scripts and a short list of telltale words and channel names, enough to
// push such videos down, and out of the popular lists when there's enough else. It steps aside for the viewer's own
// country (Settings > Country, the app's language) and for anything they watch themselves: a video they enjoyed
// teaches it that kind of content is wanted.
#pragma once

#include <string>
#include <vector>

#include "services/youtube.hpp"

namespace locale_bias {

// Settings > "Fewer videos from other countries" (on by default).
bool enabled();

// What marks a title and channel as made for another audience: "script:devanagari", "word:hindi",
// "channel:t-series". Empty for most videos.
std::vector<std::string> markers(const std::string& title, const std::string& channel);

// 0 to 0.9, how far to push this video down. 0 when it isn't marked, for the viewer's own country and for
// kinds of content they watch.
double penalty(const std::string& title, const std::string& channel);
inline double penalty(const youtube::Video& v) { return penalty(v.title, v.channel); }

// A video the viewer enjoyed: its kind of content is wanted, so it stops being pushed down.
void note_enjoyed(const std::string& title, const std::string& channel);
void reset();

// Takes the marked videos out of a popular list, keeping at least `keep` videos (the least marked ones are
// kept if there are too few others). The order of the rest stays.
void filter(std::vector<youtube::Video>& items, size_t keep = 8);

}  // namespace locale_bias
