// Minimal HLS master playlist parsing (variant selection for Twitch/YouTube).
#pragma once

#include <string>
#include <vector>

namespace hls {

struct Variant {
    std::string url;
    int bandwidth = 0;
    int width = 0, height = 0;
    float fps = 0;
    std::string codecs;
    std::string audio_group;
    std::string name;  // Twitch "VIDEO" attribute / NAME of the stream
};

struct Rendition {
    std::string type, group, name, language, url;
    bool is_default = false;
};

struct Master {
    std::vector<Variant> variants;
    std::vector<Rendition> renditions;
    bool is_master = false;
};

Master parse(const std::string& text, const std::string& base_url);
std::string resolve_url(const std::string& base, const std::string& ref);
// Best variant not above max_height (H.264 only when avc_only). Variants above max_fps (of
// their height) are only chosen if nothing else fits. nullptr if none.
const Variant* pick(const Master& m, int max_height, bool avc_only = true, float (*max_fps)(int height) = nullptr);
const Rendition* audio_for(const Master& m, const Variant& v);

}  // namespace hls
