#include "services/hls.hpp"

#include <sstream>
#include <tuple>

#include "core/util.hpp"

namespace hls {

namespace {

// Attribute list: KEY=VALUE,KEY="quoted, value",...
std::string attr(const std::string& line, const char* key) {
    std::string k = std::string(key) + "=";
    size_t p = 0;
    while ((p = line.find(k, p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ',' || line[p - 1] == ':') break;
        p += k.size();
    }
    if (p == std::string::npos) return "";
    p += k.size();
    if (p < line.size() && line[p] == '"') {
        size_t e = line.find('"', p + 1);
        return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    size_t e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

}  // namespace

std::string resolve_url(const std::string& base, const std::string& ref) {
    if (util::starts_with(ref, "http://") || util::starts_with(ref, "https://")) return ref;
    if (ref.empty()) return base;
    size_t scheme = base.find("://");
    if (ref[0] == '/') {
        size_t host_end = base.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        return (host_end == std::string::npos ? base : base.substr(0, host_end)) + ref;
    }
    size_t q = base.find('?');
    std::string path = base.substr(0, q);
    size_t slash = path.find_last_of('/');
    return path.substr(0, slash + 1) + ref;
}

Master parse(const std::string& text, const std::string& base_url) {
    Master m;
    std::istringstream in(text);
    std::string line;
    Variant pending;
    bool have_pending = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (util::starts_with(line, "#EXT-X-STREAM-INF:")) {
            m.is_master = true;
            pending = Variant();
            pending.bandwidth = atoi(attr(line, "BANDWIDTH").c_str());
            std::string res = attr(line, "RESOLUTION");
            size_t x = res.find('x');
            if (x != std::string::npos) {
                pending.width = atoi(res.substr(0, x).c_str());
                pending.height = atoi(res.substr(x + 1).c_str());
            }
            pending.fps = (float)atof(attr(line, "FRAME-RATE").c_str());
            pending.codecs = attr(line, "CODECS");
            pending.audio_group = attr(line, "AUDIO");
            pending.name = attr(line, "VIDEO");
            have_pending = true;
        } else if (util::starts_with(line, "#EXT-X-MEDIA:")) {
            Rendition r;
            r.type = attr(line, "TYPE");
            r.group = attr(line, "GROUP-ID");
            r.name = attr(line, "NAME");
            r.language = attr(line, "LANGUAGE");
            r.is_default = attr(line, "DEFAULT") == "YES";
            std::string uri = attr(line, "URI");
            if (!uri.empty()) r.url = resolve_url(base_url, uri);
            m.renditions.push_back(r);
        } else if (line[0] != '#' && have_pending) {
            pending.url = resolve_url(base_url, line);
            m.variants.push_back(pending);
            have_pending = false;
        }
    }
    return m;
}

const Variant* pick(const Master& m, int max_height, bool avc_only, float max_fps) {
    const Variant* best = nullptr;
    const Variant* smallest = nullptr;
    auto rank = [&](const Variant& v) {
        // fps within the limit first, then resolution, then bitrate
        return std::make_tuple(v.fps <= max_fps + 0.5f || v.fps == 0 ? 1 : 0, v.height, v.bandwidth);
    };
    for (const Variant& v : m.variants) {
        bool avc = v.codecs.empty() || v.codecs.find("avc1") != std::string::npos;
        if (avc_only && !avc) continue;
        if (v.height == 0 && v.codecs.find("avc1") == std::string::npos) continue;  // audio-only
        if (!smallest || v.bandwidth < smallest->bandwidth) smallest = &v;
        if (v.height > max_height) continue;
        if (!best || rank(v) > rank(*best)) best = &v;
    }
    return best ? best : smallest;
}

const Rendition* audio_for(const Master& m, const Variant& v) {
    if (v.audio_group.empty()) return nullptr;
    const Rendition* any = nullptr;
    for (const Rendition& r : m.renditions) {
        if (r.type != "AUDIO" || r.group != v.audio_group || r.url.empty()) continue;
        if (r.is_default) return &r;
        if (!any) any = &r;
    }
    return any;
}

}  // namespace hls
